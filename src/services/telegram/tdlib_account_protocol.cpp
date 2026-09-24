#include "services/telegram/tdlib_account_protocol.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <limits>
#include <map>
#include <optional>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace telegram {
namespace {
using Json = support::json::Value;

Json object(const char* type)
{
    return Json(Json::Object{{"@type", Json(type)}});
}

void put(Json& value, const char* key, Json field)
{
    value.as_object().insert_or_assign(key, std::move(field));
}

const Json* field(const Json& value, const char* key)
{
    return value.is_object() ? value.find(key) : nullptr;
}

std::string text(const Json& value, const char* key)
{
    const Json* item = field(value, key);
    return item && item->is_string() ? item->as_string() : std::string{};
}

std::string type(const Json& value) { return text(value, "@type"); }

std::int64_t number(const Json& value, const char* key)
{
    const Json* item = field(value, key);
    if (!item) return 0;
    if (item->is_number()) return item->as_int64();
    if (!item->is_string()) return 0;
    std::int64_t result = 0;
    const auto& encoded = item->as_string();
    const auto parsed = std::from_chars(encoded.data(), encoded.data() + encoded.size(), result);
    return parsed.ec == std::errc{} && parsed.ptr == encoded.data() + encoded.size() ? result : 0;
}

bool boolean(const Json& value, const char* key)
{
    const Json* item = field(value, key);
    return item && item->is_bool() && item->as_bool();
}

const Json::Array& array(const Json& value, const char* key)
{
    static const Json::Array empty;
    const Json* item = field(value, key);
    return item && item->is_array() ? item->as_array() : empty;
}

std::int64_t id_value(const Json& value)
{
    return number(Json(Json::Object{{"id", value}}), "id");
}

FileInfo avatar_file(const Json& value)
{
    FileInfo file;
    const auto id = number(value, "id");
    if (id < 0 || id > std::numeric_limits<std::int32_t>::max()) throw std::runtime_error("Telegram returned an invalid avatar file ID");
    file.id = static_cast<std::int32_t>(id);
    file.size = number(value, "size");
    if (file.size == 0) file.size = number(value, "expected_size");
    if (const auto* local = field(value, "local")) {
        file.local_path = text(*local, "path");
        file.downloaded_size = number(*local, "downloaded_size");
        file.is_downloading = boolean(*local, "is_downloading_active");
        file.is_downloaded = boolean(*local, "is_downloading_completed");
    }
    return file;
}

void status(Contact& contact, const Json& value)
{
    const auto name = type(value);
    contact.is_online = name == "userStatusOnline";
    if (contact.is_online) contact.status = "online";
    else if (name == "userStatusRecently") contact.status = "last seen recently";
    else if (name == "userStatusLastWeek") contact.status = "last seen within a week";
    else if (name == "userStatusLastMonth") contact.status = "last seen within a month";
    else if (name == "userStatusOffline") contact.status = "last seen offline";
    else contact.status = "last seen a long time ago";
}

Contact contact(const Json& value)
{
    Contact result;
    result.id = number(value, "id");
    result.first_name = text(value, "first_name");
    result.last_name = text(value, "last_name");
    result.phone_number = text(value, "phone_number");
    result.is_contact = boolean(value, "is_contact");
    result.status = "last seen a long time ago";
    if (const Json* photo = field(value, "profile_photo")) {
        const auto* small = field(*photo, "small");
        const auto* big = field(*photo, "big");
        if (small && small->is_object()) result.photo = avatar_file(*small);
        else if (big && big->is_object()) result.photo = avatar_file(*big);
    }
    if (const Json* names = field(value, "usernames")) {
        result.username = text(*names, "editable_username");
        if (result.username.empty() && !array(*names, "active_usernames").empty()) {
            const auto& first = array(*names, "active_usernames").front();
            if (first.is_string()) result.username = first.as_string();
        }
    }
    if (const Json* state = field(value, "status")) status(result, *state);
    if (const Json* user_type = field(value, "type")) result.is_deleted = type(*user_type) == "userTypeDeleted";
    return result;
}

std::string peer_username(const Json& value)
{
    const auto* names = field(value, "usernames");
    if (!names) return text(value, "username");
    for (const auto& name : array(*names, "active_usernames")) {
        if (name.is_string() && !name.as_string().empty()) return name.as_string();
    }
    return {};
}

std::string peer_bio(const Json& value)
{
    const auto* bio = field(value, "bio");
    if (bio && bio->is_string()) return bio->as_string();
    if (bio && !text(*bio, "text").empty()) return text(*bio, "text");
    const auto* bot = field(value, "bot_info");
    return bot ? text(*bot, "description") : std::string{};
}

std::int32_t member_count(const Json& value)
{
    return static_cast<std::int32_t>(std::clamp<std::int64_t>(number(value, "member_count"), 0,
                                                          std::numeric_limits<std::int32_t>::max()));
}

const char* scope_type(NotificationScope scope)
{
    switch (scope) {
        case NotificationScope::PrivateChats: return "notificationSettingsScopePrivateChats";
        case NotificationScope::GroupChats: return "notificationSettingsScopeGroupChats";
        case NotificationScope::Channels: return "notificationSettingsScopeChannelChats";
    }
    throw std::invalid_argument("Unknown notification scope");
}

const char* privacy_type(PrivacyKey key)
{
    switch (key) {
        case PrivacyKey::PhoneNumber: return "userPrivacySettingShowPhoneNumber";
        case PrivacyKey::LastSeen: return "userPrivacySettingShowStatus";
        case PrivacyKey::ProfilePhoto: return "userPrivacySettingShowProfilePhoto";
    }
    throw std::invalid_argument("Unknown privacy setting");
}

NotificationSettings notifications(const Json& value)
{
    return {true, number(value, "mute_for") == 0, boolean(value, "show_preview"), number(value, "sound_id") != 0};
}

bool base_rule(const std::string& name)
{
    return name == "userPrivacySettingRuleAllowAll" || name == "userPrivacySettingRuleRestrictAll" ||
           name == "userPrivacySettingRuleAllowContacts" || name == "userPrivacySettingRuleRestrictContacts";
}

bool standard_privacy_order(const Json& value)
{
    bool reached_base = false;
    for (const auto& rule : array(value, "rules")) {
        const auto name = type(rule);
        if (name == "userPrivacySettingRuleRestrictContacts") return false;
        if (base_rule(name)) reached_base = true;
        else if (reached_base) return false;
    }
    return true;
}

PrivacySetting privacy(const Json& value)
{
    PrivacySetting result;
    result.loaded = true;
    bool found_base = false;
    for (const Json& rule : array(value, "rules")) {
        const auto name = type(rule);
        if (base_rule(name) && !found_base) {
            found_base = true;
            if (name == "userPrivacySettingRuleAllowAll") result.visibility = PrivacyVisibility::Everyone;
            else if (name == "userPrivacySettingRuleAllowContacts") result.visibility = PrivacyVisibility::Contacts;
            else if (name == "userPrivacySettingRuleRestrictAll") result.visibility = PrivacyVisibility::Nobody;
        } else if (!base_rule(name)) {
            const auto count = array(rule, "user_ids").size() + array(rule, "chat_ids").size();
            result.exception_count += static_cast<std::int32_t>(std::max<std::size_t>(1, count));
        }
    }
    // TDLib removes the terminal RestrictAll; unmatched users are still denied.
    if (!found_base) result.visibility = PrivacyVisibility::Nobody;
    if (!standard_privacy_order(value)) result.visibility = PrivacyVisibility::Custom;
    return result;
}

std::string error_text(const Json& value)
{
    if (number(value, "code") == 406) return "Telegram rejected the request";
    const auto message = text(value, "message");
    return message.empty() ? "Telegram request failed" : message;
}
}  // namespace

