#include "services/telegram/telegram_client.hpp"

#include "app_identity.h"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <map>
#include <utility>
#include <vector>

namespace telegram {
namespace {

class DemoTelegramClient final : public TelegramClient {
public:
    explicit DemoTelegramClient(std::string image_path) : image_path_(std::move(image_path))
    {
    }

    ~DemoTelegramClient() override
    {
        stop();
    }

    bool start(const ClientConfig&, EventCallback callback) override
    {
        callback_ = std::move(callback);
        running_  = static_cast<bool>(callback_);
        if (!running_) return false;

        ClientEvent authorization;
        authorization.kind       = ClientEventKind::AuthStateChanged;
        authorization.auth_state = AuthState::Ready;
        emit(std::move(authorization));
        ClientEvent connection;
        connection.kind             = ClientEventKind::ConnectionStateChanged;
        connection.connection_state = ConnectionState::Ready;
        emit(std::move(connection));
        seed_chats();
        return true;
    }

    void stop() noexcept override
    {
        running_  = false;
        callback_ = {};
    }

    void submit_phone_number(std::string) override
    {
    }
    void request_qr_login() override
    {
    }
    void restart_phone_login() override
    {
    }
    void submit_email_address(std::string) override
    {
    }
    void submit_email_code(std::string) override
    {
    }
    void submit_code(std::string) override
    {
    }
    void submit_password(std::string) override
    {
    }

    void log_out() override
    {
        ClientEvent event;
        event.kind       = ClientEventKind::AuthStateChanged;
        event.auth_state = AuthState::Closed;
        emit(std::move(event));

        event.auth_state = AuthState::WaitingForPhoneNumber;
        emit(std::move(event));
    }

    void load_chats(std::int32_t) override
    {
        for (const auto& chat : chats_) {
            ClientEvent event;
            event.kind = ClientEventKind::ChatUpserted;
            event.chat = chat;
            emit(std::move(event));
        }
    }

    void open_chat(std::int64_t chat_id) override
    {
        load_history(chat_id, 0, 40);
    }

    void close_chat(std::int64_t) override
    {
    }

    void load_history(std::int64_t chat_id, std::int64_t, std::int32_t limit) override
    {
        const auto iterator = messages_.find(chat_id);
        if (iterator == messages_.end()) return;
        const auto start = iterator->second.size() > static_cast<std::size_t>(limit)
                               ? iterator->second.size() - static_cast<std::size_t>(limit)
                               : 0U;
        for (std::size_t index = start; index < iterator->second.size(); ++index) {
            ClientEvent event;
            event.kind       = ClientEventKind::MessageUpserted;
            event.message    = iterator->second[index];
            event.is_history = true;
            emit(std::move(event));
        }
    }

    void view_messages(std::int64_t, const std::vector<std::int64_t>&) override
    {
    }

    void open_message_content(std::int64_t, std::int64_t) override
    {
    }

    void send_text(std::int64_t chat_id, std::string text) override
    {
        if (text.empty()) return;
        Message message;
        message.id          = next_message_id_++;
        message.chat_id     = chat_id;
        message.date        = now();
        message.kind        = MessageKind::Text;
        message.sender_name = "You";
        message.sender_id   = account_.profile.user.id;
        message.sender_photo = account_.profile.user.photo;
        message.text        = std::move(text);
        message.is_outgoing = true;
        messages_[chat_id].push_back(message);

        auto chat = std::find_if(chats_.begin(), chats_.end(), [chat_id](const Chat& item) { return item.id == chat_id; });
        if (chat == chats_.end()) {
            const auto pending = private_chats_.find(chat_id);
            if (pending != private_chats_.end()) {
                chats_.push_back(pending->second);
                chat = std::prev(chats_.end());
            }
        }
        if (chat != chats_.end()) {
            chat->order = message.date;
            chat->last_message_id = message.id;
            chat->last_message_date = message.date;
            chat->last_message_preview = message.text;
            ClientEvent update;
            update.kind = ClientEventKind::ChatUpserted;
            update.chat = *chat;
            emit(std::move(update));
        }

        ClientEvent event;
        event.kind    = ClientEventKind::MessageUpserted;
        event.message = std::move(message);
        emit(std::move(event));
    }

