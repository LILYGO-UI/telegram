#include "services/telegram/telegram_client.hpp"

#include "services/telegram/tdlib_client_test.hpp"
#include "services/telegram/tdlib_account_protocol.hpp"
#include "support/json.hpp"

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <dlfcn.h>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace telegram {
namespace {

using Json = support::json::Value;

constexpr std::size_t kCommandCapacity        = 128;
constexpr std::size_t kMaximumChats           = 256;
constexpr std::size_t kMaximumMessages        = 512;
constexpr std::size_t kMaximumUsers           = 512;
constexpr std::size_t kMaximumPendingRequests = 256;
constexpr double kReceiveTimeoutSeconds       = 0.05;
constexpr auto kCloseTimeout                  = std::chrono::seconds(5);

bool leaves_main_chat_list(std::int64_t previous_order, std::int64_t current_order)
{
    return previous_order != 0 && current_order == 0;
}

Json make_object(const char *type)
{
    Json::Object object;
    object.emplace("@type", Json(type));
    return Json(std::move(object));
}

void put(Json &object, const char *key, Json value)
{
    object.as_object().insert_or_assign(key, std::move(value));
}

Json build_load_chats_request(std::int32_t limit)
{
    Json request = make_object("loadChats");
    put(request, "chat_list", make_object("chatListMain"));
    put(request, "limit", Json(std::clamp(limit, 1, 100)));
    return request;
}

Json build_initialization_request()
{
    Json request = make_object("getOption");
    put(request, "name", Json("version"));
    return request;
}

Json build_download_file_request(std::int32_t file_id, std::int32_t priority)
{
    Json request = make_object("downloadFile");
    put(request, "file_id", Json(file_id));
    put(request, "priority", Json(std::clamp(priority, 1, 32)));
    put(request, "offset", Json(0));
    put(request, "limit", Json(0));
    put(request, "synchronous", Json(true));
    return request;
}

Json build_storage_optimizer_request()
{
    Json value = make_object("optionValueBoolean");
    put(value, "value", Json(true));

    Json request = make_object("setOption");
    put(request, "name", Json("use_storage_optimizer"));
    put(request, "value", std::move(value));
    return request;
}

const Json *field(const Json &object, std::string_view key)
{
    if (!object.is_object()) return nullptr;
    return object.find(key);
}

std::string string_or(const Json &object, std::string_view key, std::string fallback = {})
{
    const Json *value = field(object, key);
    return value && value->is_string() ? value->as_string() : std::move(fallback);
}

std::optional<std::int64_t> integer_value(const Json &value)
{
    if (value.is_number()) return value.as_int64();
    if (!value.is_string()) return std::nullopt;

    const std::string &encoded = value.as_string();
    std::int64_t parsed        = 0;
    const auto result          = std::from_chars(encoded.data(), encoded.data() + encoded.size(), parsed);
    if (result.ec != std::errc{} || result.ptr != encoded.data() + encoded.size()) return std::nullopt;
    return parsed;
}

std::int64_t integer_or(const Json &object, std::string_view key, std::int64_t fallback = 0)
{
    const Json *value = field(object, key);
    if (!value) return fallback;
    const auto parsed = integer_value(*value);
    return parsed.value_or(fallback);
}

std::vector<std::int64_t> integer_array(const Json *values)
{
    std::vector<std::int64_t> result;
    if (!values || !values->is_array()) return result;
    result.reserve(values->as_array().size());
    for (const Json &value : values->as_array()) {
        if (const auto parsed = integer_value(value)) result.push_back(*parsed);
    }
    return result;
}

std::string encode_tdlib_bytes(std::string_view bytes)
{
    static constexpr char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string encoded;
    encoded.reserve(((bytes.size() + 2U) / 3U) * 4U);

    std::size_t offset = 0;
    while (offset + 3U <= bytes.size()) {
        const auto first  = static_cast<unsigned char>(bytes[offset]);
        const auto second = static_cast<unsigned char>(bytes[offset + 1U]);
        const auto third  = static_cast<unsigned char>(bytes[offset + 2U]);
        encoded.push_back(alphabet[first >> 2U]);
        encoded.push_back(alphabet[((first & 0x03U) << 4U) | (second >> 4U)]);
        encoded.push_back(alphabet[((second & 0x0fU) << 2U) | (third >> 6U)]);
        encoded.push_back(alphabet[third & 0x3fU]);
        offset += 3U;
    }

    const std::size_t remaining = bytes.size() - offset;
    if (remaining != 0) {
        const auto first = static_cast<unsigned char>(bytes[offset]);
        encoded.push_back(alphabet[first >> 2U]);
        if (remaining == 1U) {
            encoded.push_back(alphabet[(first & 0x03U) << 4U]);
            encoded.append("==");
        } else {
            const auto second = static_cast<unsigned char>(bytes[offset + 1U]);
            encoded.push_back(alphabet[((first & 0x03U) << 4U) | (second >> 4U)]);
            encoded.push_back(alphabet[(second & 0x0fU) << 2U]);
            encoded.push_back('=');
        }
    }
    return encoded;
}

std::string safe_tdlib_error_detail(std::int64_t code, std::string detail)
{
    return code == 406 ? "request was rejected by Telegram" : std::move(detail);
}

bool bool_or(const Json &object, std::string_view key, bool fallback = false)
{
    const Json *value = field(object, key);
    return value && value->is_bool() ? value->as_bool() : fallback;
}

std::int32_t narrow_int32(std::int64_t value, const char *field_name)
{
    if (value < std::numeric_limits<std::int32_t>::min() || value > std::numeric_limits<std::int32_t>::max()) {
        throw std::out_of_range(std::string("TDLib field '") + field_name + "' is outside int32 range");
    }
    return static_cast<std::int32_t>(value);
}

std::string td_type(const Json &object)
{
    return string_or(object, "@type");
}

bool formatted_text_has_spoiler(const Json *value)
{
    if (!value || !value->is_object()) return false;
    const Json *entities = field(*value, "entities");
    if (!entities || !entities->is_array()) return false;
    return std::any_of(entities->as_array().cbegin(), entities->as_array().cend(), [](const Json &entity) {
        if (!entity.is_object()) return false;
        const Json *type = field(entity, "type");
        return type && type->is_object() && td_type(*type) == "textEntityTypeSpoiler";
    });
}

std::string formatted_text(const Json *value)
{
    if (formatted_text_has_spoiler(value)) return "Spoiler text is hidden";
    return value && value->is_object() ? string_or(*value, "text") : std::string{};
}

bool is_main_chat_list(const Json *list)
{
    return !list || list->is_null() || (list->is_object() && td_type(*list) == "chatListMain");
}

std::optional<std::int64_t> main_order(const Json *positions)
{
    if (!positions || !positions->is_array()) return std::nullopt;
    for (const Json &position : positions->as_array()) {
        if (!position.is_object() || !is_main_chat_list(field(position, "list"))) continue;
        return integer_or(position, "order");
    }
    return std::int64_t{0};
}

FileInfo parse_file_value(const Json &value)
{
    if (!value.is_object()) throw std::invalid_argument("TDLib file must be a JSON object");

    FileInfo file;
    file.id   = narrow_int32(integer_or(value, "id"), "file.id");
    file.size = integer_or(value, "size");
    if (file.size == 0) file.size = integer_or(value, "expected_size");

    const Json *local = field(value, "local");
    if (local && local->is_object()) {
        file.local_path      = string_or(*local, "path");
        file.downloaded_size = integer_or(*local, "downloaded_size");
        file.is_downloading  = bool_or(*local, "is_downloading_active");
        file.is_downloaded   = bool_or(*local, "is_downloading_completed");
    }
    return file;
}

std::string content_preview(const Json *content)
{
    if (!content || !content->is_object()) return {};
    const std::string type = td_type(*content);
    if (type == "messageText") return formatted_text(field(*content, "text"));
    if (type == "messagePhoto") {
        if (bool_or(*content, "has_spoiler") || bool_or(*content, "is_secret")) return "Spoiler photo";
        const std::string caption = formatted_text(field(*content, "caption"));
        return caption.empty() ? "Photo" : caption;
    }
    if (type == "messageVoiceNote") {
        const std::string caption = formatted_text(field(*content, "caption"));
        return caption.empty() ? "Voice message" : caption;
    }
    if (type == "messageAnimation") return "Animation";
    if (type == "messageAudio") return "Audio";
    if (type == "messageDocument") return "Document";
    if (type == "messageSticker") return "Sticker";
    if (type == "messageVideo") return "Video";
    return "Unsupported message";
}

enum class MessageRestriction {
    None,
    Restricted,
    Sensitive,
};

MessageRestriction message_restriction(const Json &message)
{
    const Json *restriction = field(message, "restriction_info");
    if (!restriction || !restriction->is_object()) return MessageRestriction::None;
    if (!string_or(*restriction, "restriction_reason").empty()) return MessageRestriction::Restricted;
    if (bool_or(*restriction, "has_sensitive_content")) return MessageRestriction::Sensitive;
    return MessageRestriction::None;
}

const Json *displayed_message_content(const Json &message)
{
    const Json *ephemeral = field(message, "ephemeral_content");
    if (!ephemeral || ephemeral->is_null()) return field(message, "content");
    if (!ephemeral->is_object()) return nullptr;
    return field(*ephemeral, "content");
}

std::string message_preview(const Json *message)
{
    if (!message || !message->is_object()) return {};
    const Json *self_destruct = field(*message, "self_destruct_type");
    if (self_destruct && !self_destruct->is_null()) return "Self-destructing message";
    switch (message_restriction(*message)) {
        case MessageRestriction::Restricted:
            return "Restricted message";
        case MessageRestriction::Sensitive:
            return "Sensitive message";
        case MessageRestriction::None:
            break;
    }
    return content_preview(displayed_message_content(*message));
}

FileInfo parse_chat_photo(const Json *photo)
{
    if (!photo || !photo->is_object()) return {};
    const Json *small = field(*photo, "small");
    if (small && small->is_object()) return parse_file_value(*small);
    const Json *big = field(*photo, "big");
    return big && big->is_object() ? parse_file_value(*big) : FileInfo{};
}

Chat parse_chat_value(const Json &value)
{
    if (!value.is_object()) throw std::invalid_argument("TDLib chat must be a JSON object");

    Chat chat;
    chat.id           = integer_or(value, "id");
    chat.title        = string_or(value, "title");
    chat.unread_count = narrow_int32(integer_or(value, "unread_count"), "chat.unread_count");
    chat.photo        = parse_chat_photo(field(value, "photo"));
    const Json *type = field(value, "type");
    if (type && type->is_object()) {
        const auto chat_type = td_type(*type);
        chat.is_private = chat_type == "chatTypePrivate" || chat_type == "chatTypeSecret";
    }

    if (const auto order = main_order(field(value, "positions"))) chat.order = *order;

    const Json *last_message = field(value, "last_message");
    if (last_message && last_message->is_object()) {
        chat.last_message_id      = integer_or(*last_message, "id");
        chat.last_message_date    = integer_or(*last_message, "date");
        chat.last_message_preview = message_preview(last_message);
    }

    const Json *notification_settings = field(value, "notification_settings");
    if (notification_settings && notification_settings->is_object()) {
        chat.is_muted = integer_or(*notification_settings, "mute_for") > 0;
    }
    return chat;
}

std::int64_t parse_sender_id(const Json *sender)
{
    if (!sender || !sender->is_object()) return 0;
    const std::string type = td_type(*sender);
    if (type == "messageSenderUser") return integer_or(*sender, "user_id");
    if (type == "messageSenderChat") return integer_or(*sender, "chat_id");
    return 0;
}

struct SelectedPhoto {
    FileInfo file;
    std::int32_t width  = 0;
    std::int32_t height = 0;
};

SelectedPhoto select_photo(const Json *photo)
{
    SelectedPhoto selected;
    if (!photo || !photo->is_object()) return selected;
    const Json *sizes = field(*photo, "sizes");
    if (!sizes || !sizes->is_array()) return selected;

    constexpr std::int32_t kPreferredPhotoEdge = 720;
    std::int64_t selected_area                 = -1;
    bool selected_meets_target                 = false;
    for (const Json &size : sizes->as_array()) {
        if (!size.is_object()) continue;
        const Json *file = field(size, "photo");
        if (!file || !file->is_object()) continue;
        const std::int32_t width  = narrow_int32(integer_or(size, "width"), "photo.width");
        const std::int32_t height = narrow_int32(integer_or(size, "height"), "photo.height");
        const std::int64_t area   = static_cast<std::int64_t>(std::max(width, 0)) * std::max(height, 0);
        const bool meets_target   = std::max(width, height) >= kPreferredPhotoEdge;
        if (selected_area >= 0) {
            if (selected_meets_target && (!meets_target || area >= selected_area)) continue;
            if (!selected_meets_target && !meets_target && area <= selected_area) continue;
        }
        selected_area         = area;
        selected_meets_target = meets_target;
        selected.file         = parse_file_value(*file);
        selected.width        = width;
        selected.height       = height;
    }
    return selected;
}

struct UserSender {
    std::string name;
    FileInfo photo;
};

using UserCache = std::unordered_map<std::int64_t, UserSender>;
using ChatCache = std::unordered_map<std::int64_t, Chat>;

std::string user_display_name(const Json &user);

UserSender parse_user_sender(const Json& user)
{
    return {user_display_name(user), parse_chat_photo(field(user, "profile_photo"))};
}

bool update_message_sender(Message& message, bool is_chat, std::int64_t sender_id,
                           const std::string* name, const FileInfo* photo)
{
    if (message.sender_id != sender_id || message.sender_is_chat != is_chat) return false;
    bool changed = false;
    if (name && !message.is_outgoing && message.sender_name != *name) {
        message.sender_name = *name;
        changed = true;
    }
    if (photo && message.sender_photo != *photo) {
        message.sender_photo = *photo;
        changed = true;
    }
    return changed;
}

Message parse_message_value(const Json &value, const UserCache *users = nullptr, const ChatCache *chats = nullptr)
{
    if (!value.is_object()) throw std::invalid_argument("TDLib message must be a JSON object");

    Message message;
    message.id          = integer_or(value, "id");
    message.chat_id     = integer_or(value, "chat_id");
    message.sender_id   = parse_sender_id(field(value, "sender_id"));
    const Json* sender = field(value, "sender_id");
    message.sender_is_chat = sender && td_type(*sender) == "messageSenderChat";
    message.date        = integer_or(value, "date");
    message.is_outgoing = bool_or(value, "is_outgoing");

    const Json *sending_state = field(value, "sending_state");
    if (sending_state && sending_state->is_object()) {
        const std::string state_type = td_type(*sending_state);
        message.is_sending           = state_type == "messageSendingStatePending";
        message.is_failed            = state_type == "messageSendingStateFailed";
    }

    if (message.is_outgoing) {
        message.sender_name = "You";
    }
    if (!message.sender_is_chat && users) {
        const auto found = users->find(message.sender_id);
        if (found != users->end()) update_message_sender(message, false, message.sender_id, &found->second.name, &found->second.photo);
    }
    if (message.sender_is_chat && chats) {
        const auto found = chats->find(message.sender_id);
        if (found != chats->end()) update_message_sender(message, true, message.sender_id, &found->second.title, &found->second.photo);
    }

    const Json *self_destruct = field(value, "self_destruct_type");
    if (self_destruct && !self_destruct->is_null()) {
        message.text           = "Self-destructing message is hidden";
        message.content_hidden = true;
        return message;
    }

    switch (message_restriction(value)) {
        case MessageRestriction::Restricted:
            message.text           = "Restricted message is hidden";
            message.content_hidden = true;
            return message;
        case MessageRestriction::Sensitive:
            message.text           = "Sensitive message is hidden";
            message.content_hidden = true;
            return message;
        case MessageRestriction::None:
            break;
    }

    const Json *content = displayed_message_content(value);
    if (!content || !content->is_object()) return message;
    const std::string content_type = td_type(*content);

    if (content_type == "messageText") {
        const Json *text       = field(*content, "text");
        message.kind           = MessageKind::Text;
        message.text           = formatted_text(text);
        message.content_hidden = formatted_text_has_spoiler(text);
    } else if (content_type == "messagePhoto") {
        if (bool_or(*content, "has_spoiler") || bool_or(*content, "is_secret")) {
            message.text           = "Spoiler photo is hidden";
            message.content_hidden = true;
            return message;
        }
        message.kind                 = MessageKind::Photo;
        const Json *caption          = field(*content, "caption");
        message.text                 = formatted_text(caption);
        message.content_hidden       = formatted_text_has_spoiler(caption);
        const SelectedPhoto selected = select_photo(field(*content, "photo"));
        message.file                 = selected.file;
        message.width                = selected.width;
        message.height               = selected.height;
    } else if (content_type == "messageVoiceNote") {
        message.kind           = MessageKind::Voice;
        message.text           = formatted_text(field(*content, "caption"));
        const Json *voice_note = field(*content, "voice_note");
        if (voice_note && voice_note->is_object()) {
            message.duration_seconds = narrow_int32(integer_or(*voice_note, "duration"), "voice_note.duration");
            const Json *voice        = field(*voice_note, "voice");
            if (voice && voice->is_object()) message.file = parse_file_value(*voice);
        }
    }
    return message;
}

tdlib_test::AuthMapping parse_authorization_state_value(const Json &state)
{
    if (!state.is_object()) throw std::invalid_argument("TDLib authorization state must be a JSON object");

    tdlib_test::AuthMapping mapping;
    const std::string type = td_type(state);
    if (type == "authorizationStateWaitTdlibParameters") {
        mapping.state = AuthState::WaitingForParameters;
    } else if (type == "authorizationStateWaitPhoneNumber") {
        mapping.state = AuthState::WaitingForPhoneNumber;
    } else if (type == "authorizationStateWaitEmailAddress") {
        mapping.state = AuthState::WaitingForEmailAddress;
    } else if (type == "authorizationStateWaitEmailCode") {
        mapping.state = AuthState::WaitingForEmailCode;
    } else if (type == "authorizationStateWaitCode") {
        mapping.state = AuthState::WaitingForCode;
    } else if (type == "authorizationStateWaitPassword") {
        mapping.state  = AuthState::WaitingForPassword;
        mapping.prompt = string_or(state, "password_hint");
    } else if (type == "authorizationStateWaitRegistration") {
        mapping.state  = AuthState::WaitingForRegistration;
        mapping.prompt = "Telegram registration requires first and last name entry";
    } else if (type == "authorizationStateWaitOtherDeviceConfirmation") {
        mapping.state  = AuthState::WaitingForOtherDeviceConfirmation;
        mapping.prompt = string_or(state, "link");
    } else if (type == "authorizationStateWaitPremiumPurchase") {
        mapping.state  = AuthState::WaitingForPremiumPurchase;
        mapping.prompt = "Complete the required Telegram Premium purchase in an official app";
    } else if (type == "authorizationStateReady") {
        mapping.state = AuthState::Ready;
    } else if (type == "authorizationStateLoggingOut") {
        mapping.state = AuthState::LoggingOut;
    } else if (type == "authorizationStateClosing") {
        mapping.state = AuthState::Closing;
    } else if (type == "authorizationStateClosed") {
        mapping.state = AuthState::Closed;
    } else {
        mapping.state  = AuthState::Error;
        mapping.prompt = type.empty() ? "Unknown TDLib authorization state" : "Unsupported TDLib state: " + type;
    }
    return mapping;
}

ConnectionState parse_connection_state_value(const Json &state)
{
    if (!state.is_object()) throw std::invalid_argument("TDLib connection state must be a JSON object");
    const std::string type = td_type(state);
    if (type == "connectionStateWaitingForNetwork") return ConnectionState::WaitingForNetwork;
    if (type == "connectionStateConnectingToProxy") return ConnectionState::ConnectingToProxy;
    if (type == "connectionStateConnecting") return ConnectionState::Connecting;
    if (type == "connectionStateUpdating") return ConnectionState::Updating;
    if (type == "connectionStateReady") return ConnectionState::Ready;
    return ConnectionState::Unknown;
}

std::string user_display_name(const Json &user)
{
    std::string name            = string_or(user, "first_name");
    const std::string last_name = string_or(user, "last_name");
    if (!name.empty() && !last_name.empty()) name.push_back(' ');
    name += last_name;
    if (!name.empty()) return name;

    const Json *usernames = field(user, "usernames");
    const Json *active    = usernames && usernames->is_object() ? field(*usernames, "active_usernames") : nullptr;
    if (active && active->is_array() && !active->as_array().empty() && active->as_array().front().is_string()) {
        return active->as_array().front().as_string();
    }
    return string_or(user, "phone_number");
}

class DynamicLibrary {
public:
    ~DynamicLibrary()
    {
        if (handle_) dlclose(handle_);
    }

    DynamicLibrary(const DynamicLibrary &)            = delete;
    DynamicLibrary &operator=(const DynamicLibrary &) = delete;
    DynamicLibrary()                                  = default;

    bool open(std::string *error)
    {
        const char *override_path = std::getenv("LILYGO_UI_TDJSON_LIBRARY");
        if (override_path && override_path[0]) {
            handle_ = dlopen(override_path, RTLD_NOW | RTLD_LOCAL);
            if (handle_) return true;
            const char *detail = dlerror();
            *error             = std::string("cannot load TDLib from LILYGO_UI_TDJSON_LIBRARY='") + override_path + "'";
            if (detail && detail[0]) *error += std::string(": ") + detail;
            return false;
        }

        constexpr const char *candidates[] = {
            "libtdjson.so.1",
            "libtdjson.so",
            "libtdjson.so.1.8.38",
            "libtdjson.dylib",
        };
        for (const char *candidate : candidates) {
            dlerror();
            handle_ = dlopen(candidate, RTLD_NOW | RTLD_LOCAL);
            if (handle_) return true;
        }
        const char *detail = dlerror();
        *error             = "TDLib JSON runtime is unavailable; install libtdjson or set LILYGO_UI_TDJSON_LIBRARY";
        if (detail && detail[0]) *error += std::string(": ") + detail;
        return false;
    }

    template <typename Function>
    bool load(Function *function, const char *name, std::string *error)
    {
        static_assert(sizeof(Function) == sizeof(void *), "POSIX function pointers must fit in dlsym pointers");
        dlerror();
        void *address     = dlsym(handle_, name);
        const char *issue = dlerror();
        if (issue || !address) {
            *error = std::string("TDLib runtime is missing required symbol '") + name + "'";
            if (issue && issue[0]) *error += std::string(": ") + issue;
            return false;
        }
        std::memcpy(function, &address, sizeof(address));
        return true;
    }

    // TDLib owns background threads. If it doesn't confirm authorizationStateClosed,
    // keeping the mapping loaded is safer than unmapping code those threads may use.
    void release() noexcept
    {
        handle_ = nullptr;
    }

private:
    void *handle_ = nullptr;
};

struct TdJsonApi {
    using CreateClientId = int (*)();
    using Send           = void (*)(int, const char *);
    using Receive        = const char *(*)(double);

    bool load(std::string *error)
    {
        return library.open(error) && library.load(&create_client_id, "td_create_client_id", error) &&
               library.load(&send, "td_send", error) && library.load(&receive, "td_receive", error);
    }

    DynamicLibrary library;
    CreateClientId create_client_id = nullptr;
    Send send                       = nullptr;
    Receive receive                 = nullptr;
};

enum class PendingKind {
    Generic,
    LoadChats,
    GetChat,
    GetUser,
    GetMessage,
    History,
    SendText,
    DownloadFile,
    SetParameters,
    Account,
};

struct PendingRequest {
    PendingKind kind = PendingKind::Generic;
    std::string operation;
    std::int64_t chat_id       = 0;
    std::int64_t message_id    = 0;
    std::int32_t file_id       = 0;
    std::int32_t target_count  = 0;
    std::uint8_t attempts_left = 0;
    std::uint64_t account_wire_id = 0;
};

struct Command {
    Json request;
    PendingRequest pending;
    std::optional<AccountRequest> account = std::nullopt;
};

class TdlibClient final : public TelegramClient {
public:
    ~TdlibClient() override
    {
        stop();
    }

    bool start(const ClientConfig &config, EventCallback callback) override
    {
        if (!callback) return false;

        std::unique_lock<std::mutex> lock(lifecycle_mutex_);
        if (worker_.joinable() || join_in_progress_) return false;

        std::string error;
        if (config.api_id <= 0 || config.api_hash.empty()) {
            error = "Telegram api_id and api_hash must be configured before starting TDLib";
        } else {
            auto api = std::make_unique<TdJsonApi>();
            if (api->load(&error)) api_ = std::move(api);
        }

        if (!error.empty()) {
            lock.unlock();
            std::thread failure_worker([callback = std::move(callback), error = std::move(error)]() mutable {
                ClientEvent event;
                event.kind = ClientEventKind::Error;
                event.text = error;
                try {
                    callback(std::move(event));
                } catch (...) {
                    std::fputs("[telegram] event callback threw while reporting TDLib startup failure\n", stderr);
                }
            });
            failure_worker.join();
            return false;
        }

        config_   = config;
        callback_ = std::move(callback);
        stop_requested_.store(false, std::memory_order_release);
        worker_active_.store(true, std::memory_order_release);
        next_request_id_.store(1, std::memory_order_relaxed);
        {
            std::lock_guard<std::mutex> command_lock(command_mutex_);
            commands_.clear();
        }
        try {
            worker_ = std::thread(&TdlibClient::worker_main, this);
        } catch (...) {
            stop_requested_.store(true, std::memory_order_release);
            worker_active_.store(false, std::memory_order_release);
            callback_ = {};
            api_.reset();
            throw;
        }
        worker_id_ = worker_.get_id();
        return true;
    }

    void stop() noexcept override
    {
        std::unique_lock<std::mutex> lock(lifecycle_mutex_);
        stop_requested_.store(true, std::memory_order_release);
        if (join_in_progress_) {
            if (std::this_thread::get_id() == worker_id_) return;
            lifecycle_condition_.wait(lock, [this] { return !join_in_progress_; });
            return;
        }
        if (!worker_.joinable()) {
            callback_ = {};
            api_.reset();
            return;
        }
        if (std::this_thread::get_id() == worker_id_) return;

        join_in_progress_  = true;
        std::thread worker = std::move(worker_);
        lock.unlock();
        try {
            worker.join();
        } catch (...) {
            std::fputs("[telegram] failed to join TDLib worker\n", stderr);
        }
        lock.lock();
        callback_ = {};
        api_.reset();
        worker_id_        = {};
        join_in_progress_ = false;
        {
            std::lock_guard<std::mutex> command_lock(command_mutex_);
            commands_.clear();
        }
        lock.unlock();
        lifecycle_condition_.notify_all();
    }

    void submit_phone_number(std::string phone_number) override
    {
        Json request = make_object("setAuthenticationPhoneNumber");
        put(request, "phone_number", Json(std::move(phone_number)));
        Json settings = make_object("phoneNumberAuthenticationSettings");
        put(settings, "allow_flash_call", Json(false));
        put(settings, "allow_missed_call", Json(false));
        put(settings, "is_current_phone_number", Json(false));
        put(settings, "has_unknown_phone_number", Json(false));
        put(settings, "allow_sms_retriever_api", Json(false));
        put(settings, "firebase_authentication_settings", Json(nullptr));
        put(settings, "authentication_tokens", Json(Json::Array{}));
        put(request, "settings", std::move(settings));
        enqueue(std::move(request), {PendingKind::Generic, "submit phone number"});
    }

    void submit_email_address(std::string email_address) override
    {
        Json request = make_object("setAuthenticationEmailAddress");
        put(request, "email_address", Json(std::move(email_address)));
        enqueue(std::move(request), {PendingKind::Generic, "submit email address"});
    }

    void submit_email_code(std::string code) override
    {
        Json authentication = make_object("emailAddressAuthenticationCode");
        put(authentication, "code", Json(std::move(code)));
        Json request = make_object("checkAuthenticationEmailCode");
        put(request, "code", std::move(authentication));
        enqueue(std::move(request), {PendingKind::Generic, "submit email code"});
    }

    void submit_code(std::string code) override
    {
        Json request = make_object("checkAuthenticationCode");
        put(request, "code", Json(std::move(code)));
        enqueue(std::move(request), {PendingKind::Generic, "submit authentication code"});
    }

    void submit_password(std::string password) override
    {
        Json request = make_object("checkAuthenticationPassword");
        put(request, "password", Json(std::move(password)));
        enqueue(std::move(request), {PendingKind::Generic, "submit two-step verification password"});
    }

    void log_out() override
    {
        enqueue(make_object("logOut"), {PendingKind::Generic, "log out"});
    }

    void load_chats(std::int32_t limit) override
    {
        limit = std::clamp(limit, 1, 100);
        PendingRequest pending;
        pending.kind          = PendingKind::LoadChats;
        pending.operation     = "load chats";
        pending.target_count  = limit;
        pending.attempts_left = 5;
        enqueue(build_load_chats_request(limit), std::move(pending));
    }

    void open_chat(std::int64_t chat_id) override
    {
        Json request = make_object("openChat");
        put(request, "chat_id", Json(chat_id));
        enqueue(std::move(request), {PendingKind::Generic, "open chat", chat_id});
    }

    void close_chat(std::int64_t chat_id) override
    {
        Json request = make_object("closeChat");
        put(request, "chat_id", Json(chat_id));
        enqueue(std::move(request), {PendingKind::Generic, "close chat", chat_id});
    }

    void load_history(std::int64_t chat_id, std::int64_t from_message_id, std::int32_t limit) override
    {
        limit        = std::clamp(limit, 1, 100);
        Json request = make_object("getChatHistory");
        put(request, "chat_id", Json(chat_id));
        put(request, "from_message_id", Json(from_message_id));
        put(request, "offset", Json(0));
        put(request, "limit", Json(limit));
        put(request, "only_local", Json(false));
        enqueue(std::move(request), {PendingKind::History, "load chat history", chat_id});
    }

    void view_messages(std::int64_t chat_id, const std::vector<std::int64_t> &message_ids) override
    {
        if (chat_id == 0 || message_ids.empty()) return;
        Json::Array ids;
        ids.reserve(std::min<std::size_t>(message_ids.size(), 100));
        for (const std::int64_t message_id : message_ids) {
            if (message_id != 0) ids.emplace_back(message_id);
            if (ids.size() == 100) break;
        }
        if (ids.empty()) return;

        Json request = make_object("viewMessages");
        put(request, "chat_id", Json(chat_id));
        put(request, "message_ids", Json(std::move(ids)));
        put(request, "source", Json(nullptr));
        put(request, "force_read", Json(false));
        enqueue(std::move(request), {PendingKind::Generic, "mark messages as read", chat_id});
    }

    void open_message_content(std::int64_t chat_id, std::int64_t message_id) override
    {
        if (chat_id == 0 || message_id == 0) return;
        Json request = make_object("openMessageContent");
        put(request, "chat_id", Json(chat_id));
        put(request, "message_id", Json(message_id));
        enqueue(std::move(request), {PendingKind::Generic, "mark message content as opened", chat_id, message_id});
    }

    void send_text(std::int64_t chat_id, std::string text) override
    {
        if (text.empty()) return;
        Json formatted = make_object("formattedText");
        put(formatted, "text", Json(std::move(text)));
        put(formatted, "entities", Json(Json::Array{}));

        Json content = make_object("inputMessageText");
        put(content, "text", std::move(formatted));
        put(content, "link_preview_options", Json(nullptr));
        put(content, "clear_draft", Json(true));

        Json request = make_object("sendMessage");
        put(request, "chat_id", Json(chat_id));
        put(request, "input_message_content", std::move(content));
        enqueue(std::move(request), {PendingKind::SendText, "send text message", chat_id});
    }

    void download_file(std::int32_t file_id, std::int32_t priority) override
    {
        enqueue(build_download_file_request(file_id, priority),
                {PendingKind::DownloadFile, "download file", 0, 0, file_id});
    }

    void account_request(AccountRequest request) override
    {
        enqueue(Json(nullptr), {}, std::move(request));
    }

private:
    void enqueue(Json request, PendingRequest pending, std::optional<AccountRequest> account = std::nullopt)
    {
        if (stop_requested_.load(std::memory_order_acquire) || !worker_active_.load(std::memory_order_acquire)) {
            throw std::runtime_error("TDLib client is not running");
        }
        std::lock_guard<std::mutex> lock(command_mutex_);
        if (stop_requested_.load(std::memory_order_relaxed) || !worker_active_.load(std::memory_order_relaxed)) {
            throw std::runtime_error("TDLib client is stopping");
        }
        if (commands_.size() >= kCommandCapacity) throw std::runtime_error("TDLib command queue is full");
        commands_.push_back({std::move(request), std::move(pending), std::move(account)});
    }

    void emit(ClientEvent event)
    {
        if (stop_requested_.load(std::memory_order_acquire) || !callback_) return;
        try {
            callback_(std::move(event));
        } catch (const std::exception &error) {
            std::fprintf(stderr, "[telegram] event callback threw: %s\n", error.what());
        } catch (...) {
            std::fputs("[telegram] event callback threw an unknown exception\n", stderr);
        }
    }

    void emit_error(std::string text, const PendingRequest *pending = nullptr)
    {
        ClientEvent event;
        event.kind = ClientEventKind::Error;
        event.text = std::move(text);
        if (pending) {
            event.chat_id    = pending->chat_id;
            event.message_id = pending->message_id;
            event.file.id    = pending->file_id;
        }
        emit(std::move(event));
    }

    void emit_status(std::string text)
    {
        ClientEvent event;
        event.kind = ClientEventKind::Status;
        event.text = std::move(text);
        emit(std::move(event));
    }

    std::int64_t next_request_id()
    {
        const std::uint64_t id = next_request_id_.fetch_add(1, std::memory_order_relaxed);
        if (id > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
            throw std::overflow_error("TDLib request identifier space exhausted");
        }
        return static_cast<std::int64_t>(id);
    }

    bool send_now(Command command)
    {
        if (pending_requests_.size() >= kMaximumPendingRequests) {
            emit_error("TDLib pending request limit reached", &command.pending);
            return false;
        }
        const std::int64_t request_id = next_request_id();
        put(command.request, "@extra", Json(request_id));
        const std::string encoded = support::json::stringify(command.request);
        pending_requests_.insert_or_assign(request_id, std::move(command.pending));
        api_->send(client_id_, encoded.c_str());
        return true;
    }

    void request_chat(std::int64_t chat_id)
    {
        if (chat_id == 0 || !pending_chat_fetches_.insert(chat_id).second) return;
        Json request = make_object("getChat");
        put(request, "chat_id", Json(chat_id));
        if (!send_now({std::move(request), {PendingKind::GetChat, "get chat", chat_id}})) {
            pending_chat_fetches_.erase(chat_id);
        }
    }

    void request_user(std::int64_t user_id)
    {
        if (user_id <= 0 || pending_user_fetches_.count(user_id) != 0) return;
        pending_user_fetches_.insert(user_id);
        Json request = make_object("getUser");
        put(request, "user_id", Json(user_id));
        if (!send_now({std::move(request), {PendingKind::GetUser, "get user", user_id}})) pending_user_fetches_.erase(user_id);
    }

    void request_message(std::int64_t chat_id, std::int64_t message_id)
    {
        Json request = make_object("getMessage");
        put(request, "chat_id", Json(chat_id));
        put(request, "message_id", Json(message_id));
        send_now({std::move(request), {PendingKind::GetMessage, "get message", chat_id, message_id}});
    }

    void send_parameters()
    {
        send_now({build_storage_optimizer_request(), {PendingKind::Generic, "enable TDLib storage optimizer"}});

        Json request = make_object("setTdlibParameters");
        put(request, "use_test_dc", Json(false));
        put(request, "database_directory", Json(config_.database_directory));
        put(request, "files_directory", Json(config_.files_directory));
        put(request, "database_encryption_key", Json(encode_tdlib_bytes(config_.database_encryption_key)));
        put(request, "use_file_database", Json(true));
        put(request, "use_chat_info_database", Json(true));
        put(request, "use_message_database", Json(true));
        put(request, "use_secret_chats", Json(false));
        put(request, "api_id", Json(config_.api_id));
        put(request, "api_hash", Json(config_.api_hash));
        put(request, "system_language_code", Json(config_.system_language_code));
        put(request, "device_model", Json(config_.device_model));
        put(request, "system_version", Json("Debian GNU/Linux"));
        put(request, "application_version", Json(config_.application_version));
        send_now({std::move(request), {PendingKind::SetParameters, "configure TDLib"}});
    }

    void drain_commands()
    {
        for (std::size_t count = 0; count < kCommandCapacity; ++count) {
            Command command;
            {
                std::lock_guard<std::mutex> lock(command_mutex_);
                if (commands_.empty()) break;
                command = std::move(commands_.front());
                commands_.pop_front();
            }
            if (command.account && account_) account_->submit(std::move(*command.account));
            else send_now(std::move(command));
        }
    }

    void cache_chat(Chat chat)
    {
        if (chat.id == 0) return;
        if (chats_.find(chat.id) == chats_.end() && chats_.size() >= kMaximumChats) chats_.erase(chats_.begin());
        refresh_sender_messages(true, chat.id, &chat.title, &chat.photo);
        chats_.insert_or_assign(chat.id, std::move(chat));
    }

    void emit_chat(Chat chat)
    {
        cache_chat(chat);
        if (chat.order == 0) {
            emit_chat_removed(chat.id);
            return;
        }
        ClientEvent event;
        event.kind = ClientEventKind::ChatUpserted;
        event.chat = std::move(chat);
        emit(std::move(event));
    }

    void emit_chat_removed(std::int64_t chat_id)
    {
        ClientEvent event;
        event.kind    = ClientEventKind::ChatRemoved;
        event.chat_id = chat_id;
        emit(std::move(event));
    }

    using MessageKey = std::pair<std::int64_t, std::int64_t>;

    void cache_message(Message message)
    {
        const MessageKey key{message.chat_id, message.id};
        if (messages_.find(key) == messages_.end() && messages_.size() >= kMaximumMessages)
            messages_.erase(messages_.begin());
        messages_.insert_or_assign(key, std::move(message));
    }

    void emit_message(Message message, bool is_history = false)
    {
        const auto sender_id = message.sender_id;
        const auto sender_is_chat = message.sender_is_chat;
        cache_message(message);
        ClientEvent event;
        event.kind       = ClientEventKind::MessageUpserted;
        event.message    = std::move(message);
        event.is_history = is_history;
        emit(std::move(event));
        if (sender_is_chat && chats_.count(sender_id) == 0) request_chat(sender_id);
        else if (!sender_is_chat && sender_id > 0 && users_.count(sender_id) == 0) request_user(sender_id);
    }

    void refresh_sender_messages(bool is_chat, std::int64_t sender_id, const std::string* name, const FileInfo* photo)
    {
        for (auto& entry : messages_) {
            if (!update_message_sender(entry.second, is_chat, sender_id, name, photo)) continue;
            ClientEvent event;
            event.kind = ClientEventKind::MessageUpserted;
            event.message = entry.second;
            emit(std::move(event));
        }
    }

    void update_cached_avatar_file(const FileInfo& file)
    {
        if (file.id == 0) return;
        for (auto& entry : users_) if (entry.second.photo.id == file.id) entry.second.photo = file;
        for (auto& entry : chats_) if (entry.second.photo.id == file.id) entry.second.photo = file;
        for (auto& entry : messages_) if (entry.second.sender_photo.id == file.id) entry.second.sender_photo = file;
    }

    void emit_message_removed(std::int64_t chat_id, std::int64_t message_id)
    {
        messages_.erase({chat_id, message_id});
        ClientEvent event;
        event.kind       = ClientEventKind::MessageRemoved;
        event.chat_id    = chat_id;
        event.message_id = message_id;
        emit(std::move(event));
    }

    void handle_authorization_state(const Json &state)
    {
        const tdlib_test::AuthMapping mapping = parse_authorization_state_value(state);
        ClientEvent event;
        event.kind       = ClientEventKind::AuthStateChanged;
        event.auth_state = mapping.state;
        event.text       = mapping.prompt;
        emit(std::move(event));
        if (mapping.state == AuthState::WaitingForParameters && !stop_requested_.load(std::memory_order_acquire)) {
            send_parameters();
        }
    }

    void update_known_chat(std::int64_t chat_id, const std::function<void(Chat &)> &update)
    {
        const auto found = chats_.find(chat_id);
        if (found == chats_.end()) {
            request_chat(chat_id);
            return;
        }
        const std::int64_t previous_order = found->second.order;
        update(found->second);
        if (found->second.order == 0) {
            if (leaves_main_chat_list(previous_order, found->second.order)) emit_chat_removed(chat_id);
            return;
        }
        ClientEvent event;
        event.kind = ClientEventKind::ChatUpserted;
        event.chat = found->second;
        emit(std::move(event));
    }

    void handle_chat_update(const Json &root, const std::string &type)
    {
        const std::int64_t chat_id = integer_or(root, "chat_id");
        if (type == "updateNewChat") {
            const Json *chat = field(root, "chat");
            if (chat && chat->is_object()) emit_chat(parse_chat_value(*chat));
            return;
        }
        if (type == "updateChatRemovedFromList") {
            if (is_main_chat_list(field(root, "chat_list"))) {
                const auto found = chats_.find(chat_id);
                if (found != chats_.end()) found->second.order = 0;
                emit_chat_removed(chat_id);
            }
            return;
        }
        if (type == "updateChatAddedToList") {
            if (is_main_chat_list(field(root, "chat_list"))) request_chat(chat_id);
            return;
        }
        if (type == "updateChatTitle") {
            const auto title = string_or(root, "title");
            refresh_sender_messages(true, chat_id, &title, nullptr);
            update_known_chat(chat_id, [&](Chat &chat) { chat.title = title; });
            return;
        }
        if (type == "updateChatPhoto") {
            const auto photo = parse_chat_photo(field(root, "photo"));
            refresh_sender_messages(true, chat_id, nullptr, &photo);
            update_known_chat(chat_id, [&](Chat &chat) { chat.photo = photo; });
            ClientEvent event;
            event.kind = ClientEventKind::ChatPhotoChanged;
            event.chat_id = chat_id;
            event.file = photo;
            emit(std::move(event));
            return;
        }
        if (type == "updateChatReadInbox") {
            update_known_chat(chat_id, [&](Chat &chat) {
                chat.unread_count = narrow_int32(integer_or(root, "unread_count"), "chat.unread_count");
            });
            return;
        }
        if (type == "updateChatNotificationSettings") {
            update_known_chat(chat_id, [&](Chat &chat) {
                const Json *settings = field(root, "notification_settings");
                chat.is_muted        = settings && settings->is_object() && integer_or(*settings, "mute_for") > 0;
            });
            return;
        }
        if (type == "updateChatLastMessage") {
            update_known_chat(chat_id, [&](Chat &chat) {
                const Json *message = field(root, "last_message");
                if (!message || message->is_null()) {
                    chat.last_message_id   = 0;
                    chat.last_message_date = 0;
                    chat.last_message_preview.clear();
                } else if (message->is_object()) {
                    chat.last_message_id      = integer_or(*message, "id");
                    chat.last_message_date    = integer_or(*message, "date");
                    chat.last_message_preview = message_preview(message);
                }
                if (const auto order = main_order(field(root, "positions"))) chat.order = *order;
            });
            return;
        }
        if (type == "updateChatPosition") {
            const Json *position = field(root, "position");
            if (!position || !position->is_object() || !is_main_chat_list(field(*position, "list"))) return;
            const std::int64_t order = integer_or(*position, "order");
            if (order == 0) {
                const auto found = chats_.find(chat_id);
                if (found != chats_.end()) found->second.order = 0;
                emit_chat_removed(chat_id);
            } else {
                update_known_chat(chat_id, [&](Chat &chat) { chat.order = order; });
            }
            return;
        }
        if (type == "updateChatDraftMessage") {
            update_known_chat(chat_id, [&](Chat &chat) {
                if (const auto order = main_order(field(root, "positions"))) chat.order = *order;
            });
        }
    }

    void handle_user_update(const Json &root)
    {
        const Json *user = field(root, "user");
        if (!user || !user->is_object()) return;
        const std::int64_t user_id = integer_or(*user, "id");
        if (user_id == 0) return;
        if (users_.find(user_id) == users_.end() && users_.size() >= kMaximumUsers) users_.erase(users_.begin());
        const auto sender = parse_user_sender(*user);
        users_.insert_or_assign(user_id, sender);
        refresh_sender_messages(false, user_id, &sender.name, &sender.photo);
    }

    void handle_message_update(const Json &root, const std::string &type)
    {
        if (type == "updateNewMessage") {
            const Json *message = field(root, "message");
            if (message && message->is_object()) emit_message(parse_message_value(*message, &users_, &chats_));
            return;
        }
        if (type == "updateMessageSendSucceeded" || type == "updateMessageSendFailed") {
            const Json *message = field(root, "message");
            if (!message || !message->is_object()) return;
            Message parsed = parse_message_value(*message, &users_, &chats_);
            if (type == "updateMessageSendFailed") {
                parsed.is_failed  = true;
                parsed.is_sending = false;
            }
            const std::int64_t old_id = integer_or(root, "old_message_id");
            if (old_id != 0 && old_id != parsed.id) emit_message_removed(parsed.chat_id, old_id);
            emit_message(std::move(parsed));
            return;
        }
        if (type == "updateMessageContent" || type == "updateMessageEphemeralContent") {
            const std::int64_t chat_id    = integer_or(root, "chat_id");
            const std::int64_t message_id = integer_or(root, "message_id");
            if (chat_id != 0 && message_id != 0) request_message(chat_id, message_id);
            return;
        }
        if (type == "updateMessageSendAcknowledged") {
            const std::int64_t chat_id    = integer_or(root, "chat_id");
            const std::int64_t message_id = integer_or(root, "message_id");
            const auto found              = messages_.find({chat_id, message_id});
            if (found != messages_.end()) {
                found->second.is_sending = true;
                ClientEvent event;
                event.kind    = ClientEventKind::MessageUpserted;
                event.message = found->second;
                emit(std::move(event));
            }
            return;
        }
        if (type == "updateDeleteMessages") {
            const std::int64_t chat_id = integer_or(root, "chat_id");
            for (const std::int64_t message_id : integer_array(field(root, "message_ids")))
                emit_message_removed(chat_id, message_id);
        }
    }

    std::optional<PendingRequest> take_pending(const Json &root)
    {
        const Json *extra = field(root, "@extra");
        if (!extra) return std::nullopt;
        const auto request_id = integer_value(*extra);
        if (!request_id) return std::nullopt;
        const auto found = pending_requests_.find(*request_id);
        if (found == pending_requests_.end()) return std::nullopt;
        PendingRequest pending = std::move(found->second);
        pending_requests_.erase(found);
        if (pending.kind == PendingKind::GetChat) pending_chat_fetches_.erase(pending.chat_id);
        if (pending.kind == PendingKind::GetUser) pending_user_fetches_.erase(pending.chat_id);
        return pending;
    }

    void handle_error(const Json &root, const PendingRequest *pending)
    {
        const std::int64_t code  = integer_or(root, "code");
        const std::string detail = safe_tdlib_error_detail(code, string_or(root, "message", "unknown TDLib error"));
        if (pending && pending->kind == PendingKind::LoadChats && code == 404) return;
        std::string message = pending && !pending->operation.empty() ? pending->operation : "TDLib request";
        message += " failed";
        if (code != 0) message += " (code " + std::to_string(code) + ")";
        message += ": " + detail;
        emit_error(std::move(message), pending);
    }

    void handle_response(const Json &root, const std::string &type, const PendingRequest *pending)
    {
        if (type == "ok" && pending && pending->kind == PendingKind::LoadChats && pending->attempts_left > 1) {
            const auto loaded = static_cast<std::int32_t>(std::count_if(
                chats_.cbegin(), chats_.cend(), [](const auto &entry) { return entry.second.order != 0; }));
            if (loaded < pending->target_count) {
                PendingRequest next = *pending;
                --next.attempts_left;
                send_now({build_load_chats_request(pending->target_count - loaded), std::move(next)});
            }
            return;
        }
        if (type == "chats") {
            for (const std::int64_t chat_id : integer_array(field(root, "chat_ids"))) request_chat(chat_id);
            return;
        }
        if (type == "chat") {
            emit_chat(parse_chat_value(root));
            return;
        }
        if (type == "user") {
            Json update = make_object("updateUser");
            put(update, "user", root);
            handle_user_update(update);
            return;
        }
        if (type == "messages") {
            const Json *messages = field(root, "messages");
            if (!messages || !messages->is_array()) return;
            const bool is_history = pending && pending->kind == PendingKind::History;
            for (const Json &message : messages->as_array()) {
                if (message.is_object()) emit_message(parse_message_value(message, &users_, &chats_), is_history);
            }
            return;
        }
        if (type == "message") {
            emit_message(parse_message_value(root, &users_, &chats_));
            return;
        }
        if (type == "file") {
            ClientEvent event;
            event.kind = ClientEventKind::FileUpdated;
            event.file = parse_file_value(root);
            update_cached_avatar_file(event.file);
            emit(std::move(event));
            return;
        }
        if (pending && pending->kind == PendingKind::SetParameters && type != "ok") {
            emit_error("TDLib returned unexpected response while configuring: " + type, pending);
        }
    }

    bool handle_payload(std::string_view payload)
    {
        try {
            const Json root = support::json::parse(payload);
            if (!root.is_object()) throw std::invalid_argument("TDLib response root is not an object");
            const Json *client = field(root, "@client_id");
            if (client) {
                const auto response_client_id = integer_value(*client);
                if (response_client_id && *response_client_id != client_id_) return true;
            }

            const std::string type                = td_type(root);
            std::optional<PendingRequest> pending = take_pending(root);
            if (pending && pending->kind == PendingKind::Account && account_) {
                account_->response(pending->account_wire_id, root);
                return true;
            }
            if (account_) account_->observe(root);
            if (type == "error") {
                handle_error(root, pending ? &*pending : nullptr);
                return true;
            }
            if (type == "updateAuthorizationState") {
                const Json *state = field(root, "authorization_state");
                if (!state) throw std::invalid_argument("authorization update has no authorization_state");
                handle_authorization_state(*state);
                return parse_authorization_state_value(*state).state != AuthState::Closed;
            }
            if (type == "updateConnectionState") {
                const Json *state = field(root, "state");
                if (!state) throw std::invalid_argument("connection update has no state");
                ClientEvent event;
                event.kind             = ClientEventKind::ConnectionStateChanged;
                event.connection_state = parse_connection_state_value(*state);
                emit(std::move(event));
                return true;
            }
            if (type == "updateFile") {
                const Json *file = field(root, "file");
                if (file && file->is_object()) {
                    ClientEvent event;
                    event.kind = ClientEventKind::FileUpdated;
                    event.file = parse_file_value(*file);
                    update_cached_avatar_file(event.file);
                    emit(std::move(event));
                }
                return true;
            }
            if (type == "updateUser") {
                handle_user_update(root);
                return true;
            }
            if (type.rfind("updateChat", 0) == 0 || type == "updateNewChat") {
                handle_chat_update(root, type);
                return true;
            }
            if (type.rfind("updateMessage", 0) == 0 || type == "updateNewMessage" || type == "updateDeleteMessages") {
                handle_message_update(root, type);
                return true;
            }
            handle_response(root, type, pending ? &*pending : nullptr);
        } catch (const std::exception &error) {
            emit_error(std::string("cannot process TDLib response: ") + error.what());
        }
        return true;
    }

    bool close_tdlib_client() noexcept
    {
        if (!api_ || client_id_ <= 0) return true;
        try {
            const std::string request = support::json::stringify(make_object("close"));
            api_->send(client_id_, request.c_str());
            const auto deadline = std::chrono::steady_clock::now() + kCloseTimeout;
            while (std::chrono::steady_clock::now() < deadline) {
                const char *payload = api_->receive(kReceiveTimeoutSeconds);
                if (!payload) continue;
                try {
                    const Json root    = support::json::parse(payload);
                    const Json *client = field(root, "@client_id");
                    if (client) {
                        const auto response_client_id = integer_value(*client);
                        if (response_client_id && *response_client_id != client_id_) continue;
                    }
                    if (td_type(root) != "updateAuthorizationState") continue;
                    const Json *state = field(root, "authorization_state");
                    if (state && state->is_object() && td_type(*state) == "authorizationStateClosed") return true;
                } catch (...) {
                }
            }
        } catch (...) {
        }
        return false;
    }

    void worker_main() noexcept
    {
        bool closed = false;
        try {
            client_id_ = api_->create_client_id();
            if (client_id_ <= 0) {
                emit_error("td_create_client_id returned an invalid client identifier");
            } else {
                account_ = std::make_unique<TdlibAccountProtocol>(
                    [this](Json request, std::uint64_t wire_id) {
                        PendingRequest pending;
                        pending.kind = PendingKind::Account;
                        pending.operation = "account request";
                        pending.account_wire_id = wire_id;
                        return send_now({std::move(request), std::move(pending), std::nullopt});
                    },
                    [this](AccountUpdate update) {
                        ClientEvent event;
                        event.kind = ClientEventKind::Account;
                        event.account = std::move(update);
                        emit(std::move(event));
                    },
                    [this](const Json& value) {
                        Chat chat = parse_chat_value(value);
                        cache_chat(chat);
                        return chat;
                    });
                emit_status("TDLib runtime loaded");
                send_now({build_initialization_request(), {PendingKind::Generic, "initialize TDLib"}});
                while (!stop_requested_.load(std::memory_order_acquire)) {
                    drain_commands();
                    const char *payload = api_->receive(kReceiveTimeoutSeconds);
                    if (payload && !handle_payload(payload)) {
                        closed = true;
                        break;
                    }
                }
            }
        } catch (const std::exception &error) {
            emit_error(std::string("TDLib worker stopped: ") + error.what());
        } catch (...) {
            emit_error("TDLib worker stopped because of an unknown error");
        }

        if (!closed && !close_tdlib_client()) {
            std::fputs("[telegram] TDLib did not close in time; keeping libtdjson loaded for process safety\n", stderr);
            api_->library.release();
        }
        pending_requests_.clear();
        account_.reset();
        pending_chat_fetches_.clear();
        pending_user_fetches_.clear();
        chats_.clear();
        users_.clear();
        messages_.clear();
        client_id_ = 0;
        worker_active_.store(false, std::memory_order_release);
    }

    std::mutex lifecycle_mutex_;
    std::condition_variable lifecycle_condition_;
    std::thread worker_;
    std::thread::id worker_id_;
    bool join_in_progress_ = false;

    std::mutex command_mutex_;
    std::deque<Command> commands_;
    std::atomic<bool> stop_requested_{true};
    std::atomic<bool> worker_active_{false};
    std::atomic<std::uint64_t> next_request_id_{1};

    ClientConfig config_;
    EventCallback callback_;
    std::unique_ptr<TdJsonApi> api_;
    std::unique_ptr<TdlibAccountProtocol> account_;
    int client_id_ = 0;

    std::unordered_map<std::int64_t, PendingRequest> pending_requests_;
    std::unordered_set<std::int64_t> pending_chat_fetches_;
    std::unordered_set<std::int64_t> pending_user_fetches_;
    ChatCache chats_;
    UserCache users_;
    std::map<MessageKey, Message> messages_;
};

}  // namespace

std::unique_ptr<TelegramClient> make_tdlib_client()
{
    return std::make_unique<TdlibClient>();
}

namespace tdlib_test {

AuthMapping map_authorization_state(std::string_view json)
{
    return parse_authorization_state_value(support::json::parse(json));
}

ConnectionState map_connection_state(std::string_view json)
{
    return parse_connection_state_value(support::json::parse(json));
}

FileInfo map_file(std::string_view json)
{
    return parse_file_value(support::json::parse(json));
}

Chat map_chat(std::string_view json)
{
    return parse_chat_value(support::json::parse(json));
}

Message map_message(std::string_view json)
{
    return parse_message_value(support::json::parse(json));
}

Message map_message_with_senders(std::string_view json, const std::vector<std::string>& user_values,
                                 const std::vector<std::string>& chat_values)
{
    UserCache users;
    ChatCache chats;
    for (const auto& encoded : user_values) {
        const auto user = support::json::parse(encoded);
        users.insert_or_assign(integer_or(user, "id"), parse_user_sender(user));
    }
    for (const auto& encoded : chat_values) {
        auto chat = parse_chat_value(support::json::parse(encoded));
        chats.insert_or_assign(chat.id, std::move(chat));
    }
    return parse_message_value(support::json::parse(json), &users, &chats);
}

bool apply_sender_update(Message& message, std::string_view encoded)
{
    const auto value = support::json::parse(encoded);
    const auto type = td_type(value);
    if (type == "updateUser") {
        const auto* user = field(value, "user");
        if (!user) return false;
        const auto sender = parse_user_sender(*user);
        return update_message_sender(message, false, integer_or(*user, "id"), &sender.name, &sender.photo);
    }
    if (type == "updateChatPhoto") {
        const auto photo = parse_chat_photo(field(value, "photo"));
        return update_message_sender(message, true, integer_or(value, "chat_id"), nullptr, &photo);
    }
    if (type == "updateChatTitle") {
        const auto title = string_or(value, "title");
        return update_message_sender(message, true, integer_or(value, "chat_id"), &title, nullptr);
    }
    return false;
}

std::string encode_bytes(std::string_view bytes)
{
    return encode_tdlib_bytes(bytes);
}

std::string sanitize_error_detail(std::int64_t code, std::string detail)
{
    return safe_tdlib_error_detail(code, std::move(detail));
}

std::string load_chats_request(std::int32_t limit)
{
    return support::json::stringify(build_load_chats_request(limit));
}

std::string initialization_request()
{
    return support::json::stringify(build_initialization_request());
}

std::string download_file_request(std::int32_t file_id, std::int32_t priority)
{
    return support::json::stringify(build_download_file_request(file_id, priority));
}

std::string storage_optimizer_request()
{
    return support::json::stringify(build_storage_optimizer_request());
}

std::vector<std::int64_t> decode_integer_array(std::string_view json)
{
    const Json value = support::json::parse(json);
    return integer_array(&value);
}

bool chat_leaves_main_list(std::int64_t previous_order, std::int64_t current_order)
{
    return leaves_main_chat_list(previous_order, current_order);
}

}  // namespace tdlib_test
}  // namespace telegram
