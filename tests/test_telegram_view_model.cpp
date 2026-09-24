#include "pages/telegram/telegram_view_model.hpp"
#include "support/json.hpp"

#include <cassert>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>
#include <unistd.h>

namespace {

class FakeTelegramClient final : public telegram::TelegramClient {
public:
    bool start(const telegram::ClientConfig& config, EventCallback callback) override
    {
        config_   = config;
        callback_ = std::move(callback);
        started   = true;
        if (!startup_error.empty()) {
            telegram::ClientEvent event;
            event.kind = telegram::ClientEventKind::Error;
            event.text = startup_error;
            callback_(std::move(event));
        }
        return start_result;
    }

    void stop() noexcept override
    {
        stopped = true;
    }

    void submit_phone_number(std::string value) override
    {
        phone = std::move(value);
    }

    void submit_email_address(std::string value) override
    {
        email = std::move(value);
    }

    void submit_email_code(std::string value) override
    {
        email_code = std::move(value);
    }

    void submit_code(std::string value) override
    {
        code = std::move(value);
    }

    void submit_password(std::string value) override
    {
        password = std::move(value);
    }

    void log_out() override
    {
        if (fail_logout) throw std::runtime_error("TDLib command queue is full");
        logged_out = true;
    }

    void load_chats(std::int32_t limit) override
    {
        chat_limit = limit;
    }

    void open_chat(std::int64_t chat_id) override
    {
        opened_chat = chat_id;
    }

    void close_chat(std::int64_t chat_id) override
    {
        closed_chat = chat_id;
    }

    void load_history(std::int64_t chat_id, std::int64_t from_message_id, std::int32_t limit) override
    {
        history_chat  = chat_id;
        history_from  = from_message_id;
        history_limit = limit;
    }

    void view_messages(std::int64_t chat_id, const std::vector<std::int64_t>& message_ids) override
    {
        viewed_chat     = chat_id;
        viewed_messages = message_ids;
        ++view_messages_calls;
    }

    void open_message_content(std::int64_t chat_id, std::int64_t message_id) override
    {
        opened_content_chat    = chat_id;
        opened_content_message = message_id;
    }

    void send_text(std::int64_t chat_id, std::string value) override
    {
        if (fail_send) throw std::runtime_error("TDLib command queue is full");
        sent_chat = chat_id;
        sent_text = std::move(value);
    }

    void download_file(std::int32_t file_id, std::int32_t priority) override
    {
        if (fail_download) throw std::runtime_error("TDLib command queue is full");
        downloaded_file   = file_id;
        download_priority = priority;
    }

    void account_request(telegram::AccountRequest request) override
    {
        if (fail_account) throw std::runtime_error("Account command queue is full");
        account_requests.push_back(std::move(request));
    }

    void emit(telegram::ClientEvent event)
    {
        callback_(std::move(event));
    }