    void download_file(std::int32_t file_id, std::int32_t) override
    {
        if ((file_id != kDemoPhotoFileId && (file_id < kDemoSelfAvatarId || file_id > kDemoSenderAvatarId)) ||
            image_path_.empty()) return;
        ClientEvent event;
        event.kind                 = ClientEventKind::FileUpdated;
        event.file.id              = file_id;
        event.file.local_path      = image_path_;
        event.file.is_downloaded   = true;
        event.file.size            = 1;
        event.file.downloaded_size = 1;
        emit(std::move(event));
    }

    void account_request(AccountRequest request) override
    {
        const auto publish = [this, &request](AccountUpdate update) {
            update.request_id = request.request_id;
            ClientEvent event;
            event.kind = ClientEventKind::Account;
            event.account = std::move(update);
            emit(std::move(event));
        };
        bool success = true;
        std::string notice;
        AccountUpdate update;
        const auto load_profile = [&] {
            update.kind = AccountUpdateKind::Profile;
            update.profile = account_.profile;
            publish(update);
        };
        const auto load_contacts = [&] {
            update.kind = AccountUpdateKind::Contacts;
            update.contacts = account_.contacts;
            update.total_count = static_cast<std::int32_t>(account_.contacts.size());
            publish(update);
        };
        const auto load_storage = [&] {
            update.kind = AccountUpdateKind::Storage;
            update.storage = account_.storage;
            publish(update);
        };
        switch (request.action) {
            case AccountAction::LoadProfile:
                load_profile();
                break;
            case AccountAction::LoadContacts:
                load_contacts();
                break;
            case AccountAction::LoadContactDetails:
            case AccountAction::LoadChatDetails: {
                auto& details = update.peer_details;
                details.target_id = request.target_id;
                details.target_is_chat = request.action == AccountAction::LoadChatDetails;
                std::int64_t user_id = request.target_id;
                if (details.target_is_chat) {
                    const auto chat = std::find_if(chats_.begin(), chats_.end(),
                        [&request](const Chat& item) { return item.id == request.target_id; });
                    if (chat != chats_.end()) details.chat = *chat;
                    else if (const auto pending = private_chats_.find(request.target_id); pending != private_chats_.end())
                        details.chat = pending->second;
                    else {
                        success = false;
                        notice = "Chat is no longer available";
                        break;
                    }
                    if (!details.chat.is_private) {
                        details.kind = PeerKind::Supergroup;
                        details.member_count = 24;
                        details.username = "lilygo_team";
                        details.description = "LILYGO hardware, firmware and interface development.";
                    } else user_id = request.target_id == 1003 ? account_.profile.user.id
                                          : request.target_id == 1002 ? 502 : request.target_id - 2000;
                }
                if (details.kind == PeerKind::User) {
                    if (user_id == account_.profile.user.id) {
                        details.user = account_.profile.user;
                        details.description = account_.profile.bio;
                    } else {
                        const auto* contact = find_contact(user_id);
                        if (!contact) {
                            success = false;
                            notice = "Contact is no longer available";
                            break;
                        }
                        details.user = *contact;
                        if (user_id == 502) details.description = "Designing interfaces and sharing ideas.";
                    }
                    details.username = details.user.username;
                }
                details.loaded = true;
                update.kind = AccountUpdateKind::PeerDetails;
                publish(update);
                break;
            }
            case AccountAction::OpenSavedMessages:
            case AccountAction::OpenContact: {
                const bool saved = request.action == AccountAction::OpenSavedMessages;
                const auto* contact = find_contact(request.target_id);
                if (!saved && !contact) {
                    success = false;
                    notice = "Contact is no longer available";
                    break;
                }
                const auto chat_id = saved ? 1003 : request.target_id == 502 ? 1002 : request.target_id + 2000;
                const auto found = std::find_if(chats_.begin(), chats_.end(),
                    [chat_id](const Chat& item) { return item.id == chat_id; });
                update.kind = AccountUpdateKind::PrivateChatReady;
                if (found != chats_.end()) update.chat = *found;
                else {
                    update.chat.id = chat_id;
                    update.chat.title = contact_name(*contact);
                    update.chat.photo = contact->photo;
                    update.chat.is_private = true;
                    private_chats_[chat_id] = update.chat;
                }
                publish(update);
                break;
            }
            case AccountAction::AddContact: {
                if (account_.contacts.size() >= kMaxContacts) {
                    success = false;
                    notice = "Contact limit reached";
                    break;
                }
                auto contact = request.contact;
                contact.id = next_contact_id_++;
                contact.is_contact = true;
                contact.status = "last seen recently";
                account_.contacts.push_back(std::move(contact));
                load_contacts();
                notice = "Contact added";
                break;
            }
            case AccountAction::RemoveContact: {
                if (const auto* contact = find_contact(request.target_id)) {
                    update.kind = AccountUpdateKind::ContactChanged;
                    update.contact = *contact;
                    update.contact.is_contact = false;
                    if (removed_contacts_.size() >= kMaxContacts) removed_contacts_.erase(removed_contacts_.begin());
                    removed_contacts_.insert_or_assign(update.contact.id, update.contact);
                    publish(update);
                }
                account_.contacts.erase(std::remove_if(account_.contacts.begin(), account_.contacts.end(),
                    [&request](const Contact& item) { return item.id == request.target_id; }), account_.contacts.end());
                load_contacts();
                notice = "Contact deleted";
                break;
            }
            case AccountAction::SetName:
                account_.profile.user.first_name = request.profile.user.first_name;
                account_.profile.user.last_name = request.profile.user.last_name;
                load_profile();
                notice = "Name updated";
                break;
            case AccountAction::SetUsername:
                if (request.profile.user.username == "taken_username") {
                    success = false;
                    notice = "This username is already taken";
                } else {
                    account_.profile.user.username = request.profile.user.username;
                    load_profile();
                    notice = "Username updated";
                }
                break;
            case AccountAction::SetBio:
                account_.profile.bio = request.profile.bio;
                load_profile();
                notice = "Bio updated";
                break;
            case AccountAction::LoadNotifications:
            case AccountAction::SetNotifications:
                if (request.action == AccountAction::SetNotifications) {
                    account_.notifications.at(static_cast<std::size_t>(request.scope)) = request.notifications;
                    notice = "Notifications updated";
                }
                for (std::size_t index = 0; index < account_.notifications.size(); ++index) {
                    update.kind = AccountUpdateKind::Notifications;
                    update.scope = static_cast<NotificationScope>(index);
                    update.notifications = account_.notifications[index];
                    update.notifications.loaded = true;
                    publish(update);
                }
                break;
            case AccountAction::LoadPrivacy:
            case AccountAction::SetPrivacy:
                if (request.action == AccountAction::SetPrivacy) {
                    account_.privacy.at(static_cast<std::size_t>(request.privacy_key)).visibility = request.visibility;
                    notice = "Privacy updated";
                }
                for (std::size_t index = 0; index < account_.privacy.size(); ++index) {
                    update.kind = AccountUpdateKind::Privacy;
                    update.privacy_key = static_cast<PrivacyKey>(index);
                    update.privacy = account_.privacy[index];
                    publish(update);
                }
                break;
            case AccountAction::LoadSessions:
            case AccountAction::TerminateSession:
                if (request.action == AccountAction::TerminateSession) {
                    const auto found = std::find_if(account_.sessions.begin(), account_.sessions.end(),
                        [&request](const AccountSession& item) { return item.id == request.target_id; });
                    if (found == account_.sessions.end() || found->is_current) {
                        success = false;
                        notice = "This device cannot be terminated here";
                        break;
                    }
                    account_.sessions.erase(found);
                    notice = "Session terminated";
                }
                update.kind = AccountUpdateKind::Sessions;
                update.sessions = account_.sessions;
                publish(update);
                break;
            case AccountAction::LoadBlocked:
            case AccountAction::UnblockSender:
                if (request.action == AccountAction::UnblockSender) {
                    account_.blocked.erase(std::remove_if(account_.blocked.begin(), account_.blocked.end(),
                        [&request](const BlockedSender& item) {
                            return item.id == request.target_id && item.is_chat == request.target_is_chat;
                        }), account_.blocked.end());
                    notice = "Unblocked";
                }
                update.kind = AccountUpdateKind::Blocked;
                update.blocked = account_.blocked;
                update.total_count = static_cast<std::int32_t>(account_.blocked.size());
                publish(update);
                break;
            case AccountAction::LoadStorage:
                load_storage();
                break;
            case AccountAction::ClearMediaCache:
                account_.storage.files_size = 0;
                account_.storage.file_count = 0;
                update.kind = AccountUpdateKind::MediaCacheCleared;
                publish(update);
                load_storage();
                notice = "Media cache cleared";
                break;
        }
        AccountUpdate finished;
        finished.kind = AccountUpdateKind::RequestFinished;
        finished.success = success;
        finished.text = std::move(notice);
        publish(std::move(finished));
    }

private:
    static constexpr std::int32_t kDemoPhotoFileId = 101;
    static constexpr std::int32_t kDemoSelfAvatarId = 301;
    static constexpr std::int32_t kDemoAliceAvatarId = 302;
    static constexpr std::int32_t kDemoTeamAvatarId = 303;
    static constexpr std::int32_t kDemoSenderAvatarId = 304;