struct TdlibAccountProtocol::Impl {
    enum class Step { Me, FullProfile, Contacts, ContactUser, OpenChat, Import, SetProfile, RefreshContacts,
                      Notification, SetNotification, Privacy, SetPrivacy, Sessions, Terminate,
                      Blocked, BlockedUser, BlockedChat, Unblock, Storage, ClearStorage,
                      PeerChat, PeerUser, PeerUserFull, PeerBasicGroup, PeerSupergroup,
                      PeerBasicGroupFull, PeerSupergroupFull };
    struct Task {
        AccountRequest request;
        std::size_t outstanding = 0;
        bool finished = false;
        std::vector<Contact> contacts;
        std::vector<BlockedSender> blocked;
        std::int32_t total = 0;
        bool contacts_result = false;
        bool blocked_result = false;
        PeerDetails peer;
        std::int64_t group_id = 0;
    };
    struct Wire {
        std::shared_ptr<Task> task;
        Step step;
        std::int64_t target = 0;
    };

    Send send;
    Emit emit;
    ParseChat parse_chat;
    Clock clock;
    std::uint64_t next_wire = 1;
    std::map<std::uint64_t, Wire> wires;
    std::unordered_map<std::int64_t, Contact> users;
    std::unordered_set<std::int64_t> tracked_contacts;
    std::unordered_map<std::int64_t, std::string> chats;
    UserProfile profile;
    std::array<std::optional<Json>, 3> notification_raw;
    std::array<std::int64_t, 3> notification_received_at{};
    std::array<std::int64_t, 3> previous_sound{{-1, -1, -1}};
    std::array<std::optional<Json>, 3> privacy_raw;
    std::vector<AccountSession> sessions;
    bool sessions_loaded = false;
    std::shared_ptr<Task> peer_task;

    void publish(AccountUpdate update, const std::shared_ptr<Task>& task = {})
    {
        if (task) update.request_id = task->request.request_id;
        emit(std::move(update));
    }

    void finish(const std::shared_ptr<Task>& task, std::string failure = {})
    {
        if (task->finished) return;
        task->finished = true;
        for (auto it = wires.begin(); it != wires.end();) {
            if (it->second.task == task) it = wires.erase(it);
            else ++it;
        }
        AccountUpdate result;
        result.kind = AccountUpdateKind::RequestFinished;
        result.success = failure.empty();
        result.text = std::move(failure);
        publish(std::move(result), task);
    }

    void request(const std::shared_ptr<Task>& task, Json value, Step step, std::int64_t target = 0)
    {
        if (task->finished) return;
        if (wires.size() >= 192) throw std::runtime_error("Too many pending account requests");
        const auto id = next_wire++;
        wires.emplace(id, Wire{task, step, target});
        ++task->outstanding;
        if (!send(std::move(value), id)) throw std::runtime_error("Unable to queue Telegram request");
    }

    void user_request(const std::shared_ptr<Task>& task, std::int64_t id, Step step)
    {
        Json value = object("getUser");
        put(value, "user_id", Json(id));
        request(task, std::move(value), step, id);
    }

