#ifndef LILYGO_UI_TELEGRAM_DOMAIN_TELEGRAM_MODEL_HPP
#define LILYGO_UI_TELEGRAM_DOMAIN_TELEGRAM_MODEL_HPP

#include "domain/telegram_types.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace telegram {

class TelegramModel {
public:
    enum class MessageRetention {
        Newest,
        Oldest,
    };

    static constexpr std::size_t kMaxChats           = 100;
    static constexpr std::size_t kMaxCurrentMessages = 80;

    [[nodiscard]] AuthState auth_state() const noexcept;
    [[nodiscard]] ConnectionState connection_state() const noexcept;
    [[nodiscard]] const std::vector<Chat>& chats() const noexcept;
    [[nodiscard]] std::int64_t current_chat_id() const noexcept;
    [[nodiscard]] const Chat* current_chat() const noexcept;
    [[nodiscard]] const std::vector<Message>& messages() const noexcept;
    [[nodiscard]] const Chat* find_chat(std::int64_t chat_id) const noexcept;
    [[nodiscard]] const Message* find_message(std::int64_t message_id) const noexcept;

    // Mutators return true only when the observable model state changed.
    bool set_auth_state(AuthState state);
    bool set_connection_state(ConnectionState state);
    bool upsert_chat(Chat chat);
    bool remove_chat(std::int64_t chat_id);
    bool clear_chats();
    bool select_chat(std::int64_t chat_id);
    bool stage_transient_chat(Chat chat);
    bool clear_current_chat();
    bool upsert_message(Message message, MessageRetention retention = MessageRetention::Newest);
    bool remove_message(std::int64_t message_id);
    bool clear_messages();
    void clear();

    [[nodiscard]] std::vector<ModelEvent> take_events();

private:
    void emit(ModelEvent event);
    void sort_and_trim_chats();
    void sort_and_trim_messages(MessageRetention retention);
    void clear_selection_for_missing_chat();

    AuthState auth_state_             = AuthState::Unknown;
    ConnectionState connection_state_ = ConnectionState::Unknown;
    std::vector<Chat> chats_;
    std::optional<Chat> transient_chat_;
    std::int64_t current_chat_id_ = 0;
    std::vector<Message> messages_;
    std::vector<ModelEvent> events_;
};

}  // namespace telegram

#endif
