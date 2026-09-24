#include "domain/telegram_account_model.hpp"

#include "support/json.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <unordered_set>
#include <unistd.h>

namespace telegram {
namespace {

bool text_valid(const std::string& text, std::size_t characters)
{
    if (text.size() > characters * 4) return false;
    std::size_t count = 0;
    for (std::size_t offset = 0; offset < text.size(); ++count) {
        const auto byte = static_cast<unsigned char>(text[offset]);
        const std::size_t length = byte < 0x80 ? 1 : byte >= 0xc2 && byte <= 0xdf ? 2
                                                : byte >= 0xe0 && byte <= 0xef ? 3
                                                : byte >= 0xf0 && byte <= 0xf4 ? 4 : 0;
        if (length == 0 || length > text.size() - offset || byte < 0x20 || byte == 0x7f) return false;
        for (std::size_t index = 1; index < length; ++index) {
            const auto next = static_cast<unsigned char>(text[offset + index]);
            if (next < 0x80 || next > 0xbf) return false;
        }
        if (length > 1) {
            const auto second = static_cast<unsigned char>(text[offset + 1]);
            if ((byte == 0xe0 && second < 0xa0) || (byte == 0xed && second > 0x9f) ||
                (byte == 0xf0 && second < 0x90) || (byte == 0xf4 && second > 0x8f)) return false;
        }
        offset += length;
    }
    return count <= characters;
}

bool nonblank(const std::string& text)
{
    return std::any_of(text.begin(), text.end(), [](unsigned char byte) { return byte > 0x20; });
}

std::string validate_request(const AccountRequest& request, const AccountState& state)
{
    const auto bounded_contact = [](const Contact& contact) {
        return contact.first_name.size() <= 256 && contact.last_name.size() <= 256 &&
               contact.username.size() <= 128 && contact.phone_number.size() <= 32 && contact.status.size() <= 256;
    };
    if (!bounded_contact(request.contact) || !bounded_contact(request.profile.user) || request.profile.bio.size() > 280)
        return "Account input is too long";
    const auto valid_name = [](const Contact& contact) {
        return nonblank(contact.first_name) && text_valid(contact.first_name, 64) &&
               text_valid(contact.last_name, 64);
    };
    switch (request.action) {
        case AccountAction::SetName:
            if (!valid_name(request.profile.user)) return "Enter a first name (up to 64 characters)";
            break;
        case AccountAction::SetUsername: {
            const auto& value = request.profile.user.username;
            if (value.empty()) break;
            if (value.size() < 5 || value.size() > 32 ||
                !((value.front() >= 'a' && value.front() <= 'z') || (value.front() >= 'A' && value.front() <= 'Z')) ||
                !std::all_of(value.begin(), value.end(), [](unsigned char byte) {
                    return (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') ||
                           (byte >= '0' && byte <= '9') || byte == '_';
                })) return "Use 5-32 letters, numbers or underscores, starting with a letter";
            break;
        }
        case AccountAction::SetBio:
            if (!text_valid(request.profile.bio, 70)) return "Bio must be at most 70 characters";
            break;
        case AccountAction::AddContact: {
            if (!valid_name(request.contact)) return "Enter a first name (up to 64 characters)";
            const auto& phone = request.contact.phone_number;
            const std::size_t start = !phone.empty() && phone.front() == '+' ? 1 : 0;
            if (phone.size() - start < 5 || phone.size() - start > 15 ||
                !std::all_of(phone.begin() + start, phone.end(), [](unsigned char byte) {
                    return byte >= '0' && byte <= '9';
                })) return "Enter a phone number with country code";
            break;
        }
        case AccountAction::OpenContact:
        case AccountAction::LoadContactDetails:
        case AccountAction::RemoveContact:
            if (request.target_id <= 0) return "Select a contact";
            break;
        case AccountAction::LoadChatDetails:
            if (request.target_id == 0) return "Select a chat";
            break;
        case AccountAction::UnblockSender:
            if (request.target_id == 0 || (!request.target_is_chat && request.target_id < 0))
                return "Select a blocked user or chat";
            break;
        case AccountAction::SetNotifications:
            if (request.scope < NotificationScope::PrivateChats || request.scope > NotificationScope::Channels)
                return "Invalid notification category";
            break;
        case AccountAction::SetPrivacy:
            if (request.privacy_key < PrivacyKey::PhoneNumber || request.privacy_key > PrivacyKey::ProfilePhoto ||
                request.visibility < PrivacyVisibility::Everyone || request.visibility > PrivacyVisibility::Nobody)
                return "Select a supported privacy option";
            break;
        case AccountAction::TerminateSession: {
            const auto session = std::find_if(state.sessions.begin(), state.sessions.end(), [&](const auto& value) {
                return value.id == request.target_id;
            });
            if (request.target_id == 0 || session == state.sessions.end()) return "Select an active session";
            if (session->is_current) return "Use Log Out to end the current session";
            break;
        }
        case AccountAction::LoadProfile:
        case AccountAction::LoadContacts:
        case AccountAction::OpenSavedMessages:
        case AccountAction::LoadNotifications:
        case AccountAction::LoadPrivacy:
        case AccountAction::LoadSessions:
        case AccountAction::LoadBlocked:
        case AccountAction::LoadStorage:
        case AccountAction::ClearMediaCache:
            break;
        default:
            return "Unsupported account action";
    }
    return {};
}

const char* success_notice(AccountAction action)
{
    switch (action) {
        case AccountAction::AddContact: return "Contact added";
        case AccountAction::RemoveContact: return "Contact removed";
        case AccountAction::SetName:
        case AccountAction::SetUsername:
        case AccountAction::SetBio: return "Profile updated";
        case AccountAction::SetNotifications: return "Notification settings saved";
        case AccountAction::SetPrivacy: return "Privacy settings saved";
        case AccountAction::TerminateSession: return "Session terminated";
        case AccountAction::UnblockSender: return "User unblocked";
        case AccountAction::ClearMediaCache: return "Media cache cleared";
        default: return "";
    }
}

template <typename T, typename IsValid>
void bound_unique(std::vector<T>& items, std::size_t max_size, IsValid valid)
{
    std::unordered_set<std::int64_t> ids;
    items.erase(std::remove_if(items.begin(), items.end(), [&](const T& value) {
        return !valid(value) || ids.size() >= max_size || !ids.insert(value.id).second;
    }), items.end());
}

template <typename T>
void bound_unique(std::vector<T>& items, std::size_t max_size)
{
    bound_unique(items, max_size, [](const T& value) { return value.id != 0; });
}

}  // namespace

bool TelegramAccountModel::accepts(const AccountUpdate& update) const noexcept
{
    if (update.kind == AccountUpdateKind::PeerDetails &&
        (update.peer_details.target_id == 0 || update.peer_details.target_id != state_.peer_details.target_id ||
         update.peer_details.target_is_chat != state_.peer_details.target_is_chat)) return false;
    return update.request_id == 0 ? update.kind != AccountUpdateKind::RequestFinished &&
                                       update.kind != AccountUpdateKind::PrivateChatReady
                                 : pending_.find(update.request_id) != pending_.end();
}

bool TelegramAccountModel::begin(AccountRequest& request)
{
    if (const auto validation = validate_request(request, state_); !validation.empty()) {
        report_error(validation);
        return false;
    }
    const bool peer_request = request.action == AccountAction::LoadContactDetails || request.action == AccountAction::LoadChatDetails;
    if (peer_request) {
        for (auto entry = pending_.begin(); entry != pending_.end();) {
            if (entry->second == AccountAction::LoadContactDetails || entry->second == AccountAction::LoadChatDetails)
                entry = pending_.erase(entry);
            else ++entry;
        }
    }
    if (pending_.size() >= kMaxPendingRequests) {
        report_error("Wait for the current account requests to finish");
        return false;
    }
    if (next_request_id_ == std::numeric_limits<std::uint64_t>::max()) {
        report_error("Restart Telegram before sending another account request");
        return false;
    }
    request.request_id = next_request_id_++;
    pending_.emplace(request.request_id, request.action);
    if (peer_request) {
        state_.peer_details = {};
        state_.peer_details.target_id = request.target_id;
        state_.peer_details.target_is_chat = request.action == AccountAction::LoadChatDetails;
        populated_ = true;
    }
    clear_feedback();
    return true;
}

bool TelegramAccountModel::fail(std::uint64_t request_id, std::string error)
{
    if (pending_.erase(request_id) == 0) return false;
    report_error(error.empty() ? "Account request failed" : std::move(error));
    return true;
}

bool TelegramAccountModel::report_error(std::string error)
{
    if (error.size() > 1024) error.resize(1024);
    const bool changed = error_ != error || !notice_.empty();
    error_ = std::move(error);
    notice_.clear();
    return changed;
}

bool TelegramAccountModel::clear_feedback()
{
    if (error_.empty() && notice_.empty()) return false;
    error_.clear();
    notice_.clear();
    return true;
}

bool TelegramAccountModel::reset()
{
    const bool changed = busy() || !error_.empty() || !notice_.empty() || populated_;
    state_ = {};
    populated_ = false;
    pending_.clear();
    clear_feedback();
    return changed;
}

bool TelegramAccountModel::apply(AccountUpdate update)
{
    if (!accepts(update)) return false;
    if (!update.success) return update.request_id != 0 ? fail(update.request_id, std::move(update.text))
                                                     : report_error(std::move(update.text));
    const auto update_peer_contact = [&](const Contact& contact) {
        auto& peer = state_.peer_details;
        if (peer.kind != PeerKind::User || peer.user.id <= 0 || peer.user.id != contact.id) return false;
        if (peer.user == contact) return false;
        peer.user = contact;
        return true;
    };
    switch (update.kind) {
        case AccountUpdateKind::RequestFinished: {
            const auto found = pending_.find(update.request_id);
            if (found == pending_.end()) return false;
            if (error_.empty()) notice_ = success_notice(found->second);
            pending_.erase(found);
            return true;
        }
        case AccountUpdateKind::Profile:
            if (state_.profile == update.profile) return false;
            state_.profile = std::move(update.profile);
            break;
        case AccountUpdateKind::PeerDetails:
            if (state_.peer_details == update.peer_details) return false;
            state_.peer_details = std::move(update.peer_details);
            break;
        case AccountUpdateKind::Contacts: {
            bound_unique(update.contacts, kMaxContacts);
            const auto peer = std::find_if(update.contacts.begin(), update.contacts.end(), [&](const auto& contact) {
                return contact.id == state_.peer_details.user.id;
            });
            if (peer != update.contacts.end()) update_peer_contact(*peer);
            else if (update.total_count <= static_cast<std::int32_t>(update.contacts.size()))
                state_.peer_details.user.is_contact = false;
            state_.contacts = std::move(update.contacts);
            state_.contacts_loaded = true;
            state_.contacts_total = std::max<std::int32_t>(static_cast<std::int32_t>(state_.contacts.size()), update.total_count);
            break;
        }
        case AccountUpdateKind::ContactChanged: {
            const auto found = std::find_if(state_.contacts.begin(), state_.contacts.end(), [&](const auto& value) {
                return value.id == update.contact.id;
            });
            if (update.contact.id <= 0) return false;
            const bool peer_changed = update_peer_contact(update.contact);
            if (!update.contact.is_contact || update.contact.is_deleted) {
                if (found == state_.contacts.end()) return peer_changed;
                state_.contacts.erase(found);
                state_.contacts_total = std::max<std::int32_t>(0, state_.contacts_total - 1);
            } else if (found != state_.contacts.end()) {
                if (*found == update.contact) return peer_changed;
                *found = std::move(update.contact);
            } else {
                if (state_.contacts.size() >= kMaxContacts) return peer_changed;
                state_.contacts.push_back(std::move(update.contact));
                ++state_.contacts_total;
            }
            break;
        }
        case AccountUpdateKind::Notifications:
            if (update.scope < NotificationScope::PrivateChats || update.scope > NotificationScope::Channels) return false;
            state_.notifications[static_cast<std::size_t>(update.scope)] = update.notifications;
            break;
        case AccountUpdateKind::Privacy:
            if (update.privacy_key < PrivacyKey::PhoneNumber || update.privacy_key > PrivacyKey::ProfilePhoto) return false;
            state_.privacy[static_cast<std::size_t>(update.privacy_key)] = update.privacy;
            break;
        case AccountUpdateKind::Sessions:
            bound_unique(update.sessions, kMaxAccountSessions, [](const AccountSession& session) {
                return session.id != 0 || session.is_current;
            });
            state_.sessions = std::move(update.sessions);
            state_.sessions_loaded = true;
            break;
        case AccountUpdateKind::Blocked:
            bound_unique(update.blocked, kMaxBlockedSenders);
            state_.blocked = std::move(update.blocked);
            state_.blocked_loaded = true;
            state_.blocked_total = std::max<std::int32_t>(static_cast<std::int32_t>(state_.blocked.size()), update.total_count);
            break;
        case AccountUpdateKind::Storage:
            state_.storage = update.storage;
            break;
        case AccountUpdateKind::MediaCacheCleared:
            ++state_.media_cache_generation;
            break;
        case AccountUpdateKind::PrivateChatReady:
            return false;
    }
    populated_ = true;
    return true;
}

bool TelegramAccountModel::apply_file_update(const FileInfo& file)
{
    if (file.id <= 0) return false;
    bool changed = false;
    const auto update_photo = [&](FileInfo& photo) {
        if (photo.id != file.id || photo == file) return;
        photo = file;
        changed = true;
    };
    update_photo(state_.profile.user.photo);
    update_photo(state_.peer_details.user.photo);
    update_photo(state_.peer_details.chat.photo);
    for (auto& contact : state_.contacts) update_photo(contact.photo);
    return changed;
}

bool valid_preferences(const UiPreferences& preferences) noexcept
{
    return preferences.message_font_size == 18 || preferences.message_font_size == 22 ||
           preferences.message_font_size == 26 || preferences.message_font_size == 30;
}

UiPreferences load_ui_preferences(const std::string& path)
{
    UiPreferences result;
    if (path.empty()) return result;
    try {
        std::ifstream file(path, std::ios::binary);
        std::string text(4097, '\0');
        file.read(text.data(), static_cast<std::streamsize>(text.size()));
        text.resize(static_cast<std::size_t>(file.gcount()));
        if (text.empty() || text.size() > 4096) return result;
        const auto json = support::json::parse(text);
        if (!json.is_object()) return result;
        if (const auto* font = json.find("message_font_size")) {
            if (!font->is_number()) return {};
            const auto value = font->as_int64();
            if (value != 18 && value != 22 && value != 26 && value != 30) return {};
            result.message_font_size = static_cast<std::int32_t>(value);
        }
        if (const auto* photos = json.find("auto_download_photos")) result.auto_download_photos = photos->as_bool();
        if (const auto* online = json.find("contacts_sort_online")) result.contacts_sort_online = online->as_bool();
        return result;
    } catch (...) {
        return {};
    }
}

bool save_ui_preferences(const std::string& path, const UiPreferences& preferences, std::string& error)
{
    if (!valid_preferences(preferences)) {
        error = "Unsupported message font size";
        return false;
    }
    if (path.empty()) return true;
    std::string temporary;
    int descriptor = -1;
    try {
        const auto parent = std::filesystem::path(path).parent_path();
        if (!parent.empty()) std::filesystem::create_directories(parent);
        const auto text = support::json::stringify(support::json::Value::Object{
            {"message_font_size", preferences.message_font_size},
            {"auto_download_photos", preferences.auto_download_photos},
            {"contacts_sort_online", preferences.contacts_sort_online}}) + "\n";
        temporary = path + ".tmp.XXXXXX";
        descriptor = ::mkstemp(temporary.data());
        if (descriptor < 0) throw std::runtime_error(std::strerror(errno));
        std::size_t written = 0;
        while (written < text.size()) {
            const auto count = ::write(descriptor, text.data() + written, text.size() - written);
            if (count < 0 && errno == EINTR) continue;
            if (count <= 0) throw std::runtime_error(std::strerror(errno));
            written += static_cast<std::size_t>(count);
        }
        if (::fsync(descriptor) != 0) throw std::runtime_error(std::strerror(errno));
        const auto close_result = ::close(descriptor);
        descriptor = -1;
        if (close_result != 0 || std::rename(temporary.c_str(), path.c_str()) != 0)
            throw std::runtime_error(std::strerror(errno));
        return true;
    } catch (const std::exception& exception) {
        if (descriptor >= 0) ::close(descriptor);
        if (!temporary.empty()) std::remove(temporary.c_str());
        error = std::string("Unable to save preferences: ") + exception.what();
        return false;
    }
}

}  // namespace telegram