    Contact cache_user(const Json& value)
    {
        Contact parsed = contact(value);
        if (parsed.id == 0) throw std::runtime_error("Telegram returned an invalid user");
        if (users.size() >= 1024 && users.count(parsed.id) == 0) {
            const auto evict = std::find_if(users.begin(), users.end(), [&](const auto& entry) {
                return entry.first != profile.user.id && tracked_contacts.count(entry.first) == 0;
            });
            if (evict != users.end()) users.erase(evict);
        }
        users.insert_or_assign(parsed.id, parsed);
        if (profile.user.id == parsed.id) {
            profile.user = parsed;
            // The profile editor must address the editable username, not a collectible alias.
            if (const auto* names = field(value, "usernames")) profile.user.username = text(*names, "editable_username");
        }
        return parsed;
    }

    void update_pending_contact(const Contact& changed, bool membership_changed)
    {
        std::unordered_set<Task*> visited;
        for (auto& entry : wires) {
            const auto& task = entry.second.task;
            if (!task->contacts_result || !visited.insert(task.get()).second) continue;
            const auto existing = std::find_if(task->contacts.begin(), task->contacts.end(), [&](const auto& user) { return user.id == changed.id; });
            if (existing != task->contacts.end()) {
                if (membership_changed && !changed.is_contact) {
                    task->contacts.erase(existing);
                    task->total = std::max(0, task->total - 1);
                } else *existing = changed;
            } else if (membership_changed && changed.is_contact && task->contacts.size() < kMaxContacts) {
                task->contacts.push_back(changed);
                if (task->total < std::numeric_limits<std::int32_t>::max()) ++task->total;
            }
        }
    }

    void publish_profile(const std::shared_ptr<Task>& task = {})
    {
        AccountUpdate update;
        update.kind = AccountUpdateKind::Profile;
        update.profile = profile;
        publish(std::move(update), task);
    }

    void open_chat(const std::shared_ptr<Task>& task, std::int64_t id)
    {
        if (id <= 0) throw std::invalid_argument("Choose a Telegram contact first");
        Json value = object("createPrivateChat");
        put(value, "user_id", Json(id));
        put(value, "force", Json(false));
        request(task, std::move(value), Step::OpenChat);
    }

    void publish_peer(const std::shared_ptr<Task>& task, bool live = false)
    {
        if (!task || !task->peer.loaded) return;
        AccountUpdate update;
        update.kind = AccountUpdateKind::PeerDetails;
        update.peer_details = task->peer;
        publish(std::move(update), live ? std::shared_ptr<Task>{} : task);
    }

    void load_peer(const std::shared_ptr<Task>& task)
    {
        const auto id = task->request.target_id;
        const bool is_chat = task->request.action == AccountAction::LoadChatDetails;
        if (id == 0 || (!is_chat && id < 0)) throw std::invalid_argument("Choose a contact or chat first");
        if (peer_task && !peer_task->finished) finish(peer_task);
        peer_task = task;
        task->peer.target_id = id;
        task->peer.target_is_chat = is_chat;
        if (is_chat) {
            Json value = object("getChat");
            put(value, "chat_id", Json(id));
            request(task, std::move(value), Step::PeerChat, id);
        } else user_request(task, id, Step::PeerUser);
    }

    void peer_group_request(const std::shared_ptr<Task>& task, bool full)
    {
        const bool basic = task->peer.kind == PeerKind::BasicGroup;
        Json value = object(basic ? (full ? "getBasicGroupFullInfo" : "getBasicGroup")
                                  : (full ? "getSupergroupFullInfo" : "getSupergroup"));
        put(value, basic ? "basic_group_id" : "supergroup_id", Json(task->group_id));
        request(task, std::move(value), basic ? (full ? Step::PeerBasicGroupFull : Step::PeerBasicGroup)
                                            : (full ? Step::PeerSupergroupFull : Step::PeerSupergroup), task->group_id);
    }

    void load_notification(const std::shared_ptr<Task>& task, NotificationScope scope)
    {
        Json value = object("getScopeNotificationSettings");
        put(value, "scope", object(scope_type(scope)));
        request(task, std::move(value), Step::Notification, static_cast<int>(scope));
    }

    void load_privacy(const std::shared_ptr<Task>& task, PrivacyKey key)
    {
        Json value = object("getUserPrivacySettingRules");
        put(value, "setting", object(privacy_type(key)));
        request(task, std::move(value), Step::Privacy, static_cast<int>(key));
    }

    void load_blocked(const std::shared_ptr<Task>& task)
    {
        Json value = object("getBlockedMessageSenders");
        put(value, "block_list", object("blockListMain"));
        put(value, "offset", Json(task->blocked.size()));
        put(value, "limit", Json(std::min<std::size_t>(100, kMaxBlockedSenders - task->blocked.size())));
        request(task, std::move(value), Step::Blocked);
    }

