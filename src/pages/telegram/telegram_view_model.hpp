#ifndef LILYGO_UI_TELEGRAM_PAGES_TELEGRAM_TELEGRAM_VIEW_MODEL_HPP
#define LILYGO_UI_TELEGRAM_PAGES_TELEGRAM_TELEGRAM_VIEW_MODEL_HPP

#include "domain/telegram_model.hpp"
#include "domain/telegram_account_model.hpp"
#include "services/bounded_queue.hpp"
#include "services/telegram/telegram_client.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace telegram {

class TelegramViewModel {
public:
    static constexpr std::size_t kEventQueueCapacity   = 256;
    static constexpr std::size_t kMaxPumpEvents        = 64;
    static constexpr std::int32_t kHistoryPageSize     = 40;
    static constexpr std::size_t kMaxMessageCharacters = 4096;
    static constexpr std::size_t kMaxMessageUtf8Bytes  = 16U * 1024U;
    static constexpr std::size_t kMaxSearchCharacters  = 128;
    static constexpr std::size_t kMaxSearchUtf8Bytes   = 512;

    TelegramViewModel(std::unique_ptr<TelegramClient> client, ClientConfig config);
    ~TelegramViewModel();

    TelegramViewModel(const TelegramViewModel&)            = delete;
    TelegramViewModel& operator=(const TelegramViewModel&) = delete;
    TelegramViewModel(TelegramViewModel&&)                 = delete;
    TelegramViewModel& operator=(TelegramViewModel&&)      = delete;

    bool start();
    void stop() noexcept;
    std::size_t pump(std::size_t max_events = kMaxPumpEvents);

    [[nodiscard]] const TelegramModel& model() const noexcept;
    [[nodiscard]] std::uint64_t revision() const noexcept;
    [[nodiscard]] const std::string& status_text() const noexcept;
    [[nodiscard]] const std::string& error_text() const noexcept;
    std::vector<std::int32_t> take_failed_downloads();
    void set_chat_search(std::string query);
    [[nodiscard]] const std::string& chat_search() const noexcept;
    [[nodiscard]] bool chat_matches_search(const Chat& chat) const;
    void set_draft(std::int64_t chat_id, std::string text);
    [[nodiscard]] const std::string& draft(std::int64_t chat_id) const noexcept;
    [[nodiscard]] const AccountState& account() const noexcept;
    bool account_action(AccountRequest request);
    [[nodiscard]] bool account_busy() const noexcept;
    [[nodiscard]] const std::string& account_error() const noexcept;
    [[nodiscard]] const std::string& account_notice() const noexcept;
    void clear_account_feedback();
    [[nodiscard]] const UiPreferences& preferences() const noexcept;
    bool set_preferences(UiPreferences preferences);
    void set_contact_search(std::string query);
    [[nodiscard]] const std::string& contact_search() const noexcept;
    [[nodiscard]] std::vector<Contact> filtered_contacts() const;

    bool submit_auth_input(std::string input);
    bool select_chat(std::int64_t chat_id);
    bool back_to_chats();
    bool load_older();
    bool mark_current_messages_viewed();
    bool mark_message_content_opened(std::int64_t chat_id, std::int64_t message_id);
    bool send_text(std::string text);
    bool request_file(std::int32_t file_id, std::int32_t priority = 16, bool background = false);
    bool background_download_pending(std::int32_t file_id) const;
    bool log_out();

private:
    bool apply_event(ClientEvent event);
    bool apply_file_update(const FileInfo& file);
    bool refresh_status_text();
    bool set_status_text(std::string text);
    bool set_error_text(std::string text);
    bool invoke_client(const char* failure_message, const std::function<void()>& command);
    bool clear_local_chat_state();
    bool prune_drafts();
    bool clear_account_state();
    void discard_model_events();

    std::unique_ptr<TelegramClient> client_;
    ClientConfig config_;
    TelegramModel model_;
    TelegramAccountModel account_model_;
    UiPreferences preferences_;
    std::string contact_search_;
    BoundedQueue<ClientEvent> event_queue_{kEventQueueCapacity};
    std::atomic<bool> accepting_events_{false};
    bool running_            = false;
    bool account_ready_      = false;
    std::uint64_t revision_  = 0;
    std::string status_text_ = "Telegram is stopped";
    std::string error_text_;
    std::string chat_search_;
    std::unordered_map<std::int64_t, std::string> drafts_;
    std::vector<std::int32_t> failed_downloads_;
    std::unordered_set<std::int32_t> background_downloads_;
    std::int64_t viewed_chat_id_           = 0;
    std::int64_t viewed_latest_message_id_ = 0;
};

}  // namespace telegram

#endif
