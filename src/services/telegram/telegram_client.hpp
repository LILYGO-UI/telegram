#ifndef LILYGO_UI_TELEGRAM_SERVICES_TELEGRAM_TELEGRAM_CLIENT_HPP
#define LILYGO_UI_TELEGRAM_SERVICES_TELEGRAM_TELEGRAM_CLIENT_HPP

#include "domain/telegram_types.hpp"
#include "domain/telegram_account.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <stdexcept>
#include <vector>

namespace telegram {

struct ClientConfig {
    std::int32_t api_id = 0;
    std::string api_hash;
    std::string database_directory;
    std::string files_directory;
    std::string database_encryption_key;
    std::string system_language_code = "en";
    std::string device_model         = "LILYGO CM0";
    std::string application_version  = "0.1.0";
    std::string preferences_path;
};

enum class ClientEventKind {
    AuthStateChanged,
    ConnectionStateChanged,
    ChatUpserted,
    ChatRemoved,
    ChatPhotoChanged,
    MessageUpserted,
    MessageRemoved,
    FileUpdated,
    Status,
    Error,
    Account,
};

struct ClientEvent {
    ClientEventKind kind             = ClientEventKind::Status;
    AuthState auth_state             = AuthState::Unknown;
    ConnectionState connection_state = ConnectionState::Unknown;
    Chat chat;
    Message message;
    FileInfo file;
    std::int64_t chat_id    = 0;
    std::int64_t message_id = 0;
    std::string text;
    bool is_history = false;
    AccountUpdate account;
};

class TelegramClient {
public:
    using EventCallback = std::function<void(ClientEvent)>;

    virtual ~TelegramClient() = default;

    TelegramClient(const TelegramClient&)            = delete;
    TelegramClient& operator=(const TelegramClient&) = delete;
    TelegramClient(TelegramClient&&)                 = delete;
    TelegramClient& operator=(TelegramClient&&)      = delete;

    virtual bool start(const ClientConfig& config, EventCallback callback) = 0;
    virtual void stop() noexcept                                           = 0;

    virtual void submit_phone_number(std::string phone_number)   = 0;
    virtual void submit_email_address(std::string email_address) = 0;
    virtual void submit_email_code(std::string code)             = 0;
    virtual void submit_code(std::string code)                   = 0;
    virtual void submit_password(std::string password)           = 0;
    virtual void log_out()                                       = 0;

    virtual void load_chats(std::int32_t limit)                                                       = 0;
    virtual void open_chat(std::int64_t chat_id)                                                      = 0;
    virtual void close_chat(std::int64_t chat_id)                                                     = 0;
    virtual void load_history(std::int64_t chat_id, std::int64_t from_message_id, std::int32_t limit) = 0;
    virtual void view_messages(std::int64_t chat_id, const std::vector<std::int64_t>& message_ids)    = 0;
    virtual void open_message_content(std::int64_t chat_id, std::int64_t message_id)                  = 0;
    virtual void send_text(std::int64_t chat_id, std::string text)                                    = 0;
    virtual void download_file(std::int32_t file_id, std::int32_t priority = 16)                      = 0;
    virtual void account_request(AccountRequest)
    {
        throw std::runtime_error("Account operations are unavailable in this backend");
    }

protected:
    TelegramClient() = default;
};

std::unique_ptr<TelegramClient> make_tdlib_client();
std::unique_ptr<TelegramClient> make_demo_client(std::string demo_image_path = {});

}  // namespace telegram

#endif