    void submit(AccountRequest input)
    {
        auto task = std::make_shared<Task>();
        task->request = std::move(input);
        const auto& input_request = task->request;
        try {
            Json value;
            switch (input_request.action) {
                case AccountAction::LoadProfile:
                case AccountAction::OpenSavedMessages: request(task, object("getMe"), Step::Me); break;
                case AccountAction::LoadContacts: request(task, object("getContacts"), Step::Contacts); break;
                case AccountAction::LoadContactDetails:
                case AccountAction::LoadChatDetails: load_peer(task); break;
                case AccountAction::OpenContact: open_chat(task, input_request.target_id); break;
                case AccountAction::AddContact: {
                    const auto& added = input_request.contact;
                    if (added.first_name.empty() || added.phone_number.empty()) throw std::invalid_argument("Enter a first name and phone number");
                    Json item = object("importedContact");
                    put(item, "first_name", Json(added.first_name));
                    put(item, "last_name", Json(added.last_name));
                    put(item, "phone_number", Json(added.phone_number));
                    put(item, "note", Json(nullptr));
                    value = object("importContacts");
                    put(value, "contacts", Json(Json::Array{std::move(item)}));
                    request(task, std::move(value), Step::Import);
                    break;
                }
                case AccountAction::RemoveContact:
                    if (input_request.target_id <= 0) throw std::invalid_argument("Choose a contact first");
                    value = object("removeContacts");
                    put(value, "user_ids", Json(Json::Array{Json(input_request.target_id)}));
                    request(task, std::move(value), Step::RefreshContacts);
                    break;
                case AccountAction::SetName:
                    if (input_request.profile.user.first_name.empty()) throw std::invalid_argument("First name is required");
                    value = object("setName");
                    put(value, "first_name", Json(input_request.profile.user.first_name));
                    put(value, "last_name", Json(input_request.profile.user.last_name));
                    request(task, std::move(value), Step::SetProfile);
                    break;
                case AccountAction::SetUsername:
                    value = object("setUsername");
                    put(value, "username", Json(input_request.profile.user.username));
                    request(task, std::move(value), Step::SetProfile);
                    break;
                case AccountAction::SetBio:
                    if (input_request.profile.bio.find_first_of("\r\n") != std::string::npos) throw std::invalid_argument("Bio must be a single line");
                    value = object("setBio");
                    put(value, "bio", Json(input_request.profile.bio));
                    request(task, std::move(value), Step::SetProfile);
                    break;
                case AccountAction::LoadNotifications:
                    for (int index = 0; index != 3; ++index) load_notification(task, static_cast<NotificationScope>(index));
                    break;
                case AccountAction::SetNotifications: {
                    const auto index = static_cast<std::size_t>(input_request.scope);
                    scope_type(input_request.scope);
                    if (!notification_raw.at(index)) throw std::runtime_error("Load notification settings before changing them");
                    Json settings = *notification_raw[index];
                    const auto& changed = input_request.notifications;
                    const auto current = notifications(settings);
                    if (changed.enabled != current.enabled) put(settings, "mute_for", Json(changed.enabled ? 0 : std::numeric_limits<std::int32_t>::max()));
                    else {
                        const auto mute_for = number(settings, "mute_for");
                        if (mute_for > 0 && mute_for < std::numeric_limits<std::int32_t>::max()) {
                            const auto elapsed = std::max<std::int64_t>(0, clock() - notification_received_at[index]);
                            put(settings, "mute_for", Json(std::max<std::int64_t>(0, mute_for - elapsed)));
                        }
                    }
                    put(settings, "show_preview", Json(changed.show_preview));
                    if (changed.sound_enabled != current.sound_enabled) put(settings, "sound_id", Json(changed.sound_enabled ? previous_sound[index] : 0));
                    value = object("setScopeNotificationSettings");
                    put(value, "scope", object(scope_type(input_request.scope)));
                    put(value, "notification_settings", std::move(settings));
                    request(task, std::move(value), Step::SetNotification, static_cast<int>(input_request.scope));
                    break;
                }
                case AccountAction::LoadPrivacy:
                    for (int index = 0; index != 3; ++index) load_privacy(task, static_cast<PrivacyKey>(index));
                    break;
                case AccountAction::SetPrivacy: {
                    const auto index = static_cast<std::size_t>(input_request.privacy_key);
                    privacy_type(input_request.privacy_key);
                    if (!privacy_raw.at(index)) throw std::runtime_error("Load privacy settings before changing them");
                    if (!standard_privacy_order(*privacy_raw[index])) throw std::runtime_error("These privacy rules use a custom order; edit them in Telegram");
                    if (input_request.visibility == PrivacyVisibility::Custom) throw std::invalid_argument("Choose a privacy visibility");
                    Json::Array rules;
                    for (const auto& rule : array(*privacy_raw[index], "rules")) if (!base_rule(type(rule))) rules.push_back(rule);
                    if (input_request.visibility == PrivacyVisibility::Everyone) rules.push_back(object("userPrivacySettingRuleAllowAll"));
                    else {
                        if (input_request.visibility == PrivacyVisibility::Contacts) rules.push_back(object("userPrivacySettingRuleAllowContacts"));
                        rules.push_back(object("userPrivacySettingRuleRestrictAll"));
                    }
                    Json settings = object("userPrivacySettingRules");
                    put(settings, "rules", Json(std::move(rules)));
                    value = object("setUserPrivacySettingRules");
                    put(value, "setting", object(privacy_type(input_request.privacy_key)));
                    put(value, "rules", std::move(settings));
                    request(task, std::move(value), Step::SetPrivacy, static_cast<int>(input_request.privacy_key));
                    break;
                }
                case AccountAction::LoadSessions: request(task, object("getActiveSessions"), Step::Sessions); break;
                case AccountAction::TerminateSession: {
                    const auto found = std::find_if(sessions.begin(), sessions.end(), [&](const auto& session) { return session.id == input_request.target_id; });
                    if (!sessions_loaded || found == sessions.end()) throw std::runtime_error("Reload devices before ending a session");
                    if (found->is_current) throw std::invalid_argument("Use Log Out to end the current session");
                    value = object("terminateSession");
                    put(value, "session_id", Json(std::to_string(input_request.target_id)));
                    request(task, std::move(value), Step::Terminate);
                    break;
                }
                case AccountAction::LoadBlocked: load_blocked(task); break;
                case AccountAction::UnblockSender:
                    if (input_request.target_id == 0) throw std::invalid_argument("Choose a blocked sender first");
                    value = object("setMessageSenderBlockList");
                    {
                        Json sender = object(input_request.target_is_chat ? "messageSenderChat" : "messageSenderUser");
                        put(sender, input_request.target_is_chat ? "chat_id" : "user_id", Json(input_request.target_id));
                        put(value, "sender_id", std::move(sender));
                    }
                    put(value, "block_list", Json(nullptr));
                    request(task, std::move(value), Step::Unblock);
                    break;
                case AccountAction::LoadStorage: request(task, object("getStorageStatisticsFast"), Step::Storage); break;
                case AccountAction::ClearMediaCache:
                    value = object("optimizeStorage");
                    for (const char* key : {"size", "ttl", "count", "immunity_delay", "chat_limit"}) put(value, key, Json(0));
                    for (const char* key : {"file_types", "chat_ids", "exclude_chat_ids"}) put(value, key, Json(Json::Array{}));
                    put(value, "return_deleted_file_statistics", Json(false));
                    request(task, std::move(value), Step::ClearStorage);
                    break;
            }
        } catch (const std::exception& failure) { finish(task, failure.what()); }
    }