    telegram::ClientConfig config_;
    EventCallback callback_;
    bool start_result = true;
    bool started      = false;
    bool stopped      = false;
    bool logged_out   = false;
    std::string phone;
    std::string email;
    std::string email_code;
    std::string code;
    std::string password;
    std::int32_t chat_limit    = 0;
    std::int64_t opened_chat   = 0;
    std::int64_t closed_chat   = 0;
    std::int64_t history_chat  = 0;
    std::int64_t history_from  = 0;
    std::int32_t history_limit = 0;
    std::int64_t sent_chat     = 0;
    std::string sent_text;
    std::int64_t viewed_chat = 0;
    std::vector<std::int64_t> viewed_messages;
    std::size_t view_messages_calls     = 0;
    std::int64_t opened_content_chat    = 0;
    std::int64_t opened_content_message = 0;
    std::int32_t downloaded_file        = 0;
    std::int32_t download_priority      = 0;
    bool fail_send                      = false;
    bool fail_download                  = false;
    bool fail_logout                    = false;
    bool fail_account                   = false;
    std::vector<telegram::AccountRequest> account_requests;
    std::string startup_error;
};

telegram::ClientEvent auth_event(telegram::AuthState state)
{
    telegram::ClientEvent event;
    event.kind       = telegram::ClientEventKind::AuthStateChanged;
    event.auth_state = state;
    return event;
}

void pump_auth(telegram::TelegramViewModel& view_model, FakeTelegramClient& client, telegram::AuthState state)
{
    client.emit(auth_event(state));
    assert(view_model.pump() == 1);
}

void pump_chat(telegram::TelegramViewModel& view_model, FakeTelegramClient& client, std::int64_t chat_id)
{
    telegram::ClientEvent event;
    event.kind       = telegram::ClientEventKind::ChatUpserted;
    event.chat.id    = chat_id;
    event.chat.order = chat_id;
    client.emit(std::move(event));
    assert(view_model.pump() == 1);
}

std::string repeated(const std::string& value, std::size_t count)
{
    std::string result;
    for (std::size_t index = 0; index < count; ++index) result += value;
    return result;
}

void test_chat_search_matches_titles_previews_and_utf8()
{
    telegram::TelegramViewModel view_model(nullptr, {});
    telegram::Chat chat;
    chat.title                = "Design Team";
    chat.last_message_preview = "Meeting at 10";
    assert(view_model.chat_search().empty());
    assert(view_model.chat_matches_search(chat));

    auto revision = view_model.revision();
    view_model.set_chat_search("  dEsIgN\t");
    assert(view_model.revision() == revision + 1);
    assert(view_model.chat_search() == "  dEsIgN\t");
    assert(view_model.chat_matches_search(chat));
    revision = view_model.revision();
    view_model.set_chat_search(view_model.chat_search());
    assert(view_model.revision() == revision);

    view_model.set_chat_search("AT 10");
    assert(view_model.chat_matches_search(chat));
    view_model.set_chat_search("TeamMeeting");
    assert(!view_model.chat_matches_search(chat));
    view_model.set_chat_search(" \t\n ");
    assert(view_model.chat_matches_search(chat));

    chat.title                = u8"\u8bbe\u8ba1\u56e2\u961f";
    chat.last_message_preview = u8"Caf\u00e9 \u4f1a\u8bae";
    view_model.set_chat_search(u8"\u8bbe\u8ba1");
    assert(view_model.chat_matches_search(chat));
    view_model.set_chat_search(u8"CAF\u00e9");
    assert(view_model.chat_matches_search(chat));
    view_model.set_chat_search(u8"CAF\u00c9");
    assert(!view_model.chat_matches_search(chat));
    view_model.set_chat_search(u8"\u4f1a\u8bae");
    assert(view_model.chat_matches_search(chat));
    view_model.set_chat_search(u8"\u6d88\u606f");
    assert(!view_model.chat_matches_search(chat));
}

void test_chat_search_preserves_utf8_at_input_limits()
{
    telegram::TelegramViewModel view_model(nullptr, {});
    view_model.set_chat_search(std::string(129, 'x'));
    assert(view_model.chat_search() == std::string(128, 'x'));
    const auto revision = view_model.revision();
    view_model.set_chat_search(std::string(256, 'x'));
    assert(view_model.revision() == revision);

    const std::string symbol = u8"\U0001f4ac";
    view_model.set_chat_search(repeated(symbol, 129));
    assert(view_model.chat_search() == repeated(symbol, 128));
    assert(view_model.chat_search().size() == 512);
    view_model.set_chat_search(std::string(127, 'x') + symbol + "extra");
    assert(view_model.chat_search() == std::string(127, 'x') + symbol);
    view_model.set_chat_search(std::string("valid") + "\xf0\x9f");
    assert(view_model.chat_search() == "valid");
}

void test_drafts_remain_with_their_chat_and_survive_send_attempts()
{
    auto client = std::make_unique<FakeTelegramClient>();
    auto* fake  = client.get();
    telegram::TelegramViewModel view_model(std::move(client), {});
    assert(view_model.start());
    pump_auth(view_model, *fake, telegram::AuthState::Ready);
    pump_chat(view_model, *fake, 7);
    pump_chat(view_model, *fake, 8);
    assert(view_model.draft(7).empty());

    auto revision = view_model.revision();
    view_model.set_draft(0, "invalid");
    view_model.set_draft(99, "unknown");
    view_model.set_draft(7, {});
    assert(view_model.revision() == revision);
    assert(view_model.draft(99).empty());

    view_model.set_draft(7, "  draft seven\n");
    assert(view_model.revision() == revision + 1);
    revision = view_model.revision();
    view_model.set_draft(7, "  draft seven\n");
    assert(view_model.revision() == revision);
    view_model.set_draft(8, "draft eight");
    assert(view_model.select_chat(7));
    assert(view_model.draft(7) == "  draft seven\n");
    assert(view_model.back_to_chats());
    assert(view_model.select_chat(8));
    assert(view_model.draft(8) == "draft eight");
    assert(view_model.draft(7) == "  draft seven\n");

    fake->fail_send = true;
    assert(!view_model.send_text(view_model.draft(8)));
    assert(view_model.draft(8) == "draft eight");
    fake->fail_send = false;
    assert(view_model.send_text(view_model.draft(8)));
    assert(fake->sent_chat == 8);
    assert(fake->sent_text == "draft eight");
    assert(view_model.draft(8) == "draft eight");
    view_model.set_draft(8, {});
    assert(view_model.draft(8).empty());
    assert(view_model.draft(7) == "  draft seven\n");

    view_model.set_draft(7, std::string(4097, 'x'));
    assert(view_model.draft(7) == std::string(4096, 'x'));
    const std::string symbol = u8"\U0001f4ac";
    view_model.set_draft(7, repeated(symbol, 4097));
    assert(view_model.draft(7) == repeated(symbol, 4096));
    assert(view_model.draft(7).size() == telegram::TelegramViewModel::kMaxMessageUtf8Bytes);
}

void test_drafts_are_pruned_with_the_bounded_chat_list()
{
    auto client = std::make_unique<FakeTelegramClient>();
    auto* fake  = client.get();
    telegram::TelegramViewModel view_model(std::move(client), {});
    assert(view_model.start());
    for (std::size_t index = 1; index <= telegram::TelegramModel::kMaxChats; ++index) {
        const auto id = static_cast<std::int64_t>(index);
        pump_chat(view_model, *fake, id);
        view_model.set_draft(id, "draft " + std::to_string(id));
    }
    assert(view_model.draft(1) == "draft 1");
    const auto new_id = static_cast<std::int64_t>(telegram::TelegramModel::kMaxChats + 1);
    pump_chat(view_model, *fake, new_id);
    assert(!view_model.model().find_chat(1));
    assert(view_model.draft(1).empty());
    view_model.set_draft(new_id, "replacement draft");
    assert(view_model.draft(new_id) == "replacement draft");

    telegram::ClientEvent event;
    event.kind    = telegram::ClientEventKind::ChatRemoved;
    event.chat_id = new_id;
    fake->emit(std::move(event));
    assert(view_model.pump() == 1);
    assert(view_model.draft(new_id).empty());
    pump_chat(view_model, *fake, 1);
    assert(view_model.draft(1).empty());
    assert(view_model.draft(2) == "draft 2");
}

void test_local_chat_state_clears_when_authentication_resets()
{
    auto client = std::make_unique<FakeTelegramClient>();
    auto* fake  = client.get();
    telegram::TelegramViewModel view_model(std::move(client), {});
    assert(view_model.start());
    pump_auth(view_model, *fake, telegram::AuthState::Ready);
    pump_chat(view_model, *fake, 7);
    view_model.set_draft(7, "draft");
    view_model.set_chat_search("query");
    pump_auth(view_model, *fake, telegram::AuthState::Error);
    assert(view_model.draft(7) == "draft");
    assert(view_model.chat_search() == "query");
    pump_auth(view_model, *fake, telegram::AuthState::Ready);

    fake->fail_logout = true;
    assert(!view_model.log_out());
    assert(view_model.draft(7) == "draft");
    assert(view_model.chat_search() == "query");
    fake->fail_logout = false;
    assert(view_model.log_out());
    assert(view_model.draft(7).empty());
    assert(view_model.chat_search().empty());

    for (const auto state : {telegram::AuthState::WaitingForPhoneNumber, telegram::AuthState::WaitingForParameters,
                             telegram::AuthState::LoggingOut, telegram::AuthState::Closed}) {
        view_model.set_draft(7, "draft");
        view_model.set_chat_search("query");
        pump_auth(view_model, *fake, state);
        assert(view_model.draft(7).empty());
        assert(view_model.chat_search().empty());
        pump_auth(view_model, *fake, telegram::AuthState::Ready);
        pump_chat(view_model, *fake, 7);
        assert(view_model.draft(7).empty());
    }
}

void test_auth_dispatch_and_status()
{
    auto client = std::make_unique<FakeTelegramClient>();
    auto* fake  = client.get();
    telegram::ClientConfig config;
    config.api_id = 123;
    telegram::TelegramViewModel view_model(std::move(client), config);
    assert(view_model.start());
    assert(fake->started);
    assert(fake->config_.api_id == 123);

    pump_auth(view_model, *fake, telegram::AuthState::WaitingForPhoneNumber);
    assert(view_model.submit_auth_input("  +12345  "));
    assert(fake->phone == "+12345");

    pump_auth(view_model, *fake, telegram::AuthState::WaitingForEmailAddress);
    assert(view_model.submit_auth_input(" user@example.com "));
    assert(fake->email == "user@example.com");

    pump_auth(view_model, *fake, telegram::AuthState::WaitingForEmailCode);
    assert(view_model.submit_auth_input(" 1234 "));
    assert(fake->email_code == "1234");

    pump_auth(view_model, *fake, telegram::AuthState::WaitingForCode);
    assert(view_model.submit_auth_input(" 5678 "));
    assert(fake->code == "5678");

    auto password_event = auth_event(telegram::AuthState::WaitingForPassword);
    password_event.text = "first pet";
    fake->emit(std::move(password_event));
    assert(view_model.pump() == 1);
    assert(view_model.status_text() == "Enter two-step verification password. Hint: first pet");
    assert(view_model.submit_auth_input(" password with spaces "));
    assert(fake->password == " password with spaces ");

    auto confirmation_event = auth_event(telegram::AuthState::WaitingForOtherDeviceConfirmation);
    confirmation_event.text = "tg://login?token=abc";
    fake->emit(std::move(confirmation_event));
    assert(view_model.pump() == 1);
    assert(view_model.status_text() == "Confirm login on another device: tg://login?token=abc");

    pump_auth(view_model, *fake, telegram::AuthState::Ready);
    assert(fake->chat_limit == static_cast<std::int32_t>(telegram::TelegramModel::kMaxChats));
    assert(!view_model.submit_auth_input("unexpected"));
    assert(!view_model.error_text().empty());
}

void test_chat_messages_files_and_commands()
{
    auto client = std::make_unique<FakeTelegramClient>();
    auto* fake  = client.get();
    telegram::TelegramViewModel view_model(std::move(client), {});
    assert(view_model.start());

    telegram::ClientEvent chat_event;
    chat_event.kind          = telegram::ClientEventKind::ChatUpserted;
    chat_event.chat.id       = 7;
    chat_event.chat.order    = 100;
    chat_event.chat.title    = "Chat";
    chat_event.chat.photo.id = 77;
    fake->emit(chat_event);
    assert(view_model.pump() == 1);
    assert(view_model.select_chat(7));
    assert(fake->opened_chat == 7);
    assert(fake->history_chat == 7);
    assert(fake->history_from == 0);
    assert(fake->history_limit == telegram::TelegramViewModel::kHistoryPageSize);

    telegram::ClientEvent message_event;
    message_event.kind            = telegram::ClientEventKind::MessageUpserted;
    message_event.message.id      = 10;
    message_event.message.chat_id = 7;
    message_event.message.date    = 100;
    message_event.message.kind    = telegram::MessageKind::Photo;
    message_event.message.file.id = 77;
    fake->emit(std::move(message_event));
    assert(view_model.pump() == 1);
    assert(view_model.model().messages().size() == 1);
    assert(view_model.mark_current_messages_viewed());
    assert(fake->viewed_chat == 7);
    assert(fake->viewed_messages == std::vector<std::int64_t>{10});
    assert(fake->view_messages_calls == 1);
    assert(view_model.mark_current_messages_viewed());
    assert(fake->view_messages_calls == 1);
    assert(view_model.mark_message_content_opened(7, 10));
    assert(fake->opened_content_chat == 7);
    assert(fake->opened_content_message == 10);

    telegram::ClientEvent file_event;
    file_event.kind                 = telegram::ClientEventKind::FileUpdated;
    file_event.file.id              = 77;
    file_event.file.size            = 1000;
    file_event.file.downloaded_size = 1000;
    file_event.file.local_path      = "/tmp/image.jpg";
    file_event.file.is_downloaded   = true;
    fake->emit(std::move(file_event));
    assert(view_model.pump() == 1);
    assert(view_model.model().current_chat()->photo.local_path == "/tmp/image.jpg");
    assert(view_model.model().messages().front().file.local_path == "/tmp/image.jpg");

    telegram::ClientEvent download_error;
    download_error.kind    = telegram::ClientEventKind::Error;
    download_error.file.id = 88;
    download_error.text    = "download file failed";
    fake->emit(std::move(download_error));
    assert(view_model.pump() == 1);
    const auto failed_downloads = view_model.take_failed_downloads();
    assert(failed_downloads.size() == 1 && failed_downloads.front() == 88);
    assert(view_model.take_failed_downloads().empty());

    const auto foreground_error = view_model.error_text();
    assert(view_model.request_file(89, 8, true));
    download_error.file.id = 89;
    download_error.text = "download file failed (code 400): File download has failed or was canceled";
    fake->emit(download_error);
    assert(view_model.pump() == 1);
    assert(view_model.error_text() == foreground_error);
    assert(view_model.take_failed_downloads() == std::vector<std::int32_t>{89});

    assert(view_model.request_file(91, 8, true));
    assert(view_model.request_file(92, 8, true));
    assert(view_model.request_file(93, 8, true));
    assert(view_model.request_file(91, 8, true));
    assert(!view_model.request_file(94, 8, true));
    assert(view_model.background_download_pending(91));
    file_event.kind = telegram::ClientEventKind::FileUpdated;
    file_event.file.id = 91;
    file_event.file.is_downloaded = true;
    fake->emit(file_event);
    assert(view_model.pump() == 1);
    assert(!view_model.background_download_pending(91));
    assert(view_model.request_file(94, 8, true));
    fake->fail_download = true;
    assert(!view_model.request_file(90, 8, true));
    assert(view_model.error_text() == foreground_error);
    fake->fail_download = false;

    assert(view_model.request_file(89));
    fake->emit(download_error);
    assert(view_model.pump() == 1);
    assert(view_model.error_text() == download_error.text);
    assert(view_model.take_failed_downloads() == std::vector<std::int32_t>{89});

    assert(view_model.load_older());
    assert(fake->history_from == 10);
    assert(view_model.send_text("  hello  "));
    assert(fake->sent_chat == 7);
    assert(fake->sent_text == "hello");
    fake->fail_send = true;
    assert(!view_model.send_text("retry me"));
    assert(view_model.error_text().find("command queue is full") != std::string::npos);
    fake->fail_send = false;
    assert(!view_model.send_text(" \n "));
    assert(!view_model.send_text(std::string(telegram::TelegramViewModel::kMaxMessageUtf8Bytes + 1, 'x')));
    assert(view_model.error_text() == "Message is too long");
    assert(view_model.request_file(77, 100));
    assert(fake->downloaded_file == 77);
    assert(fake->download_priority == 32);
    fake->fail_download = true;
    assert(!view_model.request_file(78));
    fake->fail_download = false;
    assert(view_model.back_to_chats());
    assert(fake->closed_chat == 7);
    assert(view_model.model().current_chat_id() == 0);
    assert(view_model.model().messages().empty());
    assert(view_model.log_out());
    assert(fake->logged_out);

    view_model.stop();
    assert(fake->stopped);
    assert(!view_model.start());
}

void test_pump_limit()
{
    auto client = std::make_unique<FakeTelegramClient>();
    auto* fake  = client.get();
    telegram::TelegramViewModel view_model(std::move(client), {});
    assert(view_model.start());
    for (int index = 0; index < 70; ++index) {
        telegram::ClientEvent event;
        event.kind = telegram::ClientEventKind::Status;
        event.text = std::to_string(index);
        fake->emit(std::move(event));
    }
    assert(view_model.pump(1000) == telegram::TelegramViewModel::kMaxPumpEvents);
    assert(view_model.pump(1000) == 6);
    assert(view_model.status_text() == "69");
}

void test_history_moves_the_bounded_window_toward_older_messages()
{
    auto client = std::make_unique<FakeTelegramClient>();
    auto* fake  = client.get();
    telegram::TelegramViewModel view_model(std::move(client), {});
    assert(view_model.start());

    telegram::ClientEvent chat_event;
    chat_event.kind       = telegram::ClientEventKind::ChatUpserted;
    chat_event.chat.id    = 7;
    chat_event.chat.order = 100;
    fake->emit(std::move(chat_event));
    assert(view_model.pump() == 1);
    assert(view_model.select_chat(7));

    for (std::int64_t id = 100; id < 180; ++id) {
        telegram::ClientEvent event;
        event.kind            = telegram::ClientEventKind::MessageUpserted;
        event.message.id      = id;
        event.message.chat_id = 7;
        event.message.date    = id;
        fake->emit(std::move(event));
    }
    assert(view_model.pump() == telegram::TelegramViewModel::kMaxPumpEvents);
    assert(view_model.pump() == 16);

    telegram::ClientEvent older;
    older.kind            = telegram::ClientEventKind::MessageUpserted;
    older.message.id      = 99;
    older.message.chat_id = 7;
    older.message.date    = 99;
    older.is_history      = true;
    fake->emit(std::move(older));
    assert(view_model.pump() == 1);
    assert(view_model.model().messages().front().id == 99);
    assert(view_model.model().messages().back().id == 178);
}

void test_start_failure_is_stopped()
{
    const std::string detail =
        "TDLib JSON runtime is unavailable; install libtdjson or set LILYGO_UI_TDJSON_LIBRARY: libtdjson.so: file not "
        "found";
    auto client         = std::make_unique<FakeTelegramClient>();
    auto* fake          = client.get();
    fake->start_result  = false;
    fake->startup_error = detail;
    telegram::TelegramViewModel view_model(std::move(client), {});
    assert(!view_model.start());
    assert(fake->started);
    assert(fake->stopped);
    assert(view_model.error_text() == detail);
    assert(!view_model.start());
}

void pump_account(telegram::TelegramViewModel& model, FakeTelegramClient& client, telegram::AccountUpdate update)
{
    telegram::ClientEvent event;
    event.kind = telegram::ClientEventKind::Account;
    event.account = std::move(update);
    client.emit(std::move(event));
    assert(model.pump() == 1);
}

void test_account_validation_rejection_and_async_errors()
{
    using namespace telegram;
    auto client = std::make_unique<FakeTelegramClient>();
    auto* fake = client.get();
    TelegramViewModel model(std::move(client), {});
    AccountRequest load;
    load.action = AccountAction::LoadContacts;
    assert(!model.account_action(load));
    assert(!model.account_error().empty());
    assert(model.start());
    pump_auth(model, *fake, AuthState::Ready);
    assert(fake->account_requests.empty());

    AccountRequest request;
    request.action = AccountAction::SetName;
    request.profile.user.first_name = " ";
    assert(!model.account_action(request));
    request.profile.user.first_name = repeated(u8"\u59d3", 65);
    assert(!model.account_action(request));
    request.profile.user.first_name = "First";
    fake->fail_account = true;
    assert(!model.account_action(request));
    assert(!model.account_busy());
    assert(model.account_error().find("queue is full") != std::string::npos);
    assert(model.error_text().empty());
    fake->fail_account = false;
    assert(model.account_action(request));
    assert(model.account_busy());
    assert(model.account_error().empty());
    const auto request_id = fake->account_requests.back().request_id;
    assert(request_id != 0);

    AccountUpdate failed;
    failed.kind = AccountUpdateKind::RequestFinished;
    failed.request_id = request_id;
    failed.success = false;
    failed.text = "NAME_INVALID";
    pump_account(model, *fake, failed);
    assert(!model.account_busy());
    assert(model.account_error() == "NAME_INVALID");
    assert(model.account_notice().empty());
    assert(!model.account().profile.loaded);
    assert(model.error_text().empty());
    failed.success = true;
    pump_account(model, *fake, failed);
    assert(model.account_notice().empty());

    request.action = AccountAction::SetUsername;
    request.profile.user.username = "1invalid";
    assert(!model.account_action(request));
    request.profile.user.username = "ok_name";
    assert(model.account_action(request));
    assert(fake->account_requests.back().request_id > request_id);
    AccountUpdate updated;
    updated.kind = AccountUpdateKind::Profile;
    updated.request_id = fake->account_requests.back().request_id;
    updated.profile.loaded = true;
    updated.profile.user.username = "ok_name";
    pump_account(model, *fake, updated);
    assert(model.account().profile.user.username == "ok_name");
    assert(model.account_notice().empty());
    updated.kind = AccountUpdateKind::RequestFinished;
    pump_account(model, *fake, updated);
    assert(model.account_notice() == "Profile updated");
    model.clear_account_feedback();
    assert(model.account_notice().empty());

    request = {};
    request.action = AccountAction::AddContact;
    request.contact.first_name = "Contact";
    request.contact.phone_number = "+12345abc";
    assert(!model.account_action(request));
    request.contact.phone_number.clear();
    request.contact.id = 42;
    assert(!model.account_action(request));
    request.action = AccountAction::SetBio;
    request.profile.bio = repeated(u8"\u6587", 71);
    assert(!model.account_action(request));
    request = {};
    request.action = AccountAction::SetPrivacy;
    request.visibility = PrivacyVisibility::Custom;
    assert(!model.account_action(request));
    request.action = AccountAction::TerminateSession;
    request.target_id = 1;
    assert(!model.account_action(request));
    AccountUpdate sessions;
    sessions.kind = AccountUpdateKind::Sessions;
    AccountSession current;
    current.id = 1;
    current.is_current = true;
    sessions.sessions.push_back(current);
    pump_account(model, *fake, sessions);
    assert(!model.account_action(request));
}

void test_account_request_bounds_and_auth_reset_ignore_stale_data()
{
    using namespace telegram;
    auto client = std::make_unique<FakeTelegramClient>();
    auto* fake = client.get();
    TelegramViewModel model(std::move(client), {});
    assert(model.start());
    pump_auth(model, *fake, AuthState::Ready);
    AccountRequest load;
    load.action = AccountAction::LoadContacts;
    for (std::size_t index = 0; index < TelegramAccountModel::kMaxPendingRequests; ++index)
        assert(model.account_action(load));
    assert(!model.account_action(load));
    assert(fake->account_requests.size() == TelegramAccountModel::kMaxPendingRequests);
    const auto stale_id = fake->account_requests.back().request_id;
    AccountUpdate contacts;
    contacts.kind = AccountUpdateKind::Contacts;
    contacts.request_id = stale_id;
    contacts.total_count = 1;
    Contact contact;
    contact.id = 8;
    contact.first_name = "Stored";
    contacts.contacts.push_back(contact);
    pump_account(model, *fake, contacts);
    assert(model.account().contacts_loaded);
    model.set_contact_search("Stored");
    assert(model.log_out());
    assert(!model.account_busy());
    assert(!model.account().contacts_loaded);
    assert(model.account().contacts.empty());
    assert(model.contact_search().empty());
    contacts.request_id = 0;
    pump_account(model, *fake, contacts);
    assert(model.account().contacts.empty());
    assert(!model.account_action(load));
    pump_auth(model, *fake, AuthState::WaitingForPhoneNumber);
    pump_auth(model, *fake, AuthState::Ready);
    assert(model.account_action(load));
    const auto fresh_id = fake->account_requests.back().request_id;
    assert(fresh_id > stale_id);
    contacts.request_id = stale_id;
    pump_account(model, *fake, contacts);
    assert(model.account().contacts.empty());
    contacts.kind = AccountUpdateKind::RequestFinished;
    pump_account(model, *fake, contacts);
    assert(model.account_busy());
    assert(model.account_notice().empty());
    contacts.request_id = fresh_id;
    pump_account(model, *fake, contacts);
    assert(!model.account_busy());
}

void test_contact_search_sorting_and_account_data_bounds()
{
    using namespace telegram;
    auto client = std::make_unique<FakeTelegramClient>();
    auto* fake = client.get();
    TelegramViewModel model(std::move(client), {});
    assert(model.start());
    pump_auth(model, *fake, AuthState::Ready);
    AccountUpdate contacts;
    contacts.kind = AccountUpdateKind::Contacts;
    Contact alice;
    alice.id = 1;
    alice.first_name = "Alice";
    alice.username = "design_alice";
    alice.phone_number = "+861234567890";
    alice.is_contact = true;
    Contact bob;
    bob.id = 2;
    bob.first_name = "bob";
    bob.is_online = true;
    bob.is_contact = true;
    Contact chinese;
    chinese.id = 3;
    chinese.first_name = u8"\u5f20\u4e09";
    chinese.is_contact = true;
    contacts.contacts = {bob, chinese, alice, alice};
    pump_account(model, *fake, contacts);
    assert(model.account().contacts.size() == 3);
    assert(model.filtered_contacts().front().id == 1);
    auto preferences = model.preferences();
    preferences.contacts_sort_online = true;
    assert(model.set_preferences(preferences));
    assert(model.filtered_contacts().front().id == 2);
    model.set_contact_search("  ALICE ");
    assert(model.filtered_contacts().size() == 1);
    model.set_contact_search("DESIGN_");
    assert(model.filtered_contacts().size() == 1);
    model.set_contact_search("1234567890");
    assert(model.filtered_contacts().size() == 1);
    model.set_contact_search(u8"\u5f20");
    assert(model.filtered_contacts().size() == 1);
    model.set_contact_search(repeated(u8"\U0001f4ac", 129));
    assert(model.contact_search().size() == 512);
    model.set_contact_search({});

    contacts.contacts.clear();
    for (std::int64_t id = 1; id <= 700; ++id) {
        Contact contact;
        contact.id = id;
        contact.first_name = std::to_string(id);
        contacts.contacts.push_back(contact);
    }
    contacts.total_count = 700;
    pump_account(model, *fake, contacts);
    assert(model.account().contacts.size() == kMaxContacts);
    assert(model.account().contacts_total == 700);
    assert(model.filtered_contacts().size() == kMaxContacts);
    AccountUpdate changed;
    changed.kind = AccountUpdateKind::ContactChanged;
    changed.contact.id = 1;
    changed.contact.is_contact = false;
    pump_account(model, *fake, changed);
    assert(model.account().contacts.size() == kMaxContacts - 1);
    assert(model.account().contacts_total == 699);

    AccountUpdate sessions;
    sessions.kind = AccountUpdateKind::Sessions;
    for (std::int64_t id = 1; id <= 150; ++id) {
        AccountSession session;
        session.id = id;
        sessions.sessions.push_back(session);
    }
    pump_account(model, *fake, sessions);
    assert(model.account().sessions_loaded);
    assert(model.account().sessions.size() == kMaxAccountSessions);
    AccountUpdate blocked;
    blocked.kind = AccountUpdateKind::Blocked;
    blocked.total_count = 700;
    for (std::int64_t id = 1; id <= 700; ++id) {
        BlockedSender sender;
        sender.id = id;
        blocked.blocked.push_back(sender);
    }
    pump_account(model, *fake, blocked);
    assert(model.account().blocked_loaded);
    assert(model.account().blocked.size() == kMaxBlockedSenders);
    assert(model.account().blocked_total == 700);
    AccountUpdate notifications;
    notifications.kind = AccountUpdateKind::Notifications;
    notifications.scope = NotificationScope::GroupChats;
    notifications.notifications.loaded = true;
    notifications.notifications.enabled = false;
    pump_account(model, *fake, notifications);
    assert(model.account().notifications[1].loaded);
    assert(!model.account().notifications[1].enabled);
    notifications.scope = static_cast<NotificationScope>(999);
    pump_account(model, *fake, notifications);
    AccountUpdate privacy;
    privacy.kind = AccountUpdateKind::Privacy;
    privacy.privacy_key = PrivacyKey::ProfilePhoto;
    privacy.privacy.loaded = true;
    privacy.privacy.visibility = PrivacyVisibility::Contacts;
    pump_account(model, *fake, privacy);
    assert(model.account().privacy[2].visibility == PrivacyVisibility::Contacts);
    AccountUpdate storage;
    storage.kind = AccountUpdateKind::Storage;
    storage.storage.loaded = true;
    storage.storage.files_size = 1234;
    pump_account(model, *fake, storage);
    storage.kind = AccountUpdateKind::MediaCacheCleared;
    pump_account(model, *fake, storage);
    assert(model.account().storage.files_size == 1234);
    assert(model.account().media_cache_generation == 1);
    pump_auth(model, *fake, AuthState::WaitingForPhoneNumber);
    assert(!model.account().sessions_loaded);
    assert(model.account().sessions.empty());
    assert(!model.account().blocked_loaded);
    assert(model.account().blocked.empty());
    assert(!model.account().notifications[1].loaded);
    assert(!model.account().privacy[2].loaded);
    assert(!model.account().storage.loaded);
    assert(model.account().media_cache_generation == 0);
}

void test_contact_conversation_remains_outside_main_list_until_listed()
{
    using namespace telegram;
    auto client = std::make_unique<FakeTelegramClient>();
    auto* fake = client.get();
    TelegramViewModel model(std::move(client), {});
    assert(model.start());
    pump_auth(model, *fake, AuthState::Ready);
    pump_chat(model, *fake, 7);
    assert(model.select_chat(7));
    AccountRequest open;
    open.action = AccountAction::OpenContact;
    open.target_id = 42;
    assert(model.account_action(open));
    AccountUpdate ready;
    ready.kind = AccountUpdateKind::PrivateChatReady;
    ready.request_id = fake->account_requests.back().request_id;
    ready.chat.id = 99;
    ready.chat.title = "New contact";
    ready.chat.photo.id = 501;
    assert(ready.chat.order == 0);
    pump_account(model, *fake, ready);
    assert(model.model().chats().size() == 1);
    assert(model.model().current_chat_id() == 99);
    assert(model.model().current_chat()->title == "New contact");
    assert(fake->closed_chat == 7);
    assert(fake->opened_chat == 99);
    assert(fake->history_chat == 99);
    model.set_draft(99, "Unsent");
    pump_chat(model, *fake, 8);
    assert(model.model().current_chat_id() == 99);
    assert(model.draft(99) == "Unsent");
    ClientEvent removed;
    removed.kind = ClientEventKind::ChatRemoved;
    removed.chat_id = 99;
    fake->emit(removed);
    model.pump();
    assert(model.model().current_chat_id() == 99);
    ClientEvent avatar;
    avatar.kind = ClientEventKind::ChatPhotoChanged;
    avatar.chat_id = 99;
    avatar.file.id = 502;
    avatar.file.local_path = "/tmp/new-contact-avatar.jpg";
    avatar.file.is_downloaded = true;
    auto revision = model.revision();
    fake->emit(avatar);
    model.pump();
    assert(model.revision() > revision);
    assert(model.model().current_chat()->photo == avatar.file);
    assert(model.model().current_chat()->order == 0);
    assert(model.model().chats().size() == 2);
    assert(model.model().messages().empty());
    revision = model.revision();
    fake->emit(avatar);
    model.pump();
    assert(model.revision() == revision);
    avatar.file = {};
    fake->emit(avatar);
    model.pump();
    assert(model.model().current_chat()->photo.id == 0);
    ClientEvent old_avatar;
    old_avatar.kind = ClientEventKind::FileUpdated;
    old_avatar.file.id = 502;
    old_avatar.file.local_path = "/tmp/old-contact-avatar.jpg";
    old_avatar.file.is_downloaded = true;
    fake->emit(old_avatar);
    model.pump();
    assert(model.model().current_chat()->photo.id == 0);
    avatar.chat_id = 777;
    avatar.file.id = 700;
    fake->emit(avatar);
    model.pump();
    assert(!model.model().find_chat(777));
    assert(model.model().chats().size() == 2);
    avatar.chat_id = 7;
    fake->emit(avatar);
    model.pump();
    assert(model.model().find_chat(7)->photo.id == 700);
    assert(model.model().current_chat_id() == 99);
    assert(model.model().chats().size() == 2);
    ClientEvent message;
    message.kind = ClientEventKind::MessageUpserted;
    message.message.chat_id = 99;
    message.message.id = 1;
    fake->emit(message);
    model.pump();
    assert(model.model().messages().size() == 1);
    pump_chat(model, *fake, 99);
    assert(model.model().chats().size() == 3);
    assert(model.model().current_chat_id() == 99);
    assert(model.model().messages().size() == 1);
    assert(model.back_to_chats());
    assert(model.model().find_chat(99));

    assert(model.account_action(open));
    ready.request_id = fake->account_requests.back().request_id;
    ready.chat.id = 100;
    ready.chat.order = 200;
    ready.chat.photo.id = 333;
    pump_account(model, *fake, ready);
    ClientEvent photo;
    photo.kind = ClientEventKind::FileUpdated;
    photo.file.id = 333;
    photo.file.local_path = "/tmp/contact-photo.png";
    fake->emit(photo);
    model.pump();
    assert(model.model().chats().size() == 3);
    assert(model.model().current_chat()->photo.local_path == photo.file.local_path);
    assert(model.back_to_chats());
    assert(!model.model().find_chat(100));
    assert(fake->closed_chat == 100);
}

void test_zero_id_is_valid_only_for_the_current_session()
{
    using namespace telegram;
    auto client = std::make_unique<FakeTelegramClient>();
    auto* fake = client.get();
    TelegramViewModel model(std::move(client), {});
    assert(model.start());
    pump_auth(model, *fake, AuthState::Ready);

    AccountUpdate sessions;
    sessions.kind = AccountUpdateKind::Sessions;
    sessions.sessions.emplace_back();
    AccountSession current;
    current.is_current = true;
    current.device_model = "This device";
    sessions.sessions.push_back(current);
    sessions.sessions.push_back(current);
    AccountSession remote;
    remote.id = 42;
    sessions.sessions.push_back(remote);
    pump_account(model, *fake, sessions);
    assert(model.account().sessions.size() == 2);
    assert(model.account().sessions.front().id == 0);
    assert(model.account().sessions.front().is_current);
    assert(model.account().sessions.back().id == 42);

    AccountRequest terminate;
    terminate.action = AccountAction::TerminateSession;
    terminate.target_id = 0;
    assert(!model.account_action(terminate));
    assert(fake->account_requests.empty());

    AccountUpdate contacts;
    contacts.kind = AccountUpdateKind::Contacts;
    contacts.contacts.emplace_back();
    pump_account(model, *fake, contacts);
    assert(model.account().contacts.empty());
    AccountUpdate blocked;
    blocked.kind = AccountUpdateKind::Blocked;
    blocked.blocked.emplace_back();
    pump_account(model, *fake, blocked);
    assert(model.account().blocked.empty());
}

void test_avatar_file_updates_replacement_and_removal()
{
    using namespace telegram;
    auto client = std::make_unique<FakeTelegramClient>();
    auto* fake = client.get();
    TelegramViewModel model(std::move(client), {});
    assert(model.start());
    pump_auth(model, *fake, AuthState::Ready);

    FileInfo initial;
    initial.id = 401;
    ClientEvent chat;
    chat.kind = ClientEventKind::ChatUpserted;
    chat.chat.id = 42;
    chat.chat.order = 100;
    chat.chat.photo = initial;
    fake->emit(chat);
    model.pump();
    assert(model.select_chat(42));
    ClientEvent message;
    message.kind = ClientEventKind::MessageUpserted;
    message.message.id = 1;
    message.message.chat_id = 42;
    message.message.sender_id = 9;
    message.message.sender_photo = initial;
    message.message.file.id = 999;
    fake->emit(message);
    model.pump();
    AccountUpdate contacts;
    contacts.kind = AccountUpdateKind::Contacts;
    Contact contact;
    contact.id = 9;
    contact.first_name = "Alice";
    contact.is_contact = true;
    contact.photo = initial;
    contacts.contacts.push_back(contact);
    pump_account(model, *fake, contacts);
    AccountUpdate profile;
    profile.kind = AccountUpdateKind::Profile;
    profile.profile.loaded = true;
    profile.profile.user = contact;
    pump_account(model, *fake, profile);

    ClientEvent file;
    file.kind = ClientEventKind::FileUpdated;
    file.file = initial;
    file.file.is_downloading = true;
    file.file.downloaded_size = 17;
    auto revision = model.revision();
    fake->emit(file);
    model.pump();
    assert(model.revision() > revision);
    assert(model.model().current_chat()->photo == file.file);
    assert(model.model().messages().front().sender_photo == file.file);
    assert(model.model().messages().front().file.id == 999);
    assert(model.account().contacts.front().photo == file.file);
    assert(model.account().profile.user.photo == file.file);
    revision = model.revision();
    fake->emit(file);
    model.pump();
    assert(model.revision() == revision);

    file.file.is_downloading = false;
    file.file.is_downloaded = true;
    file.file.local_path = "/tmp/avatar-downloaded.png";
    fake->emit(file);
    model.pump();
    assert(model.account().contacts.front().photo == file.file);
    assert(model.account().profile.user.photo == file.file);
    assert(model.model().messages().front().sender_photo == file.file);

    FileInfo replacement;
    replacement.id = 402;
    AccountUpdate changed;
    changed.kind = AccountUpdateKind::ContactChanged;
    changed.contact = model.account().contacts.front();
    changed.contact.photo = replacement;
    pump_account(model, *fake, changed);
    profile.profile.user.photo = replacement;
    pump_account(model, *fake, profile);
    message.message.sender_photo = replacement;
    message.message.sender_is_chat = true;
    fake->emit(message);
    model.pump();
    assert(model.model().messages().front().sender_is_chat);
    assert(model.account().contacts.front().photo.id == 402);
    assert(model.account().profile.user.photo.id == 402);
    assert(model.model().messages().front().sender_photo.id == 402);
    fake->emit(file);
    model.pump();
    assert(model.account().contacts.front().photo.id == 402);
    assert(model.account().profile.user.photo.id == 402);
    assert(model.model().messages().front().sender_photo.id == 402);

    changed.contact.photo = {};
    pump_account(model, *fake, changed);
    profile.profile.user.photo = {};
    pump_account(model, *fake, profile);
    message.message.sender_photo = {};
    fake->emit(message);
    model.pump();
    file.file = replacement;
    file.file.local_path = "/tmp/replaced-avatar.png";
    file.file.is_downloaded = true;
    fake->emit(file);
    model.pump();
    assert(model.account().contacts.front().photo.id == 0);
    assert(model.account().profile.user.photo.id == 0);
    assert(model.model().messages().front().sender_photo.id == 0);

    message.message.sender_is_chat = false;
    revision = model.revision();
    fake->emit(message);
    model.pump();
    assert(model.revision() > revision);
    assert(!model.model().messages().front().sender_is_chat);

    file.file = initial;
    fake->emit(file);
    model.pump();
    assert(!model.model().current_chat()->photo.is_downloaded);
    assert(model.model().current_chat()->photo.local_path.empty());
    assert(model.log_out());
    fake->emit(file);
    model.pump();
    assert(model.account().contacts.empty());
    assert(model.account().profile.user.photo.id == 0);
}

void test_ui_preferences_persist_and_handle_invalid_files_and_save_failure()
{
    using namespace telegram;
    std::string directory_template = "/tmp/telegram-preferences-test-XXXXXX";
    char* directory = ::mkdtemp(directory_template.data());
    assert(directory);
    const std::filesystem::path root(directory);
    ClientConfig config;
    config.preferences_path = (root / "nested" / "preferences.json").string();
    UiPreferences chosen;
    chosen.message_font_size = 30;
    chosen.auto_download_photos = false;
    chosen.contacts_sort_online = true;
    {
        TelegramViewModel model(nullptr, config);
        assert(model.preferences().message_font_size == 22);
        assert(model.set_preferences(chosen));
    }
    {
        TelegramViewModel model(nullptr, config);
        assert(model.preferences().message_font_size == 30);
        assert(!model.preferences().auto_download_photos);
        assert(model.preferences().contacts_sort_online);
        auto invalid = chosen;
        invalid.message_font_size = 25;
        assert(!model.set_preferences(invalid));
        assert(model.preferences().message_font_size == 30);
    }
    {
        std::ifstream file(config.preferences_path);
        const std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        const auto json = support::json::parse(content);
        assert(json.as_object().size() == 3);
        assert(json.contains("message_font_size"));
        assert(json.contains("auto_download_photos"));
        assert(json.contains("contacts_sort_online"));
    }
    for (const auto& bad : {std::string("{\"message_font_size\":25}"),
                            std::string("{\"message_font_size\":30,\"auto_download_photos\":\"yes\"}"),
                            std::string("["), std::string(4097, ' ')}) {
        std::ofstream file(config.preferences_path, std::ios::trunc);
        file << bad;
        file.close();
        TelegramViewModel model(nullptr, config);
        assert(model.preferences().message_font_size == 22);
        assert(model.preferences().auto_download_photos);
    }
    config.preferences_path = (root / "blocked").string();
    std::filesystem::create_directory(config.preferences_path);
    TelegramViewModel failed(nullptr, config);
    assert(!failed.set_preferences(chosen));
    assert(failed.preferences().message_font_size == 22);
    assert(!failed.account_error().empty());
    assert(failed.error_text().empty());
    for (const auto& item : std::filesystem::directory_iterator(root))
        assert(item.path().filename().string().find(".tmp.") == std::string::npos);
    std::filesystem::remove_all(root);
}

}  // namespace

int main()
{
    test_chat_search_matches_titles_previews_and_utf8();
    test_chat_search_preserves_utf8_at_input_limits();
    test_drafts_remain_with_their_chat_and_survive_send_attempts();
    test_drafts_are_pruned_with_the_bounded_chat_list();
    test_local_chat_state_clears_when_authentication_resets();
    test_auth_dispatch_and_status();
    test_chat_messages_files_and_commands();
    test_pump_limit();
    test_history_moves_the_bounded_window_toward_older_messages();
    test_start_failure_is_stopped();
    test_account_validation_rejection_and_async_errors();
    test_account_request_bounds_and_auth_reset_ignore_stale_data();
    test_contact_search_sorting_and_account_data_bounds();
    test_contact_conversation_remains_outside_main_list_until_listed();
    test_zero_id_is_valid_only_for_the_current_session();
    test_avatar_file_updates_replacement_and_removal();
    test_ui_preferences_persist_and_handle_invalid_files_and_save_failure();
    return 0;
}
