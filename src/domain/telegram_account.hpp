#ifndef LILYGO_UI_TELEGRAM_ACCOUNT_HPP
#define LILYGO_UI_TELEGRAM_ACCOUNT_HPP

#include "domain/telegram_types.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace telegram {

inline constexpr std::size_t kMaxContacts = 500;
inline constexpr std::size_t kMaxAccountSessions = 100;
inline constexpr std::size_t kMaxBlockedSenders = 500;

struct Contact {
    std::int64_t id = 0;
    std::string first_name;
    std::string last_name;
    std::string username;
    std::string phone_number;
    std::string status;
    bool is_online = false;
    bool is_contact = false;
    bool is_deleted = false;
    FileInfo photo;
};

inline bool operator==(const Contact& left, const Contact& right)
{
    return left.id == right.id && left.first_name == right.first_name && left.last_name == right.last_name &&
           left.username == right.username && left.phone_number == right.phone_number && left.status == right.status &&
           left.is_online == right.is_online && left.is_contact == right.is_contact && left.is_deleted == right.is_deleted &&
           left.photo == right.photo;
}

inline bool operator!=(const Contact& left, const Contact& right)
{
    return !(left == right);
}

struct UserProfile {
    Contact user;
    std::string bio;
    bool loaded = false;
};

inline bool operator==(const UserProfile& left, const UserProfile& right)
{
    return left.user == right.user && left.bio == right.bio && left.loaded == right.loaded;
}

inline bool operator!=(const UserProfile& left, const UserProfile& right)
{
    return !(left == right);
}

enum class PeerKind { User, BasicGroup, Supergroup, Channel };

struct PeerDetails {
    std::int64_t target_id = 0;
    bool target_is_chat = false;
    PeerKind kind = PeerKind::User;
    Contact user;
    Chat chat;
    std::string description;
    std::string username;
    // Zero means TDLib did not provide a member count.
    std::int32_t member_count = 0;
    bool loaded = false;
};

inline bool operator==(const PeerDetails& left, const PeerDetails& right)
{
    return left.target_id == right.target_id && left.target_is_chat == right.target_is_chat &&
           left.kind == right.kind && left.user == right.user && left.chat == right.chat &&
           left.description == right.description && left.username == right.username &&
           left.member_count == right.member_count && left.loaded == right.loaded;
}

inline bool operator!=(const PeerDetails& left, const PeerDetails& right)
{
    return !(left == right);
}

enum class NotificationScope { PrivateChats, GroupChats, Channels };

struct NotificationSettings {
    bool loaded = false;
    bool enabled = true;
    bool show_preview = true;
    bool sound_enabled = true;
};

enum class PrivacyKey { PhoneNumber, LastSeen, ProfilePhoto };
enum class PrivacyVisibility { Everyone, Contacts, Nobody, Custom };

struct PrivacySetting {
    bool loaded = false;
    PrivacyVisibility visibility = PrivacyVisibility::Custom;
    std::int32_t exception_count = 0;
};

struct AccountSession {
    std::int64_t id = 0;
    bool is_current = false;
    std::string device_model;
    std::string application_name;
    std::string platform;
    std::string location;
    std::string ip_address;
    std::int64_t last_active_date = 0;
};

struct BlockedSender {
    std::int64_t id = 0;
    bool is_chat = false;
    std::string name;
};

struct StorageUsage {
    bool loaded = false;
    std::int64_t files_size = 0;
    std::int64_t file_count = 0;
    std::int64_t database_size = 0;
};

struct UiPreferences {
    std::int32_t message_font_size = 22;
    bool auto_download_photos = true;
    bool contacts_sort_online = false;
};

enum class AccountAction {
    LoadProfile, LoadContacts, OpenContact, OpenSavedMessages, AddContact, RemoveContact,
    LoadContactDetails, LoadChatDetails,
    SetName, SetUsername, SetBio,
    LoadNotifications, SetNotifications,
    LoadPrivacy, SetPrivacy,
    LoadSessions, TerminateSession,
    LoadBlocked, UnblockSender,
    LoadStorage, ClearMediaCache,
};

struct AccountRequest {
    AccountAction action = AccountAction::LoadProfile;
    std::uint64_t request_id = 0;
    std::int64_t target_id = 0;
    bool target_is_chat = false;
    Contact contact;
    UserProfile profile;
    NotificationScope scope = NotificationScope::PrivateChats;
    NotificationSettings notifications;
    PrivacyKey privacy_key = PrivacyKey::PhoneNumber;
    PrivacyVisibility visibility = PrivacyVisibility::Contacts;
};

enum class AccountUpdateKind {
    Profile, Contacts, ContactChanged, Notifications, Privacy, Sessions, Blocked,
    Storage, PrivateChatReady, RequestFinished, MediaCacheCleared, PeerDetails,
};

struct AccountUpdate {
    AccountUpdateKind kind = AccountUpdateKind::RequestFinished;
    std::uint64_t request_id = 0;
    bool success = true;
    std::string text;
    UserProfile profile;
    PeerDetails peer_details;
    std::vector<Contact> contacts;
    Contact contact;
    std::int32_t total_count = 0;
    NotificationScope scope = NotificationScope::PrivateChats;
    NotificationSettings notifications;
    PrivacyKey privacy_key = PrivacyKey::PhoneNumber;
    PrivacySetting privacy;
    std::vector<AccountSession> sessions;
    std::vector<BlockedSender> blocked;
    StorageUsage storage;
    Chat chat;
};

struct AccountState {
    UserProfile profile;
    PeerDetails peer_details;
    std::vector<Contact> contacts;
    bool contacts_loaded = false;
    std::int32_t contacts_total = 0;
    std::array<NotificationSettings, 3> notifications{};
    std::array<PrivacySetting, 3> privacy{};
    std::vector<AccountSession> sessions;
    bool sessions_loaded = false;
    std::vector<BlockedSender> blocked;
    bool blocked_loaded = false;
    std::int32_t blocked_total = 0;
    StorageUsage storage;
    std::uint64_t media_cache_generation = 0;
};

[[nodiscard]] inline std::string contact_name(const Contact& contact)
{
    std::string name = contact.first_name;
    if (!contact.last_name.empty()) {
        if (!name.empty()) name += ' ';
        name += contact.last_name;
    }
    if (name.empty()) name = contact.username.empty() ? contact.phone_number : contact.username;
    return name.empty() ? "Deleted Account" : name;
}

}  // namespace telegram

#endif