    void response(std::uint64_t id, const Json& value)
    {
        const auto found = wires.find(id);
        if (found == wires.end()) return;
        const auto wire = found->second;
        wires.erase(found);
        const auto& task = wire.task;
        if (task->finished) return;
        --task->outstanding;
        try {
            if (type(value) == "error") throw std::runtime_error(error_text(value));
            consume(wire, value);
            if (!task->finished && task->outstanding == 0) {
                if (task->contacts_result) {
                    tracked_contacts.clear();
                    for (const auto& user : task->contacts) tracked_contacts.insert(user.id);
                    AccountUpdate update;
                    update.kind = AccountUpdateKind::Contacts;
                    update.contacts = std::move(task->contacts);
                    update.total_count = task->total;
                    publish(std::move(update), task);
                }
                if (task->blocked_result) {
                    AccountUpdate update;
                    update.kind = AccountUpdateKind::Blocked;
                    update.blocked = std::move(task->blocked);
                    update.total_count = task->total;
                    publish(std::move(update), task);
                }
                publish_peer(task);
                finish(task);
            }
        } catch (const std::exception& failure) { finish(task, failure.what()); }
    }

    static void expect(const Json& value, const char* expected)
    {
        if (type(value) != expected) throw std::runtime_error(std::string("Unexpected Telegram response: ") + type(value));
    }

