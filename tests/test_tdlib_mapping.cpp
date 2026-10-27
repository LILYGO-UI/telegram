#include "services/telegram/tdlib_client_test.hpp"
#include "services/telegram/telegram_client.hpp"
#include "support/json.hpp"

#include <cassert>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

namespace {

void test_authorization_and_connection_states()
{
    using telegram::AuthState;
    using telegram::ConnectionState;

    assert(
        telegram::tdlib_test::map_authorization_state(R"({"@type":"authorizationStateWaitTdlibParameters"})").state ==
        AuthState::WaitingForParameters);
    assert(telegram::tdlib_test::map_authorization_state(R"({"@type":"authorizationStateWaitPhoneNumber"})").state ==
           AuthState::WaitingForPhoneNumber);
    assert(telegram::tdlib_test::map_authorization_state(R"({"@type":"authorizationStateWaitEmailAddress"})").state ==
           AuthState::WaitingForEmailAddress);
    assert(telegram::tdlib_test::map_authorization_state(R"({"@type":"authorizationStateWaitEmailCode"})").state ==
           AuthState::WaitingForEmailCode);
    assert(telegram::tdlib_test::map_authorization_state(R"({"@type":"authorizationStateWaitCode"})").state ==
           AuthState::WaitingForCode);
    assert(telegram::tdlib_test::map_authorization_state(R"({"@type":"authorizationStateWaitPassword"})").state ==
           AuthState::WaitingForPassword);
    assert(telegram::tdlib_test::map_authorization_state(R"({"@type":"authorizationStateWaitRegistration"})").state ==
           AuthState::WaitingForRegistration);

    const auto qr = telegram::tdlib_test::map_authorization_state(
        R"({"@type":"authorizationStateWaitOtherDeviceConfirmation","link":"tg://login?token=abc"})");
    assert(qr.state == AuthState::WaitingForOtherDeviceConfirmation);
    assert(qr.prompt == "tg://login?token=abc");
    assert(
        telegram::tdlib_test::map_authorization_state(R"({"@type":"authorizationStateWaitPremiumPurchase"})").state ==
        AuthState::WaitingForPremiumPurchase);

    assert(telegram::tdlib_test::map_authorization_state(R"({"@type":"authorizationStateReady"})").state ==
           AuthState::Ready);
    assert(telegram::tdlib_test::map_authorization_state(R"({"@type":"authorizationStateLoggingOut"})").state ==
           AuthState::LoggingOut);
    assert(telegram::tdlib_test::map_authorization_state(R"({"@type":"authorizationStateClosing"})").state ==
           AuthState::Closing);
    assert(telegram::tdlib_test::map_authorization_state(R"({"@type":"authorizationStateClosed"})").state ==
           AuthState::Closed);
    assert(telegram::tdlib_test::map_authorization_state(R"({"@type":"authorizationStateFuture"})").state ==
           AuthState::Error);

    assert(telegram::tdlib_test::map_connection_state(R"({"@type":"connectionStateWaitingForNetwork"})") ==
           ConnectionState::WaitingForNetwork);
    assert(telegram::tdlib_test::map_connection_state(R"({"@type":"connectionStateConnectingToProxy"})") ==
           ConnectionState::ConnectingToProxy);
    assert(telegram::tdlib_test::map_connection_state(R"({"@type":"connectionStateConnecting"})") ==
           ConnectionState::Connecting);
    assert(telegram::tdlib_test::map_connection_state(R"({"@type":"connectionStateUpdating"})") ==
           ConnectionState::Updating);
    assert(telegram::tdlib_test::map_connection_state(R"({"@type":"connectionStateReady"})") == ConnectionState::Ready);
}

void test_qr_login_request()
{
    const auto request = support::json::parse(telegram::tdlib_test::qr_login_request());
    assert(request.at("@type").as_string() == "requestQrCodeAuthentication");
    assert(request.at("other_user_ids").is_array());
    assert(request.at("other_user_ids").as_array().empty());
}

void test_file_mapping()
{
    const auto file = telegram::tdlib_test::map_file(
        R"({"@type":"file","id":42,"size":9876543210,"expected_size":0,"local":{"path":"/data/photo.jpg","is_downloading_active":true,"is_downloading_completed":false,"downloaded_size":12345}})");
    assert(file.id == 42);
    assert(file.size == 9876543210LL);
    assert(file.downloaded_size == 12345);
    assert(file.local_path == "/data/photo.jpg");
    assert(file.is_downloading);
    assert(!file.is_downloaded);
}

void test_chat_mapping_preserves_int64()
{
    const auto chat = telegram::tdlib_test::map_chat(
        R"({"@type":"chat","id":-100900719925474099,"title":"Exact IDs","unread_count":7,"positions":[{"@type":"chatPosition","list":{"@type":"chatListMain"},"order":"9223372036854775000"}],"last_message":{"@type":"message","id":9007199254740993,"date":1900000000,"content":{"@type":"messageText","text":{"@type":"formattedText","text":"hello","entities":[]}}},"notification_settings":{"@type":"chatNotificationSettings","mute_for":60},"photo":{"@type":"chatPhotoInfo","small":{"@type":"file","id":9,"size":100,"local":{"path":"/data/avatar.jpg","downloaded_size":100,"is_downloading_active":false,"is_downloading_completed":true}}}})");
    assert(chat.id == -100900719925474099LL);
    assert(chat.order == 9223372036854775000LL);
    assert(chat.last_message_id == 9007199254740993LL);
    assert(chat.last_message_preview == "hello");
    assert(chat.photo.id == 9);
    assert(chat.photo.is_downloaded);
    assert(chat.is_muted);
    assert(!chat.is_private);
}

void test_private_and_group_chat_types()
{
    const struct {
        const char* json;
        bool is_private;
    } cases[] = {
        {R"({"@type":"chat","id":1,"type":{"@type":"chatTypePrivate","user_id":42}})", true},
        {R"({"@type":"chat","id":2,"type":{"@type":"chatTypeSecret","secret_chat_id":43,"user_id":42}})", true},
        {R"({"@type":"chat","id":3,"type":{"@type":"chatTypeBasicGroup","basic_group_id":44}})", false},
        {R"({"@type":"chat","id":4,"type":{"@type":"chatTypeSupergroup","supergroup_id":45,"is_channel":false}})", false},
        {R"({"@type":"chat","id":5,"type":{"@type":"chatTypeSupergroup","supergroup_id":46,"is_channel":true}})", false},
        {R"({"@type":"chat","id":6,"type":{"@type":"chatTypeFuture"}})", false},
    };
    for (const auto& item : cases) {
        assert(telegram::tdlib_test::map_chat(item.json).is_private == item.is_private);
    }
}

void test_text_photo_and_voice_messages()
{
    const auto text = telegram::tdlib_test::map_message(
        R"({"@type":"message","id":9007199254740993,"chat_id":-1001234567890123,"sender_id":{"@type":"messageSenderUser","user_id":7000000001},"date":1900000001,"is_outgoing":true,"self_destruct_in":0.000000,"content":{"@type":"messageText","text":{"@type":"formattedText","text":"hello","entities":[]}}})");
    assert(text.id == 9007199254740993LL);
    assert(text.chat_id == -1001234567890123LL);
    assert(text.sender_id == 7000000001LL);
    assert(text.kind == telegram::MessageKind::Text);
    assert(text.text == "hello");
    assert(text.sender_name == "You");

    const auto photo = telegram::tdlib_test::map_message(
        R"({"@type":"message","id":2,"chat_id":3,"date":4,"content":{"@type":"messagePhoto","caption":{"@type":"formattedText","text":"caption","entities":[]},"photo":{"@type":"photo","sizes":[{"@type":"photoSize","width":90,"height":90,"photo":{"@type":"file","id":10,"size":100}},{"@type":"photoSize","width":1280,"height":720,"photo":{"@type":"file","id":11,"size":2000,"local":{"path":"/data/full.jpg","downloaded_size":2000,"is_downloading_active":false,"is_downloading_completed":true}}}]}}})");
    assert(photo.kind == telegram::MessageKind::Photo);
    assert(photo.text == "caption");
    assert(photo.file.id == 11);
    assert(photo.width == 1280);
    assert(photo.height == 720);

    const auto voice = telegram::tdlib_test::map_message(
        R"({"@type":"message","id":5,"chat_id":3,"date":6,"sending_state":{"@type":"messageSendingStatePending"},"content":{"@type":"messageVoiceNote","caption":{"@type":"formattedText","text":"","entities":[]},"voice_note":{"@type":"voiceNote","duration":19,"voice":{"@type":"file","id":12,"size":10000,"local":{"path":"/data/voice.oga","downloaded_size":0,"is_downloading_active":false,"is_downloading_completed":false}}}}})");
    assert(voice.kind == telegram::MessageKind::Voice);
    assert(voice.duration_seconds == 19);
    assert(voice.file.id == 12);
    assert(voice.is_sending);
}

void test_photo_size_selection_prefers_smallest_target()
{
    const auto photo = telegram::tdlib_test::map_message(
        R"({"@type":"message","id":20,"chat_id":3,"date":4,"content":{"@type":"messagePhoto","caption":{"@type":"formattedText","text":"","entities":[]},"photo":{"@type":"photo","sizes":[{"@type":"photoSize","width":1024,"height":768,"photo":{"@type":"file","id":21,"size":1000}},{"@type":"photoSize","width":640,"height":480,"photo":{"@type":"file","id":22,"size":500}},{"@type":"photoSize","width":720,"height":480,"photo":{"@type":"file","id":23,"size":700}},{"@type":"photoSize","width":1920,"height":1080,"photo":{"@type":"file","id":24,"size":2000}}]}}})");

    assert(photo.kind == telegram::MessageKind::Photo);
    assert(photo.file.id == 23);
    assert(photo.width == 720);
    assert(photo.height == 480);
}

void test_sensitive_message_content_is_not_revealed()
{
    const auto text = telegram::tdlib_test::map_message(
        R"({"@type":"message","id":"9007199254740993","chat_id":"-1001234567890123","content":{"@type":"messageText","text":{"@type":"formattedText","text":"visible secret","entities":[{"@type":"textEntity","offset":0,"length":14,"type":{"@type":"textEntityTypeSpoiler"}}]}}})");
    assert(text.kind == telegram::MessageKind::Text);
    assert(text.text == "Spoiler text is hidden");
    assert(text.text.find("visible secret") == std::string::npos);

    const auto photo = telegram::tdlib_test::map_message(
        R"({"@type":"message","id":"2","chat_id":"3","content":{"@type":"messagePhoto","has_spoiler":true,"caption":{"@type":"formattedText","text":"secret caption","entities":[]},"photo":{"@type":"photo","sizes":[{"@type":"photoSize","width":720,"height":480,"photo":{"@type":"file","id":23,"size":700}}]}}})");
    assert(photo.kind == telegram::MessageKind::Unsupported);
    assert(photo.file.id == 0);
    assert(photo.text == "Spoiler photo is hidden");

    const auto self_destructing = telegram::tdlib_test::map_message(
        R"({"@type":"message","id":"5","chat_id":"3","self_destruct_type":{"@type":"messageSelfDestructTypeImmediately"},"self_destruct_in":9.500000,"content":{"@type":"messageVoiceNote","voice_note":{"@type":"voiceNote","duration":5,"voice":{"@type":"file","id":12,"size":1000}}}})");
    assert(self_destructing.kind == telegram::MessageKind::Unsupported);
    assert(self_destructing.file.id == 0);
    assert(self_destructing.text == "Self-destructing message is hidden");

    const auto chat = telegram::tdlib_test::map_chat(
        R"({"@type":"chat","id":"7","title":"Private","positions":[],"last_message":{"@type":"message","id":"8","date":9,"self_destruct_type":{"@type":"messageSelfDestructTypeTimer","self_destruct_time":10},"content":{"@type":"messageText","text":{"@type":"formattedText","text":"preview secret","entities":[]}}}})");
    assert(chat.last_message_preview == "Self-destructing message");

    const auto restricted = telegram::tdlib_test::map_message(
        R"({"@type":"message","id":"6","chat_id":"3","restriction_info":{"@type":"restrictionInfo","restriction_reason":"Unavailable in your region","has_sensitive_content":false},"content":{"@type":"messageText","text":{"@type":"formattedText","text":"restricted secret","entities":[]}}})");
    assert(restricted.kind == telegram::MessageKind::Unsupported);
    assert(restricted.file.id == 0);
    assert(restricted.content_hidden);
    assert(restricted.text == "Restricted message is hidden");
    assert(restricted.text.find("restricted secret") == std::string::npos);

    const auto sensitive = telegram::tdlib_test::map_message(
        R"({"@type":"message","id":"7","chat_id":"3","restriction_info":{"@type":"restrictionInfo","restriction_reason":"","has_sensitive_content":true},"content":{"@type":"messagePhoto","caption":{"@type":"formattedText","text":"sensitive caption","entities":[]},"photo":{"@type":"photo","sizes":[{"@type":"photoSize","width":720,"height":480,"photo":{"@type":"file","id":24,"size":700}}]}}})");
    assert(sensitive.kind == telegram::MessageKind::Unsupported);
    assert(sensitive.file.id == 0);
    assert(sensitive.content_hidden);
    assert(sensitive.text == "Sensitive message is hidden");

    const auto restricted_chat = telegram::tdlib_test::map_chat(
        R"({"@type":"chat","id":"9","title":"Restricted","positions":[],"last_message":{"@type":"message","id":"10","date":11,"restriction_info":{"@type":"restrictionInfo","restriction_reason":"copyright","has_sensitive_content":false},"content":{"@type":"messageText","text":{"@type":"formattedText","text":"preview secret","entities":[]}}}})");
    assert(restricted_chat.last_message_preview == "Restricted message");
    assert(restricted_chat.last_message_preview.find("preview secret") == std::string::npos);
}

void test_ephemeral_content_replaces_regular_content()
{
    const auto message = telegram::tdlib_test::map_message(
        R"({"@type":"message","id":"12","chat_id":"3","content":{"@type":"messageText","text":{"@type":"formattedText","text":"regular content","entities":[]}},"ephemeral_content":{"@type":"ephemeralMessageContent","can_be_saved":false,"has_timestamped_media":false,"content":{"@type":"messageText","text":{"@type":"formattedText","text":"ephemeral content","entities":[]}},"reply_markup":null}})");
    assert(message.kind == telegram::MessageKind::Text);
    assert(message.text == "ephemeral content");
    assert(message.text.find("regular content") == std::string::npos);

    const auto chat = telegram::tdlib_test::map_chat(
        R"({"@type":"chat","id":"13","title":"Ephemeral","positions":[],"last_message":{"@type":"message","id":"14","date":15,"content":{"@type":"messageText","text":{"@type":"formattedText","text":"regular preview","entities":[]}},"ephemeral_content":{"@type":"ephemeralMessageContent","content":{"@type":"messageText","text":{"@type":"formattedText","text":"ephemeral preview","entities":[]}}}}})");
    assert(chat.last_message_preview == "ephemeral preview");

    const auto malformed = telegram::tdlib_test::map_message(
        R"({"@type":"message","id":"16","chat_id":"3","content":{"@type":"messageText","text":{"@type":"formattedText","text":"must stay hidden","entities":[]}},"ephemeral_content":{"@type":"ephemeralMessageContent"}})");
    assert(malformed.kind == telegram::MessageKind::Unsupported);
    assert(malformed.text.empty());
}

void test_tdlib_bytes_are_base64_encoded()
{
    assert(telegram::tdlib_test::encode_bytes("").empty());
    assert(telegram::tdlib_test::encode_bytes("a") == "YQ==");
    assert(telegram::tdlib_test::encode_bytes("ab") == "YWI=");
    assert(telegram::tdlib_test::encode_bytes("database-key") == "ZGF0YWJhc2Uta2V5");
    const std::string binary{"\0\xff\x10", 3};
    assert(telegram::tdlib_test::encode_bytes(binary) == "AP8Q");
}

void test_sensitive_tdlib_error_is_not_displayed()
{
    assert(telegram::tdlib_test::sanitize_error_detail(400, "PHONE_CODE_INVALID") == "PHONE_CODE_INVALID");
    assert(telegram::tdlib_test::sanitize_error_detail(406, "must not be displayed") ==
           "request was rejected by Telegram");
}

void test_load_chats_uses_the_update_driven_api()
{
    const auto request = support::json::parse(telegram::tdlib_test::load_chats_request(1000));
    assert(request.at("@type").as_string() == "loadChats");
    assert(request.at("limit").as_int64() == 100);
    assert(request.at("chat_list").at("@type").as_string() == "chatListMain");
}

void test_initialization_request_starts_tdlib_updates()
{
    const auto request = support::json::parse(telegram::tdlib_test::initialization_request());
    assert(request.at("@type").as_string() == "getOption");
    assert(request.at("name").as_string() == "version");
}

void test_download_request_reports_final_failure()
{
    const auto request = support::json::parse(telegram::tdlib_test::download_file_request(42, 100));
    assert(request.at("@type").as_string() == "downloadFile");
    assert(request.at("file_id").as_int64() == 42);
    assert(request.at("priority").as_int64() == 32);
    assert(request.at("synchronous").as_bool());
}

void test_storage_optimizer_uses_writable_option()
{
    const auto request = support::json::parse(telegram::tdlib_test::storage_optimizer_request());
    assert(request.at("@type").as_string() == "setOption");
    assert(request.at("name").as_string() == "use_storage_optimizer");
    assert(request.at("value").at("@type").as_string() == "optionValueBoolean");
    assert(request.at("value").at("value").as_bool());
}

void test_int64_arrays_accept_tdlib_decimal_strings()
{
    const auto values =
        telegram::tdlib_test::decode_integer_array(R"(["9007199254740993",-1001234567890123,"invalid",null])");
    assert(values.size() == 2);
    assert(values[0] == 9007199254740993LL);
    assert(values[1] == -1001234567890123LL);
}

void test_chat_is_removed_when_positions_leave_the_main_list()
{
    assert(telegram::tdlib_test::chat_leaves_main_list(9223372036854775000LL, 0));
    assert(!telegram::tdlib_test::chat_leaves_main_list(0, 0));
    assert(!telegram::tdlib_test::chat_leaves_main_list(0, 100));
    assert(!telegram::tdlib_test::chat_leaves_main_list(100, 200));
}

void test_missing_tdlib_reports_worker_error()
{
    const char *previous    = std::getenv("LILYGO_UI_TDJSON_LIBRARY");
    const std::string saved = previous ? previous : "";
    const bool had_previous = previous != nullptr;
    assert(setenv("LILYGO_UI_TDJSON_LIBRARY", "/definitely/missing/libtdjson-for-test.so", 1) == 0);

    auto client = telegram::make_tdlib_client();
    telegram::ClientConfig config;
    config.api_id                = 12345;
    config.api_hash              = "test-api-hash";
    const std::thread::id caller = std::this_thread::get_id();
    std::vector<telegram::ClientEvent> events;
    std::thread::id callback_thread;
    const bool started = client->start(config, [&](telegram::ClientEvent event) {
        callback_thread = std::this_thread::get_id();
        events.push_back(std::move(event));
    });
    assert(!started);
    assert(events.size() == 1);
    assert(events.front().kind == telegram::ClientEventKind::Error);
    assert(events.front().text.find("LILYGO_UI_TDJSON_LIBRARY") != std::string::npos);
    assert(callback_thread != caller);

    if (had_previous) {
        assert(setenv("LILYGO_UI_TDJSON_LIBRARY", saved.c_str(), 1) == 0);
    } else {
        assert(unsetenv("LILYGO_UI_TDJSON_LIBRARY") == 0);
    }
}

void test_sender_avatars_keep_user_and_chat_identity_separate()
{
    const std::string user = R"({"@type":"user","id":42,"first_name":"Alice","last_name":"User","profile_photo":{"@type":"profilePhoto","small":{"@type":"file","id":101,"size":512,"local":{"path":"/data/alice.jpg","downloaded_size":512,"is_downloading_completed":true}},"big":{"@type":"file","id":102}}})";
    const std::string chat = R"({"@type":"chat","id":42,"title":"Channel","photo":{"@type":"chatPhotoInfo","small":{"@type":"file","id":201,"size":1024,"local":{"path":"/data/channel.jpg","downloaded_size":1024,"is_downloading_completed":true}}}})";
    const char* user_message = R"({"@type":"message","id":1,"chat_id":99,"sender_id":{"@type":"messageSenderUser","user_id":42},"content":{"@type":"messageText","text":{"text":"User message","entities":[]}}})";
    const char* chat_message = R"({"@type":"message","id":2,"chat_id":99,"sender_id":{"@type":"messageSenderChat","chat_id":42},"content":{"@type":"messageText","text":{"text":"Channel message","entities":[]}}})";
    auto from_user = telegram::tdlib_test::map_message_with_senders(user_message, {user}, {chat});
    auto from_chat = telegram::tdlib_test::map_message_with_senders(chat_message, {user}, {chat});
    assert(!from_user.sender_is_chat && from_user.sender_name == "Alice User");
    assert(from_user.sender_photo.id == 101 && from_user.sender_photo.is_downloaded);
    assert(from_user.sender_photo.local_path == "/data/alice.jpg");
    assert(from_chat.sender_is_chat && from_chat.sender_name == "Channel");
    assert(from_chat.sender_photo.id == 201 && from_chat.sender_photo.local_path == "/data/channel.jpg");
    const auto missing_user = telegram::tdlib_test::map_message_with_senders(user_message, {}, {chat});
    const auto missing_chat = telegram::tdlib_test::map_message_with_senders(chat_message, {user}, {});
    assert(missing_user.sender_name.empty() && missing_user.sender_photo.id == 0);
    assert(missing_chat.sender_name.empty() && missing_chat.sender_photo.id == 0);

    const char* user_update = R"({"@type":"updateUser","user":{"id":42,"first_name":"Renamed","profile_photo":{"small":{"id":103,"expected_size":1000,"local":{"is_downloading_active":true}}}}})";
    assert(!telegram::tdlib_test::apply_sender_update(from_chat, user_update));
    assert(telegram::tdlib_test::apply_sender_update(from_user, user_update));
    assert(from_user.sender_name == "Renamed" && from_user.sender_photo.id == 103);
    assert(from_user.sender_photo.size == 1000 && from_user.sender_photo.is_downloading);
    assert(telegram::tdlib_test::apply_sender_update(from_chat, R"({"@type":"updateChatPhoto","chat_id":42,"photo":{"small":{"id":202,"size":50}}})"));
    assert(from_chat.sender_photo.id == 202);
    assert(!telegram::tdlib_test::apply_sender_update(from_user, R"({"@type":"updateChatPhoto","chat_id":42,"photo":null})"));
    assert(telegram::tdlib_test::apply_sender_update(from_chat, R"({"@type":"updateChatTitle","chat_id":42,"title":"Renamed Channel"})"));
    assert(from_chat.sender_name == "Renamed Channel");
    assert(telegram::tdlib_test::apply_sender_update(from_chat, R"({"@type":"updateChatPhoto","chat_id":42,"photo":null})"));
    assert(from_chat.sender_photo.id == 0 && from_chat.sender_photo.local_path.empty());
    assert(telegram::tdlib_test::apply_sender_update(from_user, R"({"@type":"updateUser","user":{"id":42,"first_name":"Renamed","profile_photo":null}})"));
    assert(from_user.sender_photo.id == 0 && from_user.sender_photo.local_path.empty());
}

void test_sender_avatar_arrives_after_message_and_outgoing_name_is_stable()
{
    auto message = telegram::tdlib_test::map_message(R"({"@type":"message","id":1,"chat_id":2,"sender_id":{"@type":"messageSenderUser","user_id":55}})");
    assert(message.sender_photo.id == 0 && message.sender_name.empty());
    const auto update = R"({"@type":"updateUser","user":{"id":55,"first_name":"Later","profile_photo":{"small":{"id":303,"local":{"path":"/data/later.jpg","is_downloading_completed":true}}}}})";
    assert(telegram::tdlib_test::apply_sender_update(message, update));
    assert(message.sender_name == "Later" && message.sender_photo.id == 303);
    assert(!telegram::tdlib_test::apply_sender_update(message, update));
    auto outgoing = telegram::tdlib_test::map_message(R"({"@type":"message","id":2,"chat_id":2,"is_outgoing":true,"sender_id":{"@type":"messageSenderUser","user_id":55}})");
    assert(telegram::tdlib_test::apply_sender_update(outgoing, update));
    assert(outgoing.sender_name == "You" && outgoing.sender_photo.id == 303);
}

}  // namespace

int main()
{
    test_authorization_and_connection_states();
    test_qr_login_request();
    test_file_mapping();
    test_chat_mapping_preserves_int64();
    test_private_and_group_chat_types();
    test_text_photo_and_voice_messages();
    test_photo_size_selection_prefers_smallest_target();
    test_sensitive_message_content_is_not_revealed();
    test_ephemeral_content_replaces_regular_content();
    test_tdlib_bytes_are_base64_encoded();
    test_sensitive_tdlib_error_is_not_displayed();
    test_load_chats_uses_the_update_driven_api();
    test_initialization_request_starts_tdlib_updates();
    test_download_request_reports_final_failure();
    test_storage_optimizer_uses_writable_option();
    test_int64_arrays_accept_tdlib_decimal_strings();
    test_chat_is_removed_when_positions_leave_the_main_list();
    test_missing_tdlib_reports_worker_error();
    test_sender_avatars_keep_user_and_chat_identity_separate();
    test_sender_avatar_arrives_after_message_and_outgoing_name_is_stable();
    return 0;
}
