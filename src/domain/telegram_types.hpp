#ifndef LILYGO_UI_TELEGRAM_DOMAIN_TELEGRAM_TYPES_HPP
#define LILYGO_UI_TELEGRAM_DOMAIN_TELEGRAM_TYPES_HPP

#include <cstdint>
#include <string>

namespace telegram {

enum class AuthState {
    Unknown,
    WaitingForParameters,
    WaitingForPhoneNumber,
    WaitingForEmailAddress,
    WaitingForEmailCode,
    WaitingForCode,
    WaitingForPassword,
    WaitingForRegistration,
    WaitingForOtherDeviceConfirmation,
    WaitingForPremiumPurchase,
    Ready,
    LoggingOut,
    Closing,
    Closed,
    Error,
};

enum class ConnectionState {
    Unknown,
    WaitingForNetwork,
    ConnectingToProxy,
    Connecting,
    Updating,
    Ready,
};

enum class MessageKind {
    Text,
    Photo,
    Voice,
    Unsupported,
};

struct FileInfo {
    std::int32_t id              = 0;
    std::int64_t size            = 0;
    std::int64_t downloaded_size = 0;
    std::string local_path;
    bool is_downloading = false;
    bool is_downloaded  = false;
};

inline bool operator==(const FileInfo& left, const FileInfo& right)
{
    return left.id == right.id && left.size == right.size && left.downloaded_size == right.downloaded_size &&
           left.local_path == right.local_path && left.is_downloading == right.is_downloading &&
           left.is_downloaded == right.is_downloaded;
}

inline bool operator!=(const FileInfo& left, const FileInfo& right)
{
    return !(left == right);
}

struct Chat {
    std::int64_t id                = 0;
    std::int64_t order             = 0;
    std::int64_t last_message_id   = 0;
    std::int64_t last_message_date = 0;
    std::int32_t unread_count      = 0;
    std::string title;
    std::string last_message_preview;
    FileInfo photo;
    bool is_muted = false;
    bool is_private = false;
};

inline bool operator==(const Chat& left, const Chat& right)
{
    return left.id == right.id && left.order == right.order && left.last_message_id == right.last_message_id &&
           left.last_message_date == right.last_message_date && left.unread_count == right.unread_count &&
           left.title == right.title && left.last_message_preview == right.last_message_preview &&
           left.photo == right.photo && left.is_muted == right.is_muted && left.is_private == right.is_private;
}

inline bool operator!=(const Chat& left, const Chat& right)
{
    return !(left == right);
}

struct Message {
    std::int64_t id        = 0;
    std::int64_t chat_id   = 0;
    std::int64_t sender_id = 0;
    std::int64_t date      = 0;
    MessageKind kind       = MessageKind::Unsupported;
    std::string sender_name;
    std::string text;
    FileInfo file;
    std::int32_t width            = 0;
    std::int32_t height           = 0;
    std::int32_t duration_seconds = 0;
    bool is_outgoing              = false;
    bool is_sending               = false;
    bool is_failed                = false;
    bool content_hidden           = false;
    FileInfo sender_photo;
    bool sender_is_chat = false;
};

inline bool operator==(const Message& left, const Message& right)
{
    return left.id == right.id && left.chat_id == right.chat_id && left.sender_id == right.sender_id &&
           left.date == right.date && left.kind == right.kind && left.sender_name == right.sender_name &&
           left.text == right.text && left.file == right.file && left.width == right.width &&
           left.height == right.height && left.duration_seconds == right.duration_seconds &&
           left.is_outgoing == right.is_outgoing && left.is_sending == right.is_sending &&
           left.is_failed == right.is_failed && left.content_hidden == right.content_hidden &&
           left.sender_photo == right.sender_photo && left.sender_is_chat == right.sender_is_chat;
}

inline bool operator!=(const Message& left, const Message& right)
{
    return !(left == right);
}

enum class ModelEventKind {
    AuthStateChanged,
    ConnectionStateChanged,
    ChatsChanged,
    CurrentChatChanged,
    MessagesChanged,
    Cleared,
};

struct ModelEvent {
    ModelEventKind kind     = ModelEventKind::Cleared;
    std::int64_t chat_id    = 0;
    std::int64_t message_id = 0;
};

inline bool operator==(const ModelEvent& left, const ModelEvent& right)
{
    return left.kind == right.kind && left.chat_id == right.chat_id && left.message_id == right.message_id;
}

inline bool operator!=(const ModelEvent& left, const ModelEvent& right)
{
    return !(left == right);
}

}  // namespace telegram

#endif