    void consume(const Wire& wire, const Json& value)
    {
        const auto& task = wire.task;
        AccountUpdate update;
        switch (wire.step) {
            case Step::PeerChat: {
                expect(value, "chat");
                if (number(value, "id") != wire.target) throw std::runtime_error("Telegram returned a different chat");
                task->peer.chat = parse_chat(value);
                const auto* chat_type = field(value, "type");
                if (!chat_type) throw std::runtime_error("Telegram returned a chat without its type");
                const auto kind = type(*chat_type);
                if (kind == "chatTypePrivate" || kind == "chatTypeSecret") {
                    const auto user_id = number(*chat_type, "user_id");
                    if (user_id <= 0) throw std::runtime_error("Telegram returned an invalid chat user");
                    task->peer.chat.is_private = true;
                    user_request(task, user_id, Step::PeerUser);
                } else if (kind == "chatTypeBasicGroup" || kind == "chatTypeSupergroup") {
                    task->peer.kind = kind == "chatTypeBasicGroup" ? PeerKind::BasicGroup
                        : boolean(*chat_type, "is_channel") ? PeerKind::Channel : PeerKind::Supergroup;
                    task->group_id = number(*chat_type, kind == "chatTypeBasicGroup" ? "basic_group_id" : "supergroup_id");
                    if (task->group_id <= 0) throw std::runtime_error("Telegram returned an invalid group");
                    peer_group_request(task, false);
                } else throw std::runtime_error("This chat type does not provide details");
                break;
            }
            case Step::PeerUser: {
                expect(value, "user");
                if (number(value, "id") != wire.target) throw std::runtime_error("Telegram returned a different user");
                task->peer.user = cache_user(value);
                task->peer.username = peer_username(value);
                Json full = object("getUserFullInfo");
                put(full, "user_id", Json(wire.target));
                request(task, std::move(full), Step::PeerUserFull, wire.target);
                break;
            }
            case Step::PeerUserFull:
                expect(value, "userFullInfo");
                task->peer.description = peer_bio(value);
                task->peer.loaded = true;
                break;
            case Step::PeerBasicGroup:
            case Step::PeerSupergroup:
                expect(value, wire.step == Step::PeerBasicGroup ? "basicGroup" : "supergroup");
                if (number(value, "id") != wire.target) throw std::runtime_error("Telegram returned a different group");
                task->peer.member_count = member_count(value);
                task->peer.username = peer_username(value);
                peer_group_request(task, true);
                break;
            case Step::PeerBasicGroupFull:
            case Step::PeerSupergroupFull:
                expect(value, wire.step == Step::PeerBasicGroupFull ? "basicGroupFullInfo" : "supergroupFullInfo");
                task->peer.description = text(value, "description");
                if (wire.step == Step::PeerSupergroupFull && member_count(value) > 0)
                    task->peer.member_count = member_count(value);
                task->peer.loaded = true;
                break;
            case Step::Me: {
                expect(value, "user");
                profile.user.id = number(value, "id");
                cache_user(value);
                if (task->request.action == AccountAction::OpenSavedMessages) open_chat(task, profile.user.id);
                else {
                    Json request_value = object("getUserFullInfo");
                    put(request_value, "user_id", Json(profile.user.id));
                    request(task, std::move(request_value), Step::FullProfile);
                }
                break;
            }
            case Step::FullProfile:
                expect(value, "userFullInfo");
                if (const Json* bio = field(value, "bio")) profile.bio = text(*bio, "text");
                else profile.bio.clear();
                profile.loaded = true;
                publish_profile(task);
                break;
            case Step::Contacts: {
                expect(value, "users");
                task->contacts_result = true;
                task->total = static_cast<std::int32_t>(std::clamp<std::int64_t>(number(value, "total_count"), 0, std::numeric_limits<std::int32_t>::max()));
                for (const auto& user_id : array(value, "user_ids")) {
                    if (task->contacts.size() >= kMaxContacts) break;
                    const auto id = id_value(user_id);
                    if (id == 0) continue;
                    const auto cached = users.find(id);
                    if (cached != users.end()) task->contacts.push_back(cached->second);
                    else {
                        Contact missing;
                        missing.id = id;
                        task->contacts.push_back(std::move(missing));
                    }
                }
                // Bound concurrent offline lookups; the next lookup starts when one finishes.
                fill_contact_users(task);
                break;
            }
            case Step::ContactUser: {
                expect(value, "user");
                const auto loaded = cache_user(value);
                if (loaded.id != wire.target) throw std::runtime_error("Telegram returned a different contact");
                for (auto& entry : task->contacts) if (entry.id == loaded.id) entry = loaded;
                fill_contact_users(task);
                break;
            }
            case Step::OpenChat:
                expect(value, "chat");
                update.kind = AccountUpdateKind::PrivateChatReady;
                update.chat = parse_chat(value);
                if (update.chat.id == 0) throw std::runtime_error("Telegram returned an invalid private chat");
                if (task->request.action == AccountAction::OpenSavedMessages) update.chat.title = "Saved Messages";
                publish(std::move(update), task);
                break;
            case Step::Import:
                expect(value, "importedContacts");
                if (array(value, "user_ids").empty() || id_value(array(value, "user_ids").front()) == 0) throw std::runtime_error("This phone number is not registered on Telegram");
                request(task, object("getContacts"), Step::Contacts);
                break;
            case Step::RefreshContacts:
                expect(value, "ok");
                request(task, object("getContacts"), Step::Contacts);
                break;
            case Step::SetProfile:
                expect(value, "ok");
                request(task, object("getMe"), Step::Me);
                break;
            case Step::Notification:
                expect(value, "scopeNotificationSettings");
                store_notification(static_cast<NotificationScope>(wire.target), value, task);
                break;
            case Step::SetNotification:
                expect(value, "ok");
                load_notification(task, static_cast<NotificationScope>(wire.target));
                break;
            case Step::Privacy:
                expect(value, "userPrivacySettingRules");
                store_privacy(static_cast<PrivacyKey>(wire.target), value, task);
                break;
            case Step::SetPrivacy:
                expect(value, "ok");
                load_privacy(task, static_cast<PrivacyKey>(wire.target));
                break;
            case Step::Sessions:
                expect(value, "sessions");
                sessions.clear();
                for (const auto& item : array(value, "sessions")) {
                    if (sessions.size() >= kMaxAccountSessions) break;
                    AccountSession session;
                    session.id = number(item, "id");
                    session.is_current = boolean(item, "is_current");
                    session.device_model = text(item, "device_model");
                    session.application_name = text(item, "application_name");
                    session.platform = text(item, "platform");
                    session.location = text(item, "location");
                    session.ip_address = text(item, "ip_address");
                    session.last_active_date = number(item, "last_active_date");
                    sessions.push_back(std::move(session));
                }
                sessions_loaded = true;
                update.kind = AccountUpdateKind::Sessions;
                update.sessions = sessions;
                publish(std::move(update), task);
                break;
            case Step::Terminate:
                expect(value, "ok");
                request(task, object("getActiveSessions"), Step::Sessions);
                break;
            case Step::Blocked: {
                expect(value, "messageSenders");
                task->blocked_result = true;
                task->total = static_cast<std::int32_t>(std::clamp<std::int64_t>(number(value, "total_count"), 0, std::numeric_limits<std::int32_t>::max()));
                const auto& senders = array(value, "senders");
                for (const auto& sender : senders) {
                    if (task->blocked.size() >= kMaxBlockedSenders) break;
                    BlockedSender blocked;
                    blocked.is_chat = type(sender) == "messageSenderChat";
                    blocked.id = number(sender, blocked.is_chat ? "chat_id" : "user_id");
                    if (blocked.is_chat) {
                        const auto entry = chats.find(blocked.id);
                        if (entry != chats.end()) blocked.name = entry->second;
                    } else {
                        const auto entry = users.find(blocked.id);
                        if (entry != users.end()) blocked.name = contact_name(entry->second);
                    }
                    task->blocked.push_back(std::move(blocked));
                }
                if (!senders.empty() && task->blocked.size() < kMaxBlockedSenders && task->blocked.size() < static_cast<std::size_t>(task->total)) load_blocked(task);
                else fill_blocked_users(task);
                break;
            }
            case Step::BlockedUser: {
                expect(value, "user");
                const auto loaded = cache_user(value);
                if (loaded.id != wire.target) throw std::runtime_error("Telegram returned a different blocked sender");
                for (auto& entry : task->blocked) if (!entry.is_chat && entry.id == loaded.id) entry.name = contact_name(loaded);
                fill_blocked_users(task);
                break;
            }
            case Step::BlockedChat: {
                expect(value, "chat");
                if (number(value, "id") != wire.target) throw std::runtime_error("Telegram returned a different blocked chat");
                const auto title = text(value, "title");
                if (chats.size() >= 512) chats.erase(chats.begin());
                chats.insert_or_assign(wire.target, title);
                for (auto& entry : task->blocked) if (entry.is_chat && entry.id == wire.target) entry.name = title.empty() ? "Deleted Channel" : title;
                fill_blocked_users(task);
                break;
            }
            case Step::Unblock:
                expect(value, "ok");
                load_blocked(task);
                break;
            case Step::Storage:
                expect(value, "storageStatisticsFast");
                update.kind = AccountUpdateKind::Storage;
                update.storage = {true, number(value, "files_size"), number(value, "file_count"), number(value, "database_size")};
                publish(std::move(update), task);
                break;
            case Step::ClearStorage:
                expect(value, "storageStatistics");
                update.kind = AccountUpdateKind::MediaCacheCleared;
                publish(std::move(update), task);
                request(task, object("getStorageStatisticsFast"), Step::Storage);
                break;
        }
    }

