#include "services/telegram/tdlib_account_protocol.hpp"
#include "domain/telegram_account_model.hpp"
#include "services/telegram/telegram_client.hpp"

#include <algorithm>
#include <cassert>
#include <deque>
#include <iostream>
#include <string>
#include <utility>

namespace {
using namespace telegram;
using Json = support::json::Value;

Json parse(const char* value) { return support::json::parse(value); }

struct Harness {
    struct Wire { Json value; std::uint64_t id; };
    std::deque<Wire> sent;
    std::vector<AccountUpdate> updates;
    std::int64_t now = 0;
    TdlibAccountProtocol protocol;

    Harness() : protocol(
        [this](Json value, std::uint64_t id) { sent.push_back({std::move(value), id}); return true; },
        [this](AccountUpdate update) { updates.push_back(std::move(update)); },
        [](const Json& value) {
            Chat chat;
            chat.id = value.at("id").as_int64();
            chat.title = value.at("title").as_string();
            return chat;
        }, [this] { return now; }) {}

    void submit(AccountAction action, std::uint64_t id = 1)
    {
        AccountRequest request;
        request.action = action;
        request.request_id = id;
        protocol.submit(std::move(request));
    }

    Wire pop(const char* type)
    {
        assert(!sent.empty());
        auto request = std::move(sent.front());
        sent.pop_front();
        assert(request.value.at("@type").as_string() == type);
        return request;
    }

    void reply(const char* request_type, const char* result)
    {
        auto request = pop(request_type);
        protocol.response(request.id, parse(result));
    }

    std::size_t finished(std::uint64_t id = 1) const
    {
        return std::count_if(updates.begin(), updates.end(), [&](const auto& update) {
            return update.kind == AccountUpdateKind::RequestFinished && update.request_id == id;
        });
    }

