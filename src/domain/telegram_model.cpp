#include "domain/telegram_model.hpp"

#include <algorithm>
#include <utility>

namespace telegram {

AuthState TelegramModel::auth_state() const noexcept
{
    return auth_state_;
}

ConnectionState TelegramModel::connection_state() const noexcept
{
    return connection_state_;
}

const std::vector<Chat>& TelegramModel::chats() const noexcept
{
    return chats_;
}

std::int64_t TelegramModel::current_chat_id() const noexcept
{
    return current_chat_id_;
}

const Chat* TelegramModel::current_chat() const noexcept
{
    return find_chat(current_chat_id_);
}

const std::vector<Message>& TelegramModel::messages() const noexcept
{
    return messages_;
}

const Chat* TelegramModel::find_chat(std::int64_t chat_id) const noexcept
{
    const auto iterator =
        std::find_if(chats_.cbegin(), chats_.cend(), [chat_id](const Chat& chat) { return chat.id == chat_id; });
    if (iterator != chats_.cend()) return &*iterator;
    return transient_chat_ && transient_chat_->id == chat_id ? &*transient_chat_ : nullptr;
}

const Message* TelegramModel::find_message(std::int64_t message_id) const noexcept
{
    const auto iterator = std::find_if(messages_.cbegin(), messages_.cend(),
                                       [message_id](const Message& message) { return message.id == message_id; });
    return iterator == messages_.cend() ? nullptr : &*iterator;
}

bool TelegramModel::set_auth_state(AuthState state)
{
    if (auth_state_ == state) return false;
    auth_state_ = state;
    emit({ModelEventKind::AuthStateChanged});
    return true;
}

bool TelegramModel::set_connection_state(ConnectionState state)
{
    if (connection_state_ == state) return false;
    connection_state_ = state;
    emit({ModelEventKind::ConnectionStateChanged});
    return true;
}

bool TelegramModel::upsert_chat(Chat chat)
{
    if (chat.id == 0) return false;

    const std::int64_t chat_id = chat.id;
    const auto iterator =
        std::find_if(chats_.begin(), chats_.end(), [chat_id](const Chat& item) { return item.id == chat_id; });
    const bool existed = iterator != chats_.end();
    if (!existed && transient_chat_ && transient_chat_->id == chat_id && chat.order == 0) {
        if (*transient_chat_ == chat) return false;
        *transient_chat_ = std::move(chat);
        emit({ModelEventKind::CurrentChatChanged, chat_id});
        return true;
    }
    if (iterator == chats_.end()) {
        chats_.push_back(std::move(chat));
    } else {
        if (*iterator == chat) return false;
        *iterator = std::move(chat);
    }

    sort_and_trim_chats();
    if (transient_chat_ && std::any_of(chats_.begin(), chats_.end(), [this](const Chat& item) {
            return item.id == transient_chat_->id;
        })) transient_chat_.reset();
    if (!existed && !find_chat(chat_id)) return false;
    emit({ModelEventKind::ChatsChanged, chat_id});
    clear_selection_for_missing_chat();
    return true;
}

bool TelegramModel::remove_chat(std::int64_t chat_id)
{
    const auto iterator =
        std::find_if(chats_.begin(), chats_.end(), [chat_id](const Chat& chat) { return chat.id == chat_id; });
    if (iterator == chats_.end()) return false;

    chats_.erase(iterator);
    emit({ModelEventKind::ChatsChanged, chat_id});
    if (current_chat_id_ == chat_id) clear_current_chat();
    return true;
}

bool TelegramModel::clear_chats()
{
    if (chats_.empty() && current_chat_id_ == 0 && messages_.empty() && !transient_chat_) return false;

    chats_.clear();
    transient_chat_.reset();
    emit({ModelEventKind::ChatsChanged});
    clear_current_chat();
    return true;
}

bool TelegramModel::select_chat(std::int64_t chat_id)
{
    if (chat_id == 0) return clear_current_chat();
    if (!find_chat(chat_id) || current_chat_id_ == chat_id) return false;

    current_chat_id_ = chat_id;
    if (transient_chat_ && transient_chat_->id != chat_id) transient_chat_.reset();
    messages_.clear();
    emit({ModelEventKind::CurrentChatChanged, chat_id});
    emit({ModelEventKind::MessagesChanged, chat_id});
    return true;
}

bool TelegramModel::stage_transient_chat(Chat chat)
{
    if (chat.id == 0 || std::any_of(chats_.begin(), chats_.end(), [&](const Chat& item) {
            return item.id == chat.id;
        })) return false;
    if (transient_chat_ && *transient_chat_ == chat) return false;
    transient_chat_ = std::move(chat);
    if (current_chat_id_ == transient_chat_->id) emit({ModelEventKind::CurrentChatChanged, current_chat_id_});
    return true;
}

bool TelegramModel::clear_current_chat()
{
    if (current_chat_id_ == 0 && messages_.empty() && !transient_chat_) return false;

    const std::int64_t previous_chat_id = current_chat_id_;
    current_chat_id_                    = 0;
    transient_chat_.reset();
    messages_.clear();
    emit({ModelEventKind::CurrentChatChanged, previous_chat_id});
    emit({ModelEventKind::MessagesChanged, previous_chat_id});
    return true;
}

bool TelegramModel::upsert_message(Message message, MessageRetention retention)
{
    if (message.id == 0 || current_chat_id_ == 0 || message.chat_id != current_chat_id_) return false;

    const std::int64_t message_id = message.id;
    const auto iterator           = std::find_if(messages_.begin(), messages_.end(),
                                                 [message_id](const Message& item) { return item.id == message_id; });
    const bool existed            = iterator != messages_.end();
    if (iterator == messages_.end()) {
        messages_.push_back(std::move(message));
    } else {
        if (*iterator == message) return false;
        *iterator = std::move(message);
    }

    sort_and_trim_messages(retention);
    if (!existed && !find_message(message_id)) return false;
    emit({ModelEventKind::MessagesChanged, current_chat_id_, message_id});
    return true;
}

bool TelegramModel::remove_message(std::int64_t message_id)
{
    const auto iterator = std::find_if(messages_.begin(), messages_.end(),
                                       [message_id](const Message& message) { return message.id == message_id; });
    if (iterator == messages_.end()) return false;

    messages_.erase(iterator);
    emit({ModelEventKind::MessagesChanged, current_chat_id_, message_id});
    return true;
}

bool TelegramModel::clear_messages()
{
    if (messages_.empty()) return false;
    messages_.clear();
    emit({ModelEventKind::MessagesChanged, current_chat_id_});
    return true;
}

void TelegramModel::clear()
{
    auth_state_       = AuthState::Unknown;
    connection_state_ = ConnectionState::Unknown;
    chats_.clear();
    transient_chat_.reset();
    current_chat_id_ = 0;
    messages_.clear();
    events_.clear();
    events_.push_back({ModelEventKind::Cleared});
}

std::vector<ModelEvent> TelegramModel::take_events()
{
    std::vector<ModelEvent> result;
    result.swap(events_);
    return result;
}

void TelegramModel::emit(ModelEvent event)
{
    if (std::find(events_.cbegin(), events_.cend(), event) == events_.cend()) events_.push_back(event);
}

void TelegramModel::sort_and_trim_chats()
{
    std::sort(chats_.begin(), chats_.end(), [](const Chat& left, const Chat& right) {
        if (left.order != right.order) return left.order > right.order;
        return left.id > right.id;
    });
    if (chats_.size() > kMaxChats) chats_.resize(kMaxChats);
}

void TelegramModel::sort_and_trim_messages(MessageRetention retention)
{
    std::sort(messages_.begin(), messages_.end(), [](const Message& left, const Message& right) {
        if (left.date != right.date) return left.date < right.date;
        return left.id < right.id;
    });
    if (messages_.size() > kMaxCurrentMessages) {
        if (retention == MessageRetention::Oldest) {
            messages_.resize(kMaxCurrentMessages);
        } else {
            messages_.erase(messages_.begin(), messages_.begin() + (messages_.size() - kMaxCurrentMessages));
        }
    }
}

void TelegramModel::clear_selection_for_missing_chat()
{
    if (current_chat_id_ != 0 && !find_chat(current_chat_id_)) clear_current_chat();
}

}  // namespace telegram