    void fill_contact_users(const std::shared_ptr<Task>& task)
    {
        for (auto& entry : task->contacts) {
            if (entry.first_name.empty() && entry.last_name.empty() && entry.status.empty()) {
                user_request(task, entry.id, Step::ContactUser);
                return;
            }
        }
    }

    void fill_blocked_users(const std::shared_ptr<Task>& task)
    {
        for (const auto& entry : task->blocked) {
            if (entry.name.empty()) {
                if (entry.is_chat) {
                    Json value = object("getChat");
                    put(value, "chat_id", Json(entry.id));
                    request(task, std::move(value), Step::BlockedChat, entry.id);
                } else user_request(task, entry.id, Step::BlockedUser);
                return;
            }
        }
    }

    void store_notification(NotificationScope scope, const Json& value, const std::shared_ptr<Task>& task = {})
    {
        const auto index = static_cast<std::size_t>(scope);
        notification_raw.at(index) = value;
        notification_received_at[index] = clock();
        const auto sound = number(value, "sound_id");
        if (sound != 0) previous_sound[index] = sound;
        AccountUpdate update;
        update.kind = AccountUpdateKind::Notifications;
        update.scope = scope;
        update.notifications = notifications(value);
        publish(std::move(update), task);
    }

    void store_privacy(PrivacyKey key, const Json& value, const std::shared_ptr<Task>& task = {})
    {
        privacy_raw.at(static_cast<std::size_t>(key)) = value;
        AccountUpdate update;
        update.kind = AccountUpdateKind::Privacy;
        update.privacy_key = key;
        update.privacy = privacy(value);
        publish(std::move(update), task);
    }