    FileInfo demo_avatar(std::int32_t file_id) const
    {
        FileInfo photo;
        photo.id = file_id;
        photo.size = 1;
        photo.local_path = image_path_;
        photo.is_downloaded = !image_path_.empty();
        photo.downloaded_size = photo.is_downloaded ? 1 : 0;
        return photo;
    }

    static std::int64_t now()
    {
        return static_cast<std::int64_t>(std::time(nullptr));
    }

    void emit(ClientEvent event)
    {
        if (running_ && callback_) callback_(std::move(event));
    }

    const Contact* find_contact(std::int64_t user_id) const
    {
        const auto contact = std::find_if(account_.contacts.begin(), account_.contacts.end(),
            [user_id](const Contact& item) { return item.id == user_id; });
        if (contact != account_.contacts.end()) return &*contact;
        const auto removed = removed_contacts_.find(user_id);
        return removed != removed_contacts_.end() ? &removed->second : nullptr;
    }

    void seed_chats()
    {
        const auto current = now();
        account_.profile.user = Contact{9001, "Alex", "Morgan", "alex_morgan", "+15550100123", "online", true,
                                        false, false, demo_avatar(kDemoSelfAvatarId)};
        account_.profile.bio = "Building things that connect people.";
        account_.profile.loaded = true;
        account_.contacts = {
            Contact{502, "Alice", "", "alice_studio", "+15550100101", "online", true, true, false,
                    demo_avatar(kDemoAliceAvatarId)},
            Contact{503, "Bob", "Chen", "bob_chen", "+15550100102", "last seen recently", false, true, false, {}},
            Contact{504, "Chen", "Lin", "chen_lin", "", "last seen within a week", false, true, false, {}},
        };
        for (auto& settings : account_.notifications) settings.loaded = true;
        for (auto& privacy : account_.privacy) privacy = {true, PrivacyVisibility::Contacts, 0};
        account_.privacy[2].visibility = PrivacyVisibility::Everyone;
        account_.sessions = {
            AccountSession{0, true, "LILYGO CM0", LILYGO_UI_TELEGRAM_APP_NAME, "Debian", "Local device", "127.0.0.1", current},
            AccountSession{9007199254741993LL, false, "MacBook", "Telegram macOS", "macOS", "Shanghai", "192.0.2.1", current - 3600},
        };
        account_.blocked = {BlockedSender{505, false, "Morgan"}, BlockedSender{77, true, "Updates Channel"}};
        account_.storage = {true, 24 * 1024 * 1024, 12, 4 * 1024 * 1024};
        chats_             = {
            Chat{1001, 300, 31, current - 60, 2, "LILYGO Team", "The new interface looks ready.", demo_avatar(kDemoTeamAvatarId), false},
            Chat{1002, 200, 22, current - 3600, 0, "Alice", "Voice message", demo_avatar(kDemoAliceAvatarId), false, true},
            Chat{1003, 100, 12, current - 86400, 0, "Saved Messages", "Device notes", {}, true, true},
        };

        Message welcome;
        welcome.id          = 29;
        welcome.chat_id     = 1001;
        welcome.sender_id   = 501;
        welcome.date        = current - 420;
        welcome.kind        = MessageKind::Text;
        welcome.sender_name = "LILYGO Team";
        welcome.sender_photo = demo_avatar(kDemoSenderAvatarId);
        welcome.text        = "Good morning! Are we ready for the next test?";

        Message photo;
        photo.id                   = 30;
        photo.chat_id              = 1001;
        photo.sender_id            = 1004;
        photo.sender_is_chat       = true;
        photo.date                 = current - 180;
        photo.kind                 = MessageKind::Photo;
        photo.sender_name          = "Design Notes";
        photo.text                 = "A photo from the studio.";
        photo.width                = 128;
        photo.height               = 128;
        photo.file.id              = kDemoPhotoFileId;
        photo.file.size            = 1;
        photo.file.local_path      = image_path_;
        photo.file.is_downloaded   = !image_path_.empty();
        photo.file.downloaded_size = photo.file.is_downloaded ? 1 : 0;

        Message reply;
        reply.id          = 31;
        reply.chat_id     = 1001;
        reply.date        = current - 60;
        reply.kind        = MessageKind::Text;
        reply.sender_name = "You";
        reply.sender_id   = account_.profile.user.id;
        reply.sender_photo = account_.profile.user.photo;
        reply.text        = "The new interface looks ready.";
        reply.is_outgoing = true;
        messages_[1001]   = {welcome, photo, reply};

        Message voice;
        voice.id               = 22;
        voice.chat_id          = 1002;
        voice.sender_id        = 502;
        voice.date             = current - 3600;
        voice.kind             = MessageKind::Voice;
        voice.sender_name      = "Alice";
        voice.sender_photo     = demo_avatar(kDemoAliceAvatarId);
        voice.duration_seconds = 18;
        voice.file.id          = 202;
        voice.file.size        = 18 * 12000;
        messages_[1002]        = {voice};

        Message saved;
        saved.id          = 12;
        saved.chat_id     = 1003;
        saved.date        = current - 86400;
        saved.kind        = MessageKind::Text;
        saved.sender_name = "You";
        saved.sender_id   = account_.profile.user.id;
        saved.sender_photo = account_.profile.user.photo;
        saved.text        = "Friday: bring the prototype and charging cable.";
        saved.is_outgoing = true;
        messages_[1003]   = {saved};

        load_chats(100);
    }

    std::string image_path_;
    EventCallback callback_;
    bool running_                 = false;
    std::int64_t next_message_id_ = 10000;
    std::vector<Chat> chats_;
    std::map<std::int64_t, std::vector<Message>> messages_;
    std::map<std::int64_t, Chat> private_chats_;
    std::map<std::int64_t, Contact> removed_contacts_;
    AccountState account_;
    std::int64_t next_contact_id_ = 600;
};

}  // namespace

std::unique_ptr<TelegramClient> make_demo_client(std::string demo_image_path)
{
    return std::make_unique<DemoTelegramClient>(std::move(demo_image_path));
}

}  // namespace telegram
