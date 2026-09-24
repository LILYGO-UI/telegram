#include "pages/telegram/telegram_view_model.hpp"

#include <algorithm>
#include <cctype>
#include <exception>
#include <utility>
#include <vector>

namespace telegram {
namespace {

std::string trim(std::string value)
{
    const auto is_space = [](unsigned char character) { return std::isspace(character) != 0; };
    const auto first    = std::find_if_not(value.begin(), value.end(), is_space);
    if (first == value.end()) return {};
    const auto last = std::find_if_not(value.rbegin(), value.rend(), is_space).base();
    return std::string(first, last);
}

std::string bounded_utf8(std::string value, std::size_t max_characters, std::size_t max_bytes)
{
    std::size_t offset     = 0;
    std::size_t characters = 0;
    while (offset < value.size() && characters < max_characters) {
        const auto first         = static_cast<unsigned char>(value[offset]);
        const std::size_t length = first < 0x80                     ? 1
                                   : first >= 0xc2 && first <= 0xdf ? 2
                                   : first >= 0xe0 && first <= 0xef ? 3
                                   : first >= 0xf0 && first <= 0xf4 ? 4
                                                                    : 0;
        if (length == 0 || length > value.size() - offset || length > max_bytes - offset) break;
        bool valid = true;
        for (std::size_t index = 1; index < length; ++index) {
            const auto byte = static_cast<unsigned char>(value[offset + index]);
            valid &= byte >= 0x80 && byte <= 0xbf;
        }
        if (length > 1) {
            const auto second = static_cast<unsigned char>(value[offset + 1]);
            valid &= !(first == 0xe0 && second < 0xa0) && !(first == 0xed && second > 0x9f) &&
                     !(first == 0xf0 && second < 0x90) && !(first == 0xf4 && second > 0x8f);
        }
        if (!valid) break;
        offset += length;
        ++characters;
    }
    return value.substr(0, offset);
}

unsigned char ascii_lower(unsigned char character)
{
    return character >= 'A' && character <= 'Z' ? character + ('a' - 'A') : character;
}

bool contains_search(const std::string& text, const std::string& query)
{
    return std::search(text.begin(), text.end(), query.begin(), query.end(),
                       [](unsigned char left, unsigned char right) {
                           return ascii_lower(left) == ascii_lower(right);
                       }) != text.end();
}

std::int64_t event_chat_id(const ClientEvent& event)
{
    if (event.chat_id != 0) return event.chat_id;
    if (event.chat.id != 0) return event.chat.id;
    return event.message.chat_id;
}

std::int64_t event_message_id(const ClientEvent& event)
{
    return event.message_id != 0 ? event.message_id : event.message.id;
}

}  // namespace

TelegramViewModel::TelegramViewModel(std::unique_ptr<TelegramClient> client, ClientConfig config)
    : client_(std::move(client)), config_(std::move(config))
{
    preferences_ = load_ui_preferences(config_.preferences_path);
}

TelegramViewModel::~TelegramViewModel()
{
    stop();
}

bool TelegramViewModel::start()
{
    if (running_) return true;
    if (!client_) {
        if (set_error_text("Telegram client is unavailable")) ++revision_;
        return false;
    }
    if (event_queue_.closed()) {
        if (set_error_text("Telegram client can't be restarted after it has stopped")) ++revision_;
        return false;
    }

    event_queue_.clear();
    model_.clear();
    clear_account_state();
    clear_local_chat_state();
    discard_model_events();
    failed_downloads_.clear();
    background_downloads_.clear();
    viewed_chat_id_           = 0;
    viewed_latest_message_id_ = 0;
    error_text_.clear();
    status_text_ = "Starting Telegram";
    ++revision_;

    accepting_events_.store(true, std::memory_order_release);
    bool started = false;
    try {
        started = client_->start(config_, [this](ClientEvent event) {
            if (!accepting_events_.load(std::memory_order_acquire)) return;
            (void)event_queue_.push(std::move(event));
        });
    } catch (const std::exception& exception) {
        set_error_text(std::string("Unable to start Telegram: ") + exception.what());
    } catch (...) {
        set_error_text("Unable to start Telegram");
    }

    if (!started) {
        accepting_events_.store(false, std::memory_order_release);
        client_->stop();
        ClientEvent startup_event;
        while (event_queue_.try_pop(startup_event)) apply_event(std::move(startup_event));
        event_queue_.close();
        event_queue_.clear();
        if (error_text_.empty()) set_error_text("Unable to start Telegram");
        model_.set_auth_state(AuthState::Error);
        discard_model_events();
        set_status_text("Telegram is stopped");
        ++revision_;
        return false;
    }

    running_ = true;
    return true;
}

void TelegramViewModel::stop() noexcept
{
    accepting_events_.store(false, std::memory_order_release);
    event_queue_.close();

    if (client_ && running_) {
        try {
            client_->stop();
        } catch (...) {
        }
    }
    running_ = false;
    event_queue_.clear();
    if (clear_account_state()) ++revision_;
}

std::size_t TelegramViewModel::pump(std::size_t max_events)
{
    const std::size_t limit = std::min(max_events, kMaxPumpEvents);
    std::size_t processed   = 0;
    ClientEvent event;
    while (processed < limit && event_queue_.try_pop(event)) {
        if (apply_event(std::move(event))) ++revision_;
        ++processed;
    }
    discard_model_events();
    return processed;
}

const TelegramModel& TelegramViewModel::model() const noexcept
{
    return model_;
}

std::uint64_t TelegramViewModel::revision() const noexcept
{
    return revision_;
}

const std::string& TelegramViewModel::status_text() const noexcept
{
    return status_text_;
}

const std::string& TelegramViewModel::error_text() const noexcept
{
    return error_text_;
}

std::vector<std::int32_t> TelegramViewModel::take_failed_downloads()
{
    std::vector<std::int32_t> failed;
    failed.swap(failed_downloads_);
    return failed;
}

void TelegramViewModel::set_chat_search(std::string query)
{
    query = bounded_utf8(std::move(query), kMaxSearchCharacters, kMaxSearchUtf8Bytes);
    if (chat_search_ == query) return;
    chat_search_ = std::move(query);
    ++revision_;
}

const std::string& TelegramViewModel::chat_search() const noexcept
{
    return chat_search_;
}

bool TelegramViewModel::chat_matches_search(const Chat& chat) const
{
    const std::string query = trim(chat_search_);
    return query.empty() || contains_search(chat.title, query) || contains_search(chat.last_message_preview, query);
}

void TelegramViewModel::set_draft(std::int64_t chat_id, std::string text)
{
    if (!model_.find_chat(chat_id)) return;
    text             = bounded_utf8(std::move(text), kMaxMessageCharacters, kMaxMessageUtf8Bytes);
    const auto found = drafts_.find(chat_id);
    if (text.empty()) {
        if (found == drafts_.end()) return;
        drafts_.erase(found);
    } else if (found != drafts_.end()) {
        if (found->second == text) return;
        found->second = std::move(text);
    } else {
        if (drafts_.size() >= TelegramModel::kMaxChats) return;
        drafts_.emplace(chat_id, std::move(text));
    }
    ++revision_;
}

const std::string& TelegramViewModel::draft(std::int64_t chat_id) const noexcept
{
    static const std::string empty;
    const auto found = drafts_.find(chat_id);
    return found == drafts_.end() ? empty : found->second;
}

bool TelegramViewModel::submit_auth_input(std::string input)
{
    if (!running_) return false;

    const AuthState state = model_.auth_state();
    if (state != AuthState::WaitingForPassword) input = trim(std::move(input));
    if (input.empty()) {
        if (set_error_text("Input can't be empty")) ++revision_;
        return false;
    }

    const char* failure_message = "Unable to submit authentication input";
    bool accepted               = true;
    switch (state) {
        case AuthState::WaitingForPhoneNumber:
            accepted = invoke_client(failure_message, [this, value = std::move(input)]() mutable {
                client_->submit_phone_number(std::move(value));
            });
            break;
        case AuthState::WaitingForEmailAddress:
            accepted = invoke_client(failure_message, [this, value = std::move(input)]() mutable {
                client_->submit_email_address(std::move(value));
            });
            break;
        case AuthState::WaitingForEmailCode:
            accepted = invoke_client(failure_message, [this, value = std::move(input)]() mutable {
                client_->submit_email_code(std::move(value));
            });
            break;
        case AuthState::WaitingForCode:
            accepted = invoke_client(failure_message, [this, value = std::move(input)]() mutable {
                client_->submit_code(std::move(value));
            });
            break;
        case AuthState::WaitingForPassword:
            accepted = invoke_client(failure_message, [this, value = std::move(input)]() mutable {
                client_->submit_password(std::move(value));
            });
            break;
        default:
            if (set_error_text("Authentication input isn't expected now")) ++revision_;
            return false;
    }

    if (accepted) {
        bool changed = set_error_text({});
        changed |= set_status_text("Checking authentication");
        if (changed) ++revision_;
    }
    return accepted;
}

bool TelegramViewModel::select_chat(std::int64_t chat_id)
{
    if (!running_ || !model_.find_chat(chat_id)) return false;

    const std::int64_t previous_chat_id = model_.current_chat_id();
    if (!model_.select_chat(chat_id)) return false;
    viewed_chat_id_           = 0;
    viewed_latest_message_id_ = 0;
    discard_model_events();
    ++revision_;

    bool succeeded = true;
    if (previous_chat_id != 0) {
        succeeded &= invoke_client("Unable to close the previous chat",
                                   [this, previous_chat_id] { client_->close_chat(previous_chat_id); });
    }
    succeeded &= invoke_client("Unable to open chat", [this, chat_id] { client_->open_chat(chat_id); });
    succeeded &= invoke_client("Unable to load chat history",
                               [this, chat_id] { client_->load_history(chat_id, 0, kHistoryPageSize); });
    return succeeded;
}

bool TelegramViewModel::back_to_chats()
{
    const std::int64_t chat_id = model_.current_chat_id();
    if (chat_id == 0) return false;

    bool succeeded = true;
    if (running_) {
        succeeded = invoke_client("Unable to close chat", [this, chat_id] { client_->close_chat(chat_id); });
    }
    if (model_.clear_current_chat()) {
        viewed_chat_id_           = 0;
        viewed_latest_message_id_ = 0;
        discard_model_events();
        ++revision_;
    }
    return succeeded;
}

bool TelegramViewModel::load_older()
{
    const std::int64_t chat_id = model_.current_chat_id();
    if (!running_ || chat_id == 0) return false;
    const std::int64_t from_message_id = model_.messages().empty() ? 0 : model_.messages().front().id;
    return invoke_client("Unable to load older messages", [this, chat_id, from_message_id] {
        client_->load_history(chat_id, from_message_id, kHistoryPageSize);
    });
}

bool TelegramViewModel::mark_current_messages_viewed()
{
    const std::int64_t chat_id = model_.current_chat_id();
    const auto& messages       = model_.messages();
    if (!running_ || chat_id == 0 || messages.empty()) return false;
    const std::int64_t latest_message_id = messages.back().id;
    if (viewed_chat_id_ == chat_id && viewed_latest_message_id_ == latest_message_id) return true;

    const std::vector<std::int64_t> message_ids{latest_message_id};
    const bool accepted = invoke_client("Unable to mark messages as read", [this, chat_id, &message_ids] {
        client_->view_messages(chat_id, message_ids);
    });
    if (accepted) {
        viewed_chat_id_           = chat_id;
        viewed_latest_message_id_ = latest_message_id;
    }
    return accepted;
}

bool TelegramViewModel::mark_message_content_opened(std::int64_t chat_id, std::int64_t message_id)
{
    if (!running_ || chat_id == 0 || message_id == 0) return false;
    return invoke_client("Unable to mark message content as opened",
                         [this, chat_id, message_id] { client_->open_message_content(chat_id, message_id); });
}

bool TelegramViewModel::send_text(std::string text)
{
    const std::int64_t chat_id = model_.current_chat_id();
    if (!running_ || chat_id == 0) return false;
    text = trim(std::move(text));
    if (text.empty()) {
        if (set_error_text("Message can't be empty")) ++revision_;
        return false;
    }
    if (text.size() > kMaxMessageUtf8Bytes) {
        if (set_error_text("Message is too long")) ++revision_;
        return false;
    }
    return invoke_client("Unable to send message", [this, chat_id, value = std::move(text)]() mutable {
        client_->send_text(chat_id, std::move(value));
    });
}

bool TelegramViewModel::request_file(std::int32_t file_id, std::int32_t priority, bool background)
{
    if (!running_ || file_id <= 0) return false;
    priority = std::max<std::int32_t>(1, std::min<std::int32_t>(priority, 32));
    if (background) {
        if (background_downloads_.count(file_id)) return true;
        if (background_downloads_.size() >= 3) return false;
        try {
            client_->download_file(file_id, priority);
            background_downloads_.insert(file_id);
            return true;
        } catch (...) {
            return false;
        }
    }
    background_downloads_.erase(file_id);
    return invoke_client("Unable to download file",
                         [this, file_id, priority] { client_->download_file(file_id, priority); });
}

bool TelegramViewModel::background_download_pending(std::int32_t file_id) const
{
    return background_downloads_.count(file_id) != 0;
}

bool TelegramViewModel::log_out()
{
    if (!running_) return false;
    const bool accepted = invoke_client("Unable to log out", [this] { client_->log_out(); });
    if (accepted) {
        bool changed = clear_local_chat_state();
        changed |= clear_account_state();
        if (changed) ++revision_;
    }
    return accepted;
}

bool TelegramViewModel::apply_event(ClientEvent event)
{
    bool changed = false;
    switch (event.kind) {
        case ClientEventKind::AuthStateChanged:
            changed |= model_.set_auth_state(event.auth_state);
            account_ready_ = event.auth_state == AuthState::Ready;
            if (!account_ready_) changed |= clear_account_state();
            if (event.auth_state != AuthState::Ready && event.auth_state != AuthState::Error) {
                changed |= clear_local_chat_state();
            }
            if (event.auth_state == AuthState::Ready) {
                changed |= set_error_text({});
                if (!invoke_client("Unable to load chats", [this] {
                        client_->load_chats(static_cast<std::int32_t>(TelegramModel::kMaxChats));
                    })) {
                    changed = true;
                }
            } else if (event.auth_state == AuthState::Closed) {
                changed |= model_.clear_chats();
            }
            changed |= refresh_status_text();
            if (!event.text.empty()) {
                if (event.auth_state == AuthState::Error) {
                    changed |= set_error_text(std::move(event.text));
                } else {
                    std::string status = status_text_;
                    status += event.auth_state == AuthState::WaitingForPassword ? ". Hint: " : ": ";
                    status += event.text;
                    changed |= set_status_text(std::move(status));
                }
            }
            break;
        case ClientEventKind::ConnectionStateChanged:
            changed |= model_.set_connection_state(event.connection_state);
            changed |= refresh_status_text();
            break;
        case ClientEventKind::ChatUpserted:
            changed |= model_.upsert_chat(std::move(event.chat));
            changed |= prune_drafts();
            break;
        case ClientEventKind::ChatRemoved:
            changed |= model_.remove_chat(event_chat_id(event));
            changed |= prune_drafts();
            break;
        case ClientEventKind::ChatPhotoChanged: {
            const auto& chats = model_.chats();
            const auto listed = std::find_if(chats.begin(), chats.end(), [&](const Chat& chat) {
                return chat.id == event.chat_id;
            });
            if (listed != chats.end()) {
                Chat chat = *listed;
                chat.photo = event.file;
                changed |= model_.upsert_chat(std::move(chat));
            } else if (const auto* current = model_.current_chat(); current && current->id == event.chat_id) {
                Chat chat = *current;
                chat.photo = event.file;
                changed |= model_.stage_transient_chat(std::move(chat));
            }
            break;
        }
        case ClientEventKind::MessageUpserted:
            changed |= model_.upsert_message(std::move(event.message), event.is_history
                                                                           ? TelegramModel::MessageRetention::Oldest
                                                                           : TelegramModel::MessageRetention::Newest);
            break;
        case ClientEventKind::MessageRemoved:
            if (event_chat_id(event) == 0 || event_chat_id(event) == model_.current_chat_id()) {
                changed |= model_.remove_message(event_message_id(event));
            }
            break;
        case ClientEventKind::FileUpdated:
            if (event.file.is_downloaded) background_downloads_.erase(event.file.id);
            changed |= apply_file_update(event.file);
            break;
        case ClientEventKind::Status:
            changed |= set_status_text(std::move(event.text));
            break;
        case ClientEventKind::Error:
            if (event.file.id > 0 && std::find(failed_downloads_.cbegin(), failed_downloads_.cend(), event.file.id) ==
                                         failed_downloads_.cend()) {
                if (failed_downloads_.size() >= 16) failed_downloads_.erase(failed_downloads_.begin());
                failed_downloads_.push_back(event.file.id);
            }
            if (background_downloads_.erase(event.file.id) == 0)
                changed |= set_error_text(std::move(event.text));
            break;
        case ClientEventKind::Account:
            if (!account_ready_ || !account_model_.accepts(event.account)) break;
            if (event.account.kind == AccountUpdateKind::PrivateChatReady && event.account.success) {
                if (event.account.chat.id == 0) {
                    changed |= account_model_.fail(event.account.request_id, "Telegram returned an invalid chat");
                    break;
                }
                const auto chat_id = event.account.chat.id;
                changed |= model_.stage_transient_chat(std::move(event.account.chat));
                if (model_.current_chat_id() != chat_id) {
                    if (!select_chat(chat_id)) {
                        changed |= account_model_.fail(event.account.request_id, "Unable to open the conversation");
                    }
                    changed = true;
                }
            } else {
                changed |= account_model_.apply(std::move(event.account));
            }
            break;
    }
    return changed;
}

bool TelegramViewModel::apply_file_update(const FileInfo& file)
{
    if (file.id == 0) return false;

    bool changed                  = false;
    const std::vector<Chat> chats = model_.chats();
    for (auto chat : chats) {
        if (chat.photo.id != file.id || chat.photo == file) continue;
        chat.photo = file;
        changed |= model_.upsert_chat(std::move(chat));
    }
    if (const auto* current = model_.current_chat(); current && current->photo.id == file.id &&
        !(current->photo == file)) {
        auto chat = *current;
        chat.photo = file;
        changed |= model_.stage_transient_chat(std::move(chat));
    }

    const std::vector<Message> messages = model_.messages();
    for (auto message : messages) {
        bool message_changed = false;
        if (message.file.id == file.id && message.file != file) {
            message.file = file;
            message_changed = true;
        }
        if (message.sender_photo.id == file.id && message.sender_photo != file) {
            message.sender_photo = file;
            message_changed = true;
        }
        if (message_changed) changed |= model_.upsert_message(std::move(message));
    }
    changed |= account_model_.apply_file_update(file);
    return changed;
}

bool TelegramViewModel::refresh_status_text()
{
    switch (model_.auth_state()) {
        case AuthState::Unknown:
            return set_status_text("Starting Telegram");
        case AuthState::WaitingForParameters:
            return set_status_text("Initializing Telegram");
        case AuthState::WaitingForPhoneNumber:
            return set_status_text("Enter phone number");
        case AuthState::WaitingForEmailAddress:
            return set_status_text("Enter email address");
        case AuthState::WaitingForEmailCode:
            return set_status_text("Enter email code");
        case AuthState::WaitingForCode:
            return set_status_text("Enter verification code");
        case AuthState::WaitingForPassword:
            return set_status_text("Enter two-step verification password");
        case AuthState::WaitingForRegistration:
            return set_status_text("Registration is required");
        case AuthState::WaitingForOtherDeviceConfirmation:
            return set_status_text("Confirm login on another device");
        case AuthState::WaitingForPremiumPurchase:
            return set_status_text("Telegram Premium is required to sign in");
        case AuthState::LoggingOut:
            return set_status_text("Logging out");
        case AuthState::Closing:
            return set_status_text("Closing Telegram");
        case AuthState::Closed:
            return set_status_text("Signed out");
        case AuthState::Error:
            return set_status_text("Telegram error");
        case AuthState::Ready:
            break;
    }

    switch (model_.connection_state()) {
        case ConnectionState::WaitingForNetwork:
            return set_status_text("Waiting for network");
        case ConnectionState::ConnectingToProxy:
            return set_status_text("Connecting to proxy");
        case ConnectionState::Connecting:
            return set_status_text("Connecting");
        case ConnectionState::Updating:
            return set_status_text("Updating chats");
        case ConnectionState::Ready:
            return set_status_text("Connected");
        case ConnectionState::Unknown:
            return set_status_text("Loading chats");
    }
    return false;
}

bool TelegramViewModel::set_status_text(std::string text)
{
    if (status_text_ == text) return false;
    status_text_ = std::move(text);
    return true;
}

bool TelegramViewModel::set_error_text(std::string text)
{
    if (error_text_ == text) return false;
    error_text_ = std::move(text);
    return true;
}

bool TelegramViewModel::invoke_client(const char* failure_message, const std::function<void()>& command)
{
    try {
        command();
        return true;
    } catch (const std::exception& exception) {
        if (set_error_text(std::string(failure_message) + ": " + exception.what())) ++revision_;
    } catch (...) {
        if (set_error_text(failure_message)) ++revision_;
    }
    return false;
}

void TelegramViewModel::discard_model_events()
{
    (void)model_.take_events();
}

bool TelegramViewModel::clear_local_chat_state()
{
    if (chat_search_.empty() && drafts_.empty()) return false;
    chat_search_.clear();
    drafts_.clear();
    return true;
}

bool TelegramViewModel::prune_drafts()
{
    bool changed = false;
    for (auto iterator = drafts_.begin(); iterator != drafts_.end();) {
        if (!model_.find_chat(iterator->first)) {
            iterator = drafts_.erase(iterator);
            changed  = true;
        } else {
            ++iterator;
        }
    }
    return changed;
}

const AccountState& TelegramViewModel::account() const noexcept { return account_model_.state(); }
bool TelegramViewModel::account_busy() const noexcept { return account_model_.busy(); }
const std::string& TelegramViewModel::account_error() const noexcept { return account_model_.error(); }
const std::string& TelegramViewModel::account_notice() const noexcept { return account_model_.notice(); }

void TelegramViewModel::clear_account_feedback()
{
    if (account_model_.clear_feedback()) ++revision_;
}

bool TelegramViewModel::account_action(AccountRequest request)
{
    if (!running_ || !account_ready_) {
        if (account_model_.report_error("Sign in to manage your Telegram account")) ++revision_;
        return false;
    }
    const bool accepted = account_model_.begin(request);
    ++revision_;
    if (!accepted) return false;
    const auto request_id = request.request_id;
    try {
        client_->account_request(std::move(request));
        return true;
    } catch (const std::exception& exception) {
        account_model_.fail(request_id, std::string("Unable to submit account request: ") + exception.what());
    } catch (...) {
        account_model_.fail(request_id, "Unable to submit account request");
    }
    return false;
}

bool TelegramViewModel::clear_account_state()
{
    background_downloads_.clear();
    account_ready_ = false;
    bool changed = account_model_.reset();
    changed |= !contact_search_.empty();
    contact_search_.clear();
    return changed;
}

const UiPreferences& TelegramViewModel::preferences() const noexcept { return preferences_; }

bool TelegramViewModel::set_preferences(UiPreferences preferences)
{
    if (preferences.message_font_size == preferences_.message_font_size &&
        preferences.auto_download_photos == preferences_.auto_download_photos &&
        preferences.contacts_sort_online == preferences_.contacts_sort_online) return true;
    std::string error;
    if (!save_ui_preferences(config_.preferences_path, preferences, error)) {
        if (account_model_.report_error(std::move(error))) ++revision_;
        return false;
    }
    preferences_ = preferences;
    account_model_.clear_feedback();
    ++revision_;
    return true;
}

void TelegramViewModel::set_contact_search(std::string query)
{
    query = bounded_utf8(std::move(query), kMaxSearchCharacters, kMaxSearchUtf8Bytes);
    if (contact_search_ == query) return;
    contact_search_ = std::move(query);
    ++revision_;
}

const std::string& TelegramViewModel::contact_search() const noexcept { return contact_search_; }

std::vector<Contact> TelegramViewModel::filtered_contacts() const
{
    const auto query = trim(contact_search_);
    std::vector<Contact> result;
    for (const auto& contact : account().contacts) {
        if (contact.is_deleted) continue;
        if (query.empty() || contains_search(contact_name(contact), query) ||
            contains_search(contact.username, query) || contains_search(contact.phone_number, query))
            result.push_back(contact);
        if (result.size() >= kMaxContacts) break;
    }
    std::sort(result.begin(), result.end(), [this](const Contact& left, const Contact& right) {
        if (preferences_.contacts_sort_online && left.is_online != right.is_online) return left.is_online;
        const auto left_name = contact_name(left);
        const auto right_name = contact_name(right);
        const auto compare = [](unsigned char l, unsigned char r) { return ascii_lower(l) < ascii_lower(r); };
        if (std::lexicographical_compare(left_name.begin(), left_name.end(), right_name.begin(), right_name.end(), compare))
            return true;
        if (std::lexicographical_compare(right_name.begin(), right_name.end(), left_name.begin(), left_name.end(), compare))
            return false;
        return left.id < right.id;
    });
    return result;
}

}  // namespace telegram
