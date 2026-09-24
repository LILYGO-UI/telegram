#ifndef LILYGO_UI_TELEGRAM_ACCOUNT_MODEL_HPP
#define LILYGO_UI_TELEGRAM_ACCOUNT_MODEL_HPP

#include "domain/telegram_account.hpp"

#include <unordered_map>

namespace telegram {

class TelegramAccountModel {
public:
    static constexpr std::size_t kMaxPendingRequests = 16;

    [[nodiscard]] const AccountState& state() const noexcept { return state_; }
    [[nodiscard]] bool busy() const noexcept { return !pending_.empty(); }
    [[nodiscard]] const std::string& error() const noexcept { return error_; }
    [[nodiscard]] const std::string& notice() const noexcept { return notice_; }
    [[nodiscard]] bool accepts(const AccountUpdate& update) const noexcept;
    bool begin(AccountRequest& request);
    bool fail(std::uint64_t request_id, std::string error);
    bool apply(AccountUpdate update);
    bool apply_file_update(const FileInfo& file);
    bool reset();
    bool clear_feedback();
    bool report_error(std::string error);

private:
    AccountState state_;
    std::unordered_map<std::uint64_t, AccountAction> pending_;
    std::uint64_t next_request_id_ = 1;
    bool populated_ = false;
    std::string error_;
    std::string notice_;
};

[[nodiscard]] bool valid_preferences(const UiPreferences& preferences) noexcept;
[[nodiscard]] UiPreferences load_ui_preferences(const std::string& path);
bool save_ui_preferences(const std::string& path, const UiPreferences& preferences, std::string& error);

}  // namespace telegram

#endif