    const AccountUpdate& last(AccountUpdateKind kind) const
    {
        const auto found = std::find_if(updates.rbegin(), updates.rend(), [&](const auto& update) { return update.kind == kind; });
        assert(found != updates.rend());
        return *found;
    }
};

const char* user_json = R"({"@type":"user","id":42,"first_name":"Alice","last_name":"Example","phone_number":"12345","is_contact":true,"usernames":{"editable_username":"alice","active_usernames":["collectible","alice"]},"status":{"@type":"userStatusOnline"}})";
const char* notification_json = R"({"@type":"scopeNotificationSettings","mute_for":3600,"sound_id":"9007199254740993","show_preview":true,"use_default_mute_stories":true,"mute_stories":false,"story_sound_id":"9876543210123456","show_story_poster":true,"disable_pinned_message_notifications":false,"disable_mention_notifications":true})";

void test_profile_waits_for_full_info_and_refresh()
{
    Harness h;
    h.submit(AccountAction::LoadProfile);
    h.reply("getMe", user_json);
    assert(h.finished() == 0);
    const auto full = h.pop("getUserFullInfo");
    assert(full.value.at("user_id").as_int64() == 42);
    h.protocol.response(full.id, parse(R"({"@type":"userFullInfo","bio":{"@type":"formattedText","text":"Bio","entities":[]}})"));
    assert(h.last(AccountUpdateKind::Profile).profile.user.username == "alice");
    assert(h.last(AccountUpdateKind::Profile).profile.bio == "Bio");
    assert(h.finished() == 1);

    AccountRequest edit;
    edit.action = AccountAction::SetName;
    edit.request_id = 2;
    edit.profile.user.first_name = "Changed";
    edit.profile.user.last_name = "Name";
    h.protocol.submit(edit);
    const auto change = h.pop("setName");
    assert(change.value.at("first_name").as_string() == "Changed");
    h.protocol.response(change.id, parse(R"({"@type":"ok"})"));
    assert(h.finished(2) == 0);
    h.reply("getMe", user_json);
    h.reply("getUserFullInfo", R"({"@type":"userFullInfo","bio":null})");
    assert(h.finished(2) == 1);
    assert(h.last(AccountUpdateKind::Profile).profile.bio.empty());
}

void test_contacts_cache_fallback_status_and_limit()
{
    Harness h;
    h.protocol.observe(Json(Json::Object{{"@type", Json("updateUser")}, {"user", parse(user_json)}}));
    h.submit(AccountAction::LoadContacts);
    h.reply("getContacts", R"({"@type":"users","total_count":2,"user_ids":[42,43]})");
    assert(h.finished() == 0);
    const auto fallback = h.pop("getUser");
    assert(fallback.value.at("user_id").as_int64() == 43);
    h.protocol.response(fallback.id, parse(R"({"@type":"user","id":43,"first_name":"Bob","is_contact":true})"));
    assert(h.finished() == 1);
    const auto& contacts = h.last(AccountUpdateKind::Contacts).contacts;
    assert(contacts.size() == 2 && contacts[0].is_online && contacts[1].first_name == "Bob");
    h.protocol.observe(parse(R"({"@type":"updateUserStatus","user_id":42,"status":{"@type":"userStatusLastWeek"}})"));
    assert(!h.last(AccountUpdateKind::ContactChanged).contact.is_online);
    assert(h.last(AccountUpdateKind::ContactChanged).contact.status == "last seen within a week");

    Json::Array ids;
    for (std::int64_t id = 1; id <= 550; ++id) {
        Json user = parse(user_json);
        user.as_object()["id"] = Json(id);
        h.protocol.observe(Json(Json::Object{{"@type", Json("updateUser")}, {"user", user}}));
        ids.emplace_back(id);
    }
    h.submit(AccountAction::LoadContacts, 2);
    const auto loaded = h.pop("getContacts");
    h.protocol.response(loaded.id, Json(Json::Object{{"@type", Json("users")}, {"total_count", Json(550)}, {"user_ids", Json(std::move(ids))}}));
    assert(h.last(AccountUpdateKind::Contacts).contacts.size() == kMaxContacts);
    assert(h.last(AccountUpdateKind::Contacts).total_count == 550);
    assert(h.finished(2) == 1);
}

void test_private_chat_without_main_list_and_saved_messages()
{
    Harness h;
    AccountRequest request;
    request.action = AccountAction::OpenContact;
    request.request_id = 1;
    request.target_id = 42;
    h.protocol.submit(request);
    const auto open = h.pop("createPrivateChat");
    assert(!open.value.at("force").as_bool());
    assert(open.value.at("user_id").as_int64() == 42);
    h.protocol.response(open.id, parse(R"({"@type":"chat","id":42,"title":"Alice","positions":[]})"));
    assert(h.last(AccountUpdateKind::PrivateChatReady).chat.order == 0);
    assert(h.finished() == 1);

    h.submit(AccountAction::OpenSavedMessages, 2);
    h.reply("getMe", user_json);
    h.reply("createPrivateChat", R"({"@type":"chat","id":42,"title":"Alice","positions":[]})");
    assert(h.last(AccountUpdateKind::PrivateChatReady).chat.title == "Saved Messages");
    assert(h.finished(2) == 1);
}

void test_contact_import_schema_and_unregistered_failure()
{
    Harness h;
    AccountRequest request;
    request.action = AccountAction::AddContact;
    request.request_id = 1;
    request.contact.first_name = "Contact";
    request.contact.phone_number = "+12025550123";
    h.protocol.submit(request);
    const auto wire = h.pop("importContacts");
    const auto& item = wire.value.at("contacts").at(0);
    assert(item.at("@type").as_string() == "importedContact");
    assert(item.at("note").is_null());
    assert(!item.contains("vcard") && !item.contains("user_id"));
    h.protocol.response(wire.id, parse(R"({"@type":"importedContacts","user_ids":[0],"importer_count":[0]})"));
    assert(h.finished() == 1 && !h.last(AccountUpdateKind::RequestFinished).success);
    assert(h.sent.empty());
    request.request_id = 2;
    h.protocol.submit(request);
    h.reply("importContacts", R"({"@type":"importedContacts","user_ids":[42],"importer_count":[0]})");
    assert(h.finished(2) == 0);
    h.reply("getContacts", R"({"@type":"users","total_count":0,"user_ids":[]})");
    assert(h.finished(2) == 1);
}

void test_notification_preservation_and_group_completion()
{
    Harness h;
    AccountRequest set;
    set.action = AccountAction::SetNotifications;
    set.request_id = 10;
    h.protocol.submit(set);
    assert(h.sent.empty());
    assert(!h.last(AccountUpdateKind::RequestFinished).success);

    h.submit(AccountAction::LoadNotifications);
    h.reply("getScopeNotificationSettings", notification_json);
    h.reply("getScopeNotificationSettings", notification_json);
    assert(h.finished() == 0);
    h.reply("getScopeNotificationSettings", notification_json);
    assert(h.finished() == 1);

    set.request_id = 2;
    set.notifications = {true, false, false, true};
    h.now = 120;
    h.protocol.submit(set);
    const auto wire = h.pop("setScopeNotificationSettings");
    const auto& settings = wire.value.at("notification_settings");
    assert(settings.at("mute_for").as_int64() == 3480);
    assert(settings.at("sound_id").as_string() == "9007199254740993");
    assert(settings.at("story_sound_id").as_string() == "9876543210123456");
    assert(settings.at("disable_mention_notifications").as_bool());
    assert(!settings.at("show_preview").as_bool());
    h.protocol.response(wire.id, parse(R"({"@type":"ok"})"));
    assert(h.finished(2) == 0);
    h.reply("getScopeNotificationSettings", notification_json);
    assert(h.finished(2) == 1);
}

void test_privacy_preserves_exceptions_and_failure_once()
{
    Harness h;
    const auto raw = R"({"@type":"userPrivacySettingRules","rules":[{"@type":"userPrivacySettingRuleRestrictUsers","user_ids":[123,456]},{"@type":"userPrivacySettingRuleAllowChatMembers","chat_ids":[-999]},{"@type":"userPrivacySettingRuleAllowContacts"},{"@type":"userPrivacySettingRuleRestrictAll"}]})";
    h.submit(AccountAction::LoadPrivacy);
    h.reply("getUserPrivacySettingRules", raw);
    assert(h.last(AccountUpdateKind::Privacy).privacy.exception_count == 3);
    h.reply("getUserPrivacySettingRules", raw);
    h.reply("getUserPrivacySettingRules", raw);
    assert(h.finished() == 1);
    AccountRequest set;
    set.action = AccountAction::SetPrivacy;
    set.request_id = 2;
    set.visibility = PrivacyVisibility::Nobody;
    h.protocol.submit(set);
    const auto wire = h.pop("setUserPrivacySettingRules");
    const auto& rules = wire.value.at("rules").at("rules").as_array();
    assert(rules.size() == 3);
    assert(rules[0] == parse(raw).at("rules").at(0));
    assert(rules[1] == parse(raw).at("rules").at(1));
    assert(rules[2].at("@type").as_string() == "userPrivacySettingRuleRestrictAll");
    h.protocol.response(wire.id, parse(R"({"@type":"ok"})"));
    h.reply("getUserPrivacySettingRules", raw);
    assert(h.finished(2) == 1);

    h.submit(AccountAction::LoadPrivacy, 3);
    auto first = h.pop("getUserPrivacySettingRules");
    auto second = h.pop("getUserPrivacySettingRules");
    auto third = h.pop("getUserPrivacySettingRules");
    h.protocol.response(second.id, parse(R"({"@type":"error","code":406,"message":"SENSITIVE_DETAIL"})"));
    assert(h.finished(3) == 1);
    assert(h.last(AccountUpdateKind::RequestFinished).text.find("SENSITIVE_DETAIL") == std::string::npos);
    h.protocol.response(first.id, parse(raw));
    h.protocol.response(third.id, parse(raw));
    assert(h.finished(3) == 1);
}

void test_sessions_exact_ids_guard_and_refresh()
{
    Harness h;
    h.submit(AccountAction::LoadSessions);
    h.reply("getActiveSessions", R"({"@type":"sessions","sessions":[{"id":"9223372036854775000","is_current":true,"device_model":"Current"},{"id":"-9223372036854775000","is_current":false,"device_model":"Other"}]})");
    const auto sessions = h.last(AccountUpdateKind::Sessions).sessions;
    assert(sessions[0].id == 9223372036854775000LL);
    assert(sessions[1].id == -9223372036854775000LL);
    AccountRequest end;
    end.action = AccountAction::TerminateSession;
    end.request_id = 2;
    end.target_id = sessions[0].id;
    h.protocol.submit(end);
    assert(h.sent.empty() && !h.last(AccountUpdateKind::RequestFinished).success);
    end.request_id = 3;
    end.target_id = sessions[1].id;
    h.protocol.submit(end);
    const auto terminate = h.pop("terminateSession");
    assert(terminate.value.at("session_id").as_string() == "-9223372036854775000");
    h.protocol.response(terminate.id, parse(R"({"@type":"ok"})"));
    assert(h.finished(3) == 0);
    h.reply("getActiveSessions", R"({"@type":"sessions","sessions":[]})");
    assert(h.finished(3) == 1);
}

void test_blocked_pages_and_unblock_schema()
{
    Harness h;
    h.protocol.observe(Json(Json::Object{{"@type", Json("updateUser")}, {"user", parse(user_json)}}));
    h.protocol.observe(parse(R"({"@type":"updateNewChat","chat":{"id":-99,"title":"Channel"}})"));
    h.submit(AccountAction::LoadBlocked);
    h.reply("getBlockedMessageSenders", R"({"@type":"messageSenders","total_count":2,"senders":[{"@type":"messageSenderUser","user_id":42}]})");
    assert(h.finished() == 0);
    const auto next = h.pop("getBlockedMessageSenders");
    assert(next.value.at("offset").as_int64() == 1);
    h.protocol.response(next.id, parse(R"({"@type":"messageSenders","total_count":2,"senders":[{"@type":"messageSenderChat","chat_id":-99}]})"));
    assert(h.finished() == 1);
    const auto& blocked = h.last(AccountUpdateKind::Blocked).blocked;
    assert(blocked.size() == 2 && blocked[0].name == "Alice Example" && blocked[1].name == "Channel");
    AccountRequest unblock;
    unblock.action = AccountAction::UnblockSender;
    unblock.request_id = 2;
    unblock.target_id = -99;
    unblock.target_is_chat = true;
    h.protocol.submit(unblock);
    const auto wire = h.pop("setMessageSenderBlockList");
    assert(wire.value.at("block_list").is_null());
    assert(wire.value.at("sender_id").at("@type").as_string() == "messageSenderChat");
    h.protocol.response(wire.id, parse(R"({"@type":"ok"})"));
    h.reply("getBlockedMessageSenders", R"({"@type":"messageSenders","total_count":0,"senders":[]})");
    assert(h.finished(2) == 1);
}

void test_clear_cache_uses_tdlib_and_waits_for_usage()
{
    Harness h;
    h.submit(AccountAction::ClearMediaCache);
    const auto clear = h.pop("optimizeStorage");
    assert(clear.value.at("file_types").as_array().empty());
    assert(clear.value.at("exclude_chat_ids").as_array().empty());
    assert(clear.value.at("size").as_int64() == 0);
    assert(!clear.value.at("return_deleted_file_statistics").as_bool());
    h.protocol.response(clear.id, parse(R"({"@type":"storageStatistics","size":0,"count":0,"by_chat":[]})"));
    h.last(AccountUpdateKind::MediaCacheCleared);
    assert(h.finished() == 0);
    h.reply("getStorageStatisticsFast", R"({"@type":"storageStatisticsFast","files_size":9007199254740993,"file_count":7,"database_size":123456})");
    assert(h.last(AccountUpdateKind::Storage).storage.files_size == 9007199254740993LL);
    assert(h.finished() == 1);
    assert(h.updates[h.updates.size() - 2].kind == AccountUpdateKind::Storage);
}

void test_remaining_setters_refresh_and_rejections()
{
    Harness h;
    AccountRequest set;
    set.action = AccountAction::SetUsername;
    set.request_id = 1;
    set.profile.user.username = "new_name";
    h.protocol.submit(set);
    const auto username = h.pop("setUsername");
    assert(username.value.at("username").as_string() == "new_name");
    h.protocol.response(username.id, parse(R"({"@type":"error","code":400,"message":"USERNAME_OCCUPIED"})"));
    assert(h.finished() == 1 && !h.last(AccountUpdateKind::RequestFinished).success);
    assert(h.sent.empty());

    set.action = AccountAction::SetBio;
    set.request_id = 2;
    set.profile.bio = "new bio";
    h.protocol.submit(set);
    const auto bio = h.pop("setBio");
    assert(bio.value.at("bio").as_string() == "new bio");
    h.protocol.response(bio.id, parse(R"({"@type":"ok"})"));
    h.reply("getMe", user_json);
    h.reply("getUserFullInfo", R"({"@type":"userFullInfo","bio":{"text":"new bio","entities":[]}})");
    assert(h.finished(2) == 1 && h.last(AccountUpdateKind::Profile).profile.bio == "new bio");

    set.action = AccountAction::RemoveContact;
    set.request_id = 3;
    set.target_id = 42;
    h.protocol.submit(set);
    const auto remove = h.pop("removeContacts");
    assert(remove.value.at("user_ids").at(0).as_int64() == 42);
    h.protocol.response(remove.id, parse(R"({"@type":"ok"})"));
    assert(h.finished(3) == 0);
    h.reply("getContacts", R"({"@type":"users","total_count":0,"user_ids":[]})");
    assert(h.finished(3) == 1);
    h.submit(AccountAction::LoadStorage, 4);
    h.reply("getStorageStatisticsFast", R"({"@type":"storageStatisticsFast","files_size":1024,"file_count":1,"database_size":3000})");
    assert(h.finished(4) == 1 && h.last(AccountUpdateKind::Storage).storage.database_size == 3000);

    set.action = AccountAction::SetPrivacy;
    set.request_id = 5;
    h.protocol.submit(set);
    assert(h.finished(5) == 1 && !h.last(AccountUpdateKind::RequestFinished).success);
    set.action = AccountAction::SetBio;
    set.request_id = 6;
    set.profile.bio = "line\nbreak";
    h.protocol.submit(set);
    assert(h.finished(6) == 1 && !h.last(AccountUpdateKind::RequestFinished).success);
    assert(h.sent.empty());
}

void test_bounded_sessions_and_blocked_results()
{
    Harness h;
    h.submit(AccountAction::LoadSessions);
    Json::Array entries;
    for (int id = 1; id <= 150; ++id) entries.emplace_back(Json::Object{{"id", Json(id)}, {"is_current", Json(id == 1)}});
    const auto session_request = h.pop("getActiveSessions");
    h.protocol.response(session_request.id, Json(Json::Object{{"@type", Json("sessions")}, {"sessions", Json(std::move(entries))}}));
    assert(h.last(AccountUpdateKind::Sessions).sessions.size() == kMaxAccountSessions);

    h.submit(AccountAction::LoadBlocked, 2);
    for (int page = 0; page != 5; ++page) {
        Json::Array senders;
        for (int id = 1; id <= 100; ++id) {
            const auto chat_id = -(page * 100 + id);
            senders.emplace_back(Json::Object{{"@type", Json("messageSenderChat")}, {"chat_id", Json(chat_id)}});
            h.protocol.observe(Json(Json::Object{{"@type", Json("updateNewChat")}, {"chat", Json(Json::Object{{"id", Json(chat_id)}, {"title", Json("Channel")}})}}));
        }
        const auto blocked_request = h.pop("getBlockedMessageSenders");
        assert(blocked_request.value.at("offset").as_int64() == page * 100);
        h.protocol.response(blocked_request.id, Json(Json::Object{{"@type", Json("messageSenders")}, {"total_count", Json(600)}, {"senders", Json(std::move(senders))}}));
    }
    assert(h.sent.empty());
    assert(h.finished(2) == 1);
    assert(h.last(AccountUpdateKind::Blocked).blocked.size() == kMaxBlockedSenders);
    assert(h.last(AccountUpdateKind::Blocked).total_count == 600);

    h.submit(AccountAction::LoadBlocked, 3);
    h.reply("getBlockedMessageSenders", R"({"@type":"messageSenders","total_count":1,"senders":[{"@type":"messageSenderChat","chat_id":-9999}]})");
    assert(h.finished(3) == 0);
    h.reply("getChat", R"({"@type":"chat","id":-9999,"title":"Fetched Channel"})");
    assert(h.finished(3) == 1 && h.last(AccountUpdateKind::Blocked).blocked[0].name == "Fetched Channel");
}

void test_send_failure_finishes_once_and_invalid_payload()
{
    std::vector<AccountUpdate> updates;
    TdlibAccountProtocol protocol(
        [](Json, std::uint64_t) { return false; },
        [&](AccountUpdate update) { updates.push_back(std::move(update)); },
        [](const Json&) { return Chat{}; });
    AccountRequest request;
    request.action = AccountAction::LoadNotifications;
    request.request_id = 1;
    protocol.submit(request);
    assert(updates.size() == 1 && !updates[0].success && updates[0].kind == AccountUpdateKind::RequestFinished);
    Harness h;
    h.submit(AccountAction::LoadContacts);
    h.reply("getContacts", R"({"@type":"ok"})");
    assert(h.finished() == 1 && !h.last(AccountUpdateKind::RequestFinished).success);
}

void test_tdlib_implicit_privacy_default_is_nobody()
{
    Harness h;
    h.submit(AccountAction::LoadPrivacy);
    h.reply("getUserPrivacySettingRules", R"({"@type":"userPrivacySettingRules","rules":[]})");
    assert(h.last(AccountUpdateKind::Privacy).privacy.visibility == PrivacyVisibility::Nobody);
    h.reply("getUserPrivacySettingRules", R"({"@type":"userPrivacySettingRules","rules":[{"@type":"userPrivacySettingRuleAllowUsers","user_ids":[42]}]})");
    assert(h.last(AccountUpdateKind::Privacy).privacy.visibility == PrivacyVisibility::Nobody);
    assert(h.last(AccountUpdateKind::Privacy).privacy.exception_count == 1);
    h.reply("getUserPrivacySettingRules", R"({"@type":"userPrivacySettingRules","rules":[{"@type":"userPrivacySettingRuleAllowContacts"}]})");
    assert(h.last(AccountUpdateKind::Privacy).privacy.visibility == PrivacyVisibility::Contacts);
    assert(h.finished() == 1);
}

void test_contact_updates_survive_pending_snapshot()
{
    Harness h;
    h.protocol.observe(Json(Json::Object{{"@type", Json("updateUser")}, {"user", parse(user_json)}}));
    h.submit(AccountAction::LoadContacts);
    h.reply("getContacts", R"({"@type":"users","total_count":2,"user_ids":[42,43]})");
    auto pending = h.pop("getUser");
    Json removed = parse(user_json);
    removed.as_object()["is_contact"] = Json(false);
    h.protocol.observe(Json(Json::Object{{"@type", Json("updateUser")}, {"user", std::move(removed)}}));
    h.protocol.response(pending.id, parse(R"({"@type":"user","id":43,"first_name":"Bob","is_contact":true})"));
    const auto& contacts = h.last(AccountUpdateKind::Contacts).contacts;
    assert(contacts.size() == 1 && contacts[0].id == 43);
    assert(h.last(AccountUpdateKind::Contacts).total_count == 1);
    assert(h.finished() == 1);

    h.submit(AccountAction::LoadContacts, 2);
    h.reply("getContacts", R"({"@type":"users","total_count":2,"user_ids":[43,44]})");
    pending = h.pop("getUser");
    h.protocol.observe(parse(R"({"@type":"updateUserStatus","user_id":43,"status":{"@type":"userStatusOnline"}})"));
    h.protocol.response(pending.id, parse(R"({"@type":"user","id":44,"first_name":"Carol","is_contact":true})"));
    assert(h.last(AccountUpdateKind::Contacts).contacts[0].is_online);
    assert(h.finished(2) == 1);
}

void test_custom_privacy_order_does_not_activate_exceptions()
{
    Harness h;
    h.protocol.observe(parse(R"({"@type":"updateUserPrivacySettingRules","setting":{"@type":"userPrivacySettingShowPhoneNumber"},"rules":{"@type":"userPrivacySettingRules","rules":[{"@type":"userPrivacySettingRuleAllowAll"},{"@type":"userPrivacySettingRuleRestrictUsers","user_ids":[42]}]}})"));
    assert(h.last(AccountUpdateKind::Privacy).privacy.visibility == PrivacyVisibility::Custom);
    AccountRequest change;
    change.action = AccountAction::SetPrivacy;
    change.request_id = 1;
    change.visibility = PrivacyVisibility::Contacts;
    h.protocol.submit(change);
    assert(h.sent.empty());
    assert(h.finished() == 1 && !h.last(AccountUpdateKind::RequestFinished).success);
}

void test_contact_profile_avatars_and_download_updates()
{
    Harness h;
    auto user = parse(user_json);
    user.as_object()["profile_photo"] = parse(R"({"@type":"profilePhoto","small":{"@type":"file","id":901,"expected_size":600,"local":{"path":"","is_downloading_active":true,"is_downloading_completed":false}},"big":{"@type":"file","id":902}})");
    h.protocol.observe(Json(Json::Object{{"@type", Json("updateUser")}, {"user", user}}));
    assert(h.last(AccountUpdateKind::ContactChanged).contact.photo.id == 901);
    assert(h.last(AccountUpdateKind::ContactChanged).contact.photo.size == 600);
    assert(h.last(AccountUpdateKind::ContactChanged).contact.photo.is_downloading);

    h.submit(AccountAction::LoadProfile);
    const auto me = h.pop("getMe");
    h.protocol.response(me.id, user);
    h.reply("getUserFullInfo", R"({"@type":"userFullInfo","bio":null})");
    assert(h.last(AccountUpdateKind::Profile).profile.user.photo.id == 901);

    h.submit(AccountAction::LoadContacts, 2);
    h.reply("getContacts", R"({"@type":"users","total_count":2,"user_ids":[42,43]})");
    const auto pending = h.pop("getUser");
    h.protocol.observe(parse(R"({"@type":"updateFile","file":{"@type":"file","id":901,"size":600,"local":{"path":"/data/contact.jpg","downloaded_size":600,"is_downloading_active":false,"is_downloading_completed":true}}})"));
    h.protocol.response(pending.id, parse(R"({"@type":"user","id":43,"first_name":"Other","is_contact":true})"));
    const auto& photo = h.last(AccountUpdateKind::Contacts).contacts[0].photo;
    assert(photo.id == 901 && photo.is_downloaded && !photo.is_downloading);
    assert(photo.local_path == "/data/contact.jpg" && photo.downloaded_size == 600);

    user.as_object()["profile_photo"] = Json(nullptr);
    h.protocol.observe(Json(Json::Object{{"@type", Json("updateUser")}, {"user", user}}));
    assert(h.last(AccountUpdateKind::ContactChanged).contact.photo.id == 0);
    assert(h.last(AccountUpdateKind::Profile).profile.user.photo.id == 0);
    h.submit(AccountAction::LoadContacts, 3);
    h.reply("getContacts", R"({"@type":"users","total_count":1,"user_ids":[42]})");
    assert(h.last(AccountUpdateKind::Contacts).contacts[0].photo.local_path.empty());

    h.protocol.observe(parse(R"({"@type":"updateFile","file":{"@type":"file","id":901,"local":{"path":"/data/old.jpg","is_downloading_completed":true}}})"));
    h.submit(AccountAction::LoadContacts, 4);
    h.reply("getContacts", R"({"@type":"users","total_count":1,"user_ids":[42]})");
    assert(h.last(AccountUpdateKind::Contacts).contacts[0].photo.id == 0);
}

void test_peer_user_details_are_separate_from_own_profile_and_live()
{
    Harness h;
    h.submit(AccountAction::LoadProfile);
    h.reply("getMe", user_json);
    h.reply("getUserFullInfo", R"({"@type":"userFullInfo","bio":{"text":"My own bio"}})");
    const auto own_profile = h.last(AccountUpdateKind::Profile).profile;

    AccountRequest request;
    request.action = AccountAction::LoadContactDetails;
    request.target_id = 43;
    request.request_id = 2;
    h.protocol.submit(request);
    const auto user = h.pop("getUser");
    assert(user.value.at("user_id").as_int64() == 43);
    h.protocol.response(user.id, parse(R"({"@type":"user","id":43,"first_name":"Bob","phone_number":"","is_contact":false,"usernames":{"active_usernames":["public_bob"]},"profile_photo":{"small":{"id":701}}})"));
    const auto full = h.pop("getUserFullInfo");
    assert(full.value.at("user_id").as_int64() == 43 && h.finished(2) == 0);
    h.protocol.response(full.id, parse(R"({"@type":"userFullInfo","bio":{"text":"Bob's bio"}})"));
    const auto details = h.last(AccountUpdateKind::PeerDetails).peer_details;
    assert(details.loaded && details.target_id == 43 && !details.target_is_chat);
    assert(details.user.id == 43 && details.kind == PeerKind::User && details.username == "public_bob");
    assert(details.user.phone_number.empty() && details.description == "Bob's bio");
    assert(h.last(AccountUpdateKind::Profile).profile == own_profile && h.finished(2) == 1);

    h.protocol.observe(parse(R"({"@type":"updateUserStatus","user_id":43,"status":{"@type":"userStatusOnline"}})"));
    assert(h.last(AccountUpdateKind::PeerDetails).request_id == 0);
    assert(h.last(AccountUpdateKind::PeerDetails).peer_details.user.is_online);
    h.protocol.observe(parse(R"({"@type":"updateUserFullInfo","user_id":43,"user_full_info":{"bio":null}})"));
    assert(h.last(AccountUpdateKind::PeerDetails).peer_details.description.empty());
    h.protocol.observe(parse(R"({"@type":"updateFile","file":{"id":701,"local":{"path":"/data/bob.jpg","is_downloading_completed":true}}})"));
    assert(h.last(AccountUpdateKind::PeerDetails).peer_details.user.photo.local_path == "/data/bob.jpg");
    assert(h.last(AccountUpdateKind::Profile).profile == own_profile);
}

void test_peer_chat_types_and_group_live_updates()
{
    Harness h;
    AccountRequest request;
    request.action = AccountAction::LoadChatDetails;
    request.target_id = -100;
    request.request_id = 1;
    h.protocol.submit(request);
    h.reply("getChat", R"({"@type":"chat","id":-100,"title":"Small group","type":{"@type":"chatTypeBasicGroup","basic_group_id":7}})");
    const auto basic = h.pop("getBasicGroup");
    assert(basic.value.at("basic_group_id").as_int64() == 7);
    h.protocol.response(basic.id, parse(R"({"@type":"basicGroup","id":7,"member_count":12})"));
    const auto basic_full = h.pop("getBasicGroupFullInfo");
    assert(basic_full.value.at("basic_group_id").as_int64() == 7);
    h.protocol.response(basic_full.id, parse(R"({"@type":"basicGroupFullInfo","description":"Private group"})"));
    auto details = h.last(AccountUpdateKind::PeerDetails).peer_details;
    assert(details.loaded && details.target_is_chat && details.target_id == -100 && details.chat.id == -100);
    assert(details.kind == PeerKind::BasicGroup && details.member_count == 12 && details.description == "Private group");
    assert(h.sent.empty() && h.finished() == 1);
    h.protocol.observe(parse(R"({"@type":"updateBasicGroup","basic_group":{"id":7,"member_count":13}})"));
    assert(h.last(AccountUpdateKind::PeerDetails).peer_details.member_count == 13);
    h.protocol.observe(parse(R"({"@type":"updateBasicGroupFullInfo","basic_group_id":7,"basic_group_full_info":{"description":"Updated group"}})"));
    assert(h.last(AccountUpdateKind::PeerDetails).peer_details.description == "Updated group");

    request.target_id = -200;
    request.request_id = 2;
    h.protocol.submit(request);
    h.reply("getChat", R"({"@type":"chat","id":-200,"title":"News","type":{"@type":"chatTypeSupergroup","supergroup_id":8,"is_channel":true}})");
    const auto super = h.pop("getSupergroup");
    assert(super.value.at("supergroup_id").as_int64() == 8);
    h.protocol.response(super.id, parse(R"({"@type":"supergroup","id":8,"member_count":100,"usernames":{"active_usernames":["news_feed"]}})"));
    h.reply("getSupergroupFullInfo", R"({"@type":"supergroupFullInfo","description":"Daily news","member_count":101})");
    details = h.last(AccountUpdateKind::PeerDetails).peer_details;
    assert(details.kind == PeerKind::Channel && details.member_count == 101 && details.username == "news_feed");
    assert(details.description == "Daily news" && h.sent.empty());
    h.protocol.observe(parse(R"({"@type":"updateChatTitle","chat_id":-200,"title":"Latest news"})"));
    assert(h.last(AccountUpdateKind::PeerDetails).peer_details.chat.title == "Latest news");
    h.protocol.observe(parse(R"({"@type":"updateSupergroup","supergroup":{"id":8,"member_count":120,"usernames":{"active_usernames":["latest_news"]}}})"));
    assert(h.last(AccountUpdateKind::PeerDetails).peer_details.member_count == 120);
    assert(h.last(AccountUpdateKind::PeerDetails).peer_details.username == "latest_news");
    h.protocol.observe(parse(R"({"@type":"updateChatPhoto","chat_id":-200,"photo":{"small":{"id":702}}})"));
    h.protocol.observe(parse(R"({"@type":"updateFile","file":{"id":702,"local":{"path":"/data/news.jpg","is_downloading_completed":true}}})"));
    assert(h.last(AccountUpdateKind::PeerDetails).peer_details.chat.photo.local_path == "/data/news.jpg");
    h.protocol.observe(parse(R"({"@type":"updateChatPhoto","chat_id":-200,"photo":null})"));
    assert(h.last(AccountUpdateKind::PeerDetails).peer_details.chat.photo.id == 0);

    request.target_id = 99;
    request.request_id = 3;
    h.protocol.submit(request);
    h.reply("getChat", R"({"@type":"chat","id":99,"title":"Alice","type":{"@type":"chatTypePrivate","user_id":42}})");
    const auto private_user = h.pop("getUser");
    assert(private_user.value.at("user_id").as_int64() == 42);
    h.protocol.response(private_user.id, parse(user_json));
    h.reply("getUserFullInfo", R"({"@type":"userFullInfo","bio":null})");
    details = h.last(AccountUpdateKind::PeerDetails).peer_details;
    assert(details.target_id == 99 && details.chat.id == 99 && details.user.id == 42 && details.kind == PeerKind::User);
    assert(details.chat.is_private && details.loaded && h.finished(3) == 1);
}

void test_peer_request_replacement_and_failure_do_not_publish_wrong_details()
{
    Harness h;
    AccountRequest request;
    request.action = AccountAction::LoadContactDetails;
    request.request_id = 1;
    request.target_id = 42;
    h.protocol.submit(request);
    const auto stale = h.pop("getUser");
    request.request_id = 2;
    request.target_id = 43;
    h.protocol.submit(request);
    const auto current = h.pop("getUser");
    assert(h.finished(1) == 1);
    h.protocol.response(stale.id, parse(user_json));
    assert(h.sent.empty());
    h.protocol.response(current.id, parse(R"({"@type":"user","id":43,"first_name":"Bob"})"));
    h.reply("getUserFullInfo", R"({"@type":"error","code":400,"message":"USER_NOT_ACCESSIBLE"})");
    assert(h.finished(2) == 1 && !h.last(AccountUpdateKind::RequestFinished).success);
    assert(std::none_of(h.updates.begin(), h.updates.end(), [](const auto& update) {
        return update.kind == AccountUpdateKind::PeerDetails;
    }));
    request.request_id = 3;
    h.protocol.submit(request);
    h.reply("getUser", user_json);
    assert(h.finished(3) == 1 && !h.last(AccountUpdateKind::RequestFinished).success && h.sent.empty());
}

void test_peer_model_rejects_stale_results_and_resets_files()
{
    TelegramAccountModel model;
    AccountRequest first;
    first.action = AccountAction::LoadContactDetails;
    first.target_id = 42;
    assert(model.begin(first));
    AccountRequest current;
    current.action = AccountAction::LoadChatDetails;
    current.target_id = -100;
    assert(model.begin(current));
    assert(model.state().peer_details.target_id == -100 && !model.state().peer_details.loaded);
    AccountUpdate stale;
    stale.kind = AccountUpdateKind::PeerDetails;
    stale.request_id = first.request_id;
    stale.peer_details.target_id = 42;
    stale.peer_details.loaded = true;
    assert(!model.apply(stale));
    assert(!model.fail(first.request_id, "Stale failure") && model.error().empty());

    AccountUpdate details;
    details.kind = AccountUpdateKind::PeerDetails;
    details.request_id = current.request_id;
    details.peer_details.target_id = -100;
    details.peer_details.target_is_chat = true;
    details.peer_details.kind = PeerKind::Supergroup;
    details.peer_details.chat.id = -100;
    details.peer_details.chat.photo.id = 702;
    details.peer_details.loaded = true;
    assert(model.apply(details));
    FileInfo file;
    file.id = 702;
    file.local_path = "/data/group.jpg";
    file.is_downloaded = true;
    assert(model.apply_file_update(file));
    assert(model.state().peer_details.chat.photo == file);
    AccountUpdate finished;
    finished.request_id = current.request_id;
    assert(model.apply(finished) && !model.busy());
    stale.request_id = 0;
    assert(!model.apply(stale));
    details.request_id = 0;
    details.peer_details.description = "Live group description";
    assert(model.apply(details));
    assert(model.reset() && model.state().peer_details.target_id == 0);
    assert(!model.apply(details) && !model.apply_file_update(file));
    first.target_id = -1;
    assert(!model.begin(first));
    current.target_id = 0;
    assert(!model.begin(current));
}

void test_peer_active_usernames_and_unknown_member_counts()
{
    Harness h;
    AccountRequest request;
    request.action = AccountAction::LoadContactDetails;
    request.target_id = 42;
    request.request_id = 1;
    h.protocol.submit(request);
    h.reply("getUser", R"({"@type":"user","id":42,"first_name":"Alice","usernames":{"editable_username":"disabled_name","active_usernames":[]}})");
    h.reply("getUserFullInfo", R"({"@type":"userFullInfo","bio":null})");
    assert(h.last(AccountUpdateKind::PeerDetails).peer_details.username.empty());
    h.protocol.observe(parse(R"({"@type":"updateUser","user":{"id":42,"first_name":"Alice","usernames":{"editable_username":"disabled_name","active_usernames":["active_alias"]}}})"));
    assert(h.last(AccountUpdateKind::PeerDetails).peer_details.username == "active_alias");

    request.action = AccountAction::LoadChatDetails;
    request.target_id = -301;
    request.request_id = 2;
    h.protocol.submit(request);
    h.reply("getChat", R"({"@type":"chat","id":-301,"title":"Group","type":{"@type":"chatTypeSupergroup","supergroup_id":31,"is_channel":false}})");
    h.reply("getSupergroup", R"({"@type":"supergroup","id":31,"member_count":20,"usernames":{"editable_username":"disabled_group","active_usernames":[]}})");
    h.reply("getSupergroupFullInfo", R"({"@type":"supergroupFullInfo","description":"Group info","member_count":0})");
    assert(h.last(AccountUpdateKind::PeerDetails).peer_details.kind == PeerKind::Supergroup);
    assert(h.last(AccountUpdateKind::PeerDetails).peer_details.member_count == 20);
    assert(h.last(AccountUpdateKind::PeerDetails).peer_details.username.empty());
    h.protocol.observe(parse(R"({"@type":"updateSupergroup","supergroup":{"id":31,"member_count":0}})"));
    assert(h.last(AccountUpdateKind::PeerDetails).peer_details.member_count == 20);
    h.protocol.observe(parse(R"({"@type":"updateSupergroupFullInfo","supergroup_id":31,"supergroup_full_info":{"description":"Changed","member_count":0}})"));
    assert(h.last(AccountUpdateKind::PeerDetails).peer_details.member_count == 20);

    request.target_id = -302;
    request.request_id = 3;
    h.protocol.submit(request);
    h.reply("getChat", R"({"@type":"chat","id":-302,"title":"Unknown group","type":{"@type":"chatTypeSupergroup","supergroup_id":32,"is_channel":false}})");
    h.reply("getSupergroup", R"({"@type":"supergroup","id":32,"member_count":0})");
    h.reply("getSupergroupFullInfo", R"({"@type":"supergroupFullInfo","description":"","member_count":0})");
    assert(h.last(AccountUpdateKind::PeerDetails).peer_details.loaded);
    assert(h.last(AccountUpdateKind::PeerDetails).peer_details.member_count == 0);
}

void test_peer_contact_updates_and_demo_deletion_keep_peer_available()
{
    TelegramAccountModel model;
    Chat opened;
    auto demo = make_demo_client();
    assert(demo->start({}, [&](ClientEvent event) {
        if (event.kind != ClientEventKind::Account) return;
        if (event.account.kind == AccountUpdateKind::PrivateChatReady) opened = event.account.chat;
        model.apply(std::move(event.account));
    }));
    const auto issue = [&](AccountAction action, std::int64_t target = 0) {
        AccountRequest request;
        request.action = action;
        request.target_id = target;
        assert(model.begin(request));
        demo->account_request(request);
        assert(!model.busy() && model.error().empty());
    };
    issue(AccountAction::LoadProfile);
    const auto own_profile = model.state().profile;
    issue(AccountAction::LoadContactDetails, 502);
    const auto description = model.state().peer_details.description;
    assert(model.state().peer_details.user.is_contact);

    AccountUpdate contacts;
    contacts.kind = AccountUpdateKind::Contacts;
    contacts.contacts = {model.state().peer_details.user};
    contacts.contacts[0].first_name = "Updated Alice";
    contacts.contacts[0].photo.id = 801;
    contacts.total_count = 1;
    assert(model.apply(contacts));
    assert(model.state().peer_details.user.first_name == "Updated Alice");
    assert(model.state().peer_details.user.photo.id == 801);
    contacts.contacts.clear();
    contacts.total_count = 700;
    assert(model.apply(contacts) && model.state().peer_details.user.is_contact);
    contacts.total_count = 0;
    assert(model.apply(contacts) && !model.state().peer_details.user.is_contact);
    AccountUpdate changed;
    changed.kind = AccountUpdateKind::ContactChanged;
    changed.contact = model.state().peer_details.user;
    changed.contact.photo.id = 802;
    changed.contact.username = "inactive_editable";
    assert(model.apply(changed));
    assert(model.state().peer_details.user.photo.id == 802);
    assert(model.state().peer_details.username == "alice_studio");
    assert(model.state().peer_details.description == description && model.state().profile == own_profile);

    issue(AccountAction::LoadContactDetails, 502);
    assert(model.state().peer_details.user.is_contact);
    issue(AccountAction::RemoveContact, 502);
    assert(!model.state().peer_details.user.is_contact && model.state().peer_details.loaded);
    issue(AccountAction::LoadContactDetails, 502);
    assert(!model.state().peer_details.user.is_contact && model.state().peer_details.description == description);
    issue(AccountAction::OpenContact, 502);
    assert(opened.id == 1002 && model.state().profile == own_profile);
    demo->stop();
}

}  // namespace

int main()
{
    test_profile_waits_for_full_info_and_refresh();
    test_contacts_cache_fallback_status_and_limit();
    test_private_chat_without_main_list_and_saved_messages();
    test_contact_import_schema_and_unregistered_failure();
    test_notification_preservation_and_group_completion();
    test_privacy_preserves_exceptions_and_failure_once();
    test_sessions_exact_ids_guard_and_refresh();
    test_blocked_pages_and_unblock_schema();
    test_clear_cache_uses_tdlib_and_waits_for_usage();
    test_remaining_setters_refresh_and_rejections();
    test_bounded_sessions_and_blocked_results();
    test_send_failure_finishes_once_and_invalid_payload();
    test_tdlib_implicit_privacy_default_is_nobody();
    test_contact_updates_survive_pending_snapshot();
    test_custom_privacy_order_does_not_activate_exceptions();
    test_contact_profile_avatars_and_download_updates();
    test_peer_user_details_are_separate_from_own_profile_and_live();
    test_peer_chat_types_and_group_live_updates();
    test_peer_request_replacement_and_failure_do_not_publish_wrong_details();
    test_peer_model_rejects_stale_results_and_resets_files();
    test_peer_active_usernames_and_unknown_member_counts();
    test_peer_contact_updates_and_demo_deletion_keep_peer_available();
    std::cout << "TDLib account protocol tests passed\n";
}