    void observe(const Json& value)
    {
        const auto name = type(value);
        if (name == "updateFile" || name == "file") {
            const Json* incoming = name == "file" ? &value : field(value, "file");
            if (!incoming || !incoming->is_object()) return;
            const auto photo = avatar_file(*incoming);
            if (photo.id == 0) return;
            for (auto& entry : users) {
                if (entry.second.photo.id != photo.id) continue;
                entry.second.photo = photo;
                update_pending_contact(entry.second, false);
            }
            if (profile.user.photo.id == photo.id) profile.user.photo = photo;
            if (peer_task) {
                bool changed = false;
                for (auto* avatar : {&peer_task->peer.user.photo, &peer_task->peer.chat.photo}) {
                    if (avatar->id == photo.id && *avatar != photo) {
                        *avatar = photo;
                        changed = true;
                    }
                }
                if (changed) publish_peer(peer_task, true);
            }
        } else if (name == "updateUser") {
            const Json* user = field(value, "user");
            if (!user || !user->is_object()) return;
            const auto previous = users.find(number(*user, "id"));
            const bool was_contact = (previous != users.end() && previous->second.is_contact) || tracked_contacts.count(number(*user, "id")) != 0;
            AccountUpdate update;
            update.kind = AccountUpdateKind::ContactChanged;
            update.contact = cache_user(*user);
            update_pending_contact(update.contact, was_contact != update.contact.is_contact);
            if (peer_task && peer_task->peer.user.id == update.contact.id) {
                peer_task->peer.user = update.contact;
                peer_task->peer.username = peer_username(*user);
                publish_peer(peer_task, true);
            }
            if (!update.contact.is_contact) tracked_contacts.erase(update.contact.id);
            else if (tracked_contacts.size() < kMaxContacts) tracked_contacts.insert(update.contact.id);
            if (was_contact || update.contact.is_contact) publish(std::move(update));
            if (profile.loaded && profile.user.id == number(*user, "id")) publish_profile();
        } else if (name == "updateUserStatus") {
            const auto found = users.find(number(value, "user_id"));
            const Json* state = field(value, "status");
            if (found == users.end() || !state) return;
            status(found->second, *state);
            update_pending_contact(found->second, false);
            if (peer_task && peer_task->peer.user.id == found->first) {
                peer_task->peer.user = found->second;
                publish_peer(peer_task, true);
            }
            AccountUpdate update;
            update.kind = AccountUpdateKind::ContactChanged;
            update.contact = found->second;
            if (found->second.is_contact) publish(std::move(update));
        } else if (name == "updateUserFullInfo") {
            const Json* full = field(value, "user_full_info");
            if (!full) return;
            if (profile.user.id == number(value, "user_id")) {
                const Json* bio = field(*full, "bio");
                profile.bio = bio ? text(*bio, "text") : std::string{};
                if (profile.loaded) publish_profile();
            }
            if (peer_task && peer_task->peer.user.id == number(value, "user_id")) {
                peer_task->peer.description = peer_bio(*full);
                publish_peer(peer_task, true);
            }
        } else if (name == "updateBasicGroup" || name == "updateSupergroup") {
            const bool basic = name == "updateBasicGroup";
            const Json* group = field(value, basic ? "basic_group" : "supergroup");
            if (!group || !peer_task || peer_task->group_id <= 0 || peer_task->group_id != number(*group, "id") ||
                (peer_task->peer.kind == PeerKind::BasicGroup) != basic) return;
            if (basic || member_count(*group) > 0) peer_task->peer.member_count = member_count(*group);
            peer_task->peer.username = peer_username(*group);
            publish_peer(peer_task, true);
        } else if (name == "updateBasicGroupFullInfo" || name == "updateSupergroupFullInfo") {
            const bool basic = name == "updateBasicGroupFullInfo";
            const Json* full = field(value, basic ? "basic_group_full_info" : "supergroup_full_info");
            if (!full || !peer_task || peer_task->group_id <= 0 || peer_task->group_id != number(value, basic ? "basic_group_id" : "supergroup_id") ||
                (peer_task->peer.kind == PeerKind::BasicGroup) != basic) return;
            peer_task->peer.description = text(*full, "description");
            if (!basic && member_count(*full) > 0) peer_task->peer.member_count = member_count(*full);
            publish_peer(peer_task, true);
        } else if (name == "updateScopeNotificationSettings") {
            const Json* scope = field(value, "scope");
            const Json* settings = field(value, "notification_settings");
            if (!scope || !settings) return;
            for (int index = 0; index != 3; ++index) if (type(*scope) == scope_type(static_cast<NotificationScope>(index))) store_notification(static_cast<NotificationScope>(index), *settings);
        } else if (name == "updateUserPrivacySettingRules") {
            const Json* key = field(value, "setting");
            const Json* rules = field(value, "rules");
            if (!key || !rules) return;
            for (int index = 0; index != 3; ++index) if (type(*key) == privacy_type(static_cast<PrivacyKey>(index))) store_privacy(static_cast<PrivacyKey>(index), *rules);
        } else if (name == "updateNewChat") {
            const Json* chat = field(value, "chat");
            if (!chat) return;
            if (chats.size() >= 512) chats.erase(chats.begin());
            chats.insert_or_assign(number(*chat, "id"), text(*chat, "title"));
            if (peer_task && peer_task->peer.chat.id == number(*chat, "id")) {
                peer_task->peer.chat = parse_chat(*chat);
                publish_peer(peer_task, true);
            }
        } else if (name == "updateChatTitle") {
            if (chats.size() >= 512) chats.erase(chats.begin());
            chats.insert_or_assign(number(value, "chat_id"), text(value, "title"));
            if (peer_task && peer_task->peer.chat.id == number(value, "chat_id")) {
                peer_task->peer.chat.title = text(value, "title");
                publish_peer(peer_task, true);
            }
        } else if (name == "updateChatPhoto" && peer_task && peer_task->peer.chat.id == number(value, "chat_id")) {
            const auto* photo = field(value, "photo");
            const auto* small = photo ? field(*photo, "small") : nullptr;
            peer_task->peer.chat.photo = small ? avatar_file(*small) : FileInfo{};
            publish_peer(peer_task, true);
        }
    }
};

TdlibAccountProtocol::TdlibAccountProtocol(Send send, Emit emit, ParseChat parse_chat, Clock clock)
    : impl_(std::make_unique<Impl>())
{
    impl_->send = std::move(send);
    impl_->emit = std::move(emit);
    impl_->parse_chat = std::move(parse_chat);
    impl_->clock = clock ? std::move(clock) : Clock([] {
        return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    });
}

TdlibAccountProtocol::~TdlibAccountProtocol() = default;
void TdlibAccountProtocol::submit(AccountRequest request) { impl_->submit(std::move(request)); }
void TdlibAccountProtocol::response(std::uint64_t id, const Json& value) { impl_->response(id, value); }
void TdlibAccountProtocol::observe(const Json& update) { impl_->observe(update); }

}  // namespace telegram
