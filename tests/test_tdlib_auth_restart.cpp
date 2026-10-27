#include "services/telegram/telegram_client.hpp"
#include "tdjson_auth_fixture.hpp"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace {

class EventLog {
public:
    void append(telegram::ClientEvent event)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        events_.push_back(std::move(event));
        changed_.notify_all();
    }

    void wait_auth(telegram::AuthState state, std::size_t count = 1)
    {
        std::unique_lock<std::mutex> lock(mutex_);
        const bool found = changed_.wait_for(lock, std::chrono::seconds(3), [&] {
            return static_cast<std::size_t>(std::count_if(events_.begin(), events_.end(), [&](const auto& event) {
                return event.kind == telegram::ClientEventKind::AuthStateChanged && event.auth_state == state;
            })) >= count;
        });
        assert(found);
    }

    void wait_error()
    {
        std::unique_lock<std::mutex> lock(mutex_);
        const bool found = changed_.wait_for(lock, std::chrono::seconds(3), [&] {
            return std::any_of(events_.begin(), events_.end(), [](const auto& event) {
                return event.kind == telegram::ClientEventKind::Error;
            });
        });
        assert(found);
    }

    bool has_stale_client_link() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return std::any_of(events_.begin(), events_.end(), [](const auto& event) {
            return event.text.find("stale-client") != std::string::npos;
        });
    }

private:
    mutable std::mutex mutex_;
    std::condition_variable changed_;
    std::vector<telegram::ClientEvent> events_;
};

telegram::ClientConfig test_config()
{
    telegram::ClientConfig config;
    config.api_id = 1;
    config.api_hash = "fixture-does-not-connect-to-Telegram";
    return config;
}

void test_restart_recreates_client_and_accepts_phone_number()
{
    telegram_auth_fixture_reset();
    EventLog events;
    auto client = telegram::make_tdlib_client();
    assert(client->start(test_config(), [&](auto event) { events.append(std::move(event)); }));
    events.wait_auth(telegram::AuthState::WaitingForPhoneNumber);
    client->request_qr_login();
    events.wait_auth(telegram::AuthState::WaitingForOtherDeviceConfirmation);

    client->restart_phone_login();
    events.wait_auth(telegram::AuthState::WaitingForPhoneNumber, 2);
    assert(telegram_auth_fixture_created_clients() == 2);
    assert(telegram_auth_fixture_parameter_requests() == 2);
    assert(telegram_auth_fixture_logout_requests() == 1);
    assert(telegram_auth_fixture_duplicate_request_ids() == 0);
    assert(!events.has_stale_client_link());

    client->submit_phone_number("+12345");
    events.wait_auth(telegram::AuthState::WaitingForCode);
    assert(telegram_auth_fixture_phone_requests() == 1);
    client->stop();
}

void test_restart_probe_preserves_an_account_that_just_signed_in()
{
    telegram_auth_fixture_reset();
    EventLog events;
    auto client = telegram::make_tdlib_client();
    assert(client->start(test_config(), [&](auto event) { events.append(std::move(event)); }));
    events.wait_auth(telegram::AuthState::WaitingForPhoneNumber);
    client->request_qr_login();
    events.wait_auth(telegram::AuthState::WaitingForOtherDeviceConfirmation);

    telegram_auth_fixture_set_server_ready();
    client->restart_phone_login();
    events.wait_auth(telegram::AuthState::Ready);
    events.wait_error();
    assert(telegram_auth_fixture_created_clients() == 1);
    assert(telegram_auth_fixture_logout_requests() == 0);
    client->stop();
}

void test_logout_recreates_client_and_returns_to_phone_login()
{
    telegram_auth_fixture_reset();
    EventLog events;
    auto client = telegram::make_tdlib_client();
    assert(client->start(test_config(), [&](auto event) { events.append(std::move(event)); }));
    events.wait_auth(telegram::AuthState::WaitingForPhoneNumber);
    telegram_auth_fixture_authorize_current_client();
    events.wait_auth(telegram::AuthState::Ready);

    client->log_out();
    events.wait_auth(telegram::AuthState::WaitingForPhoneNumber, 2);
    assert(telegram_auth_fixture_created_clients() == 2);
    assert(telegram_auth_fixture_parameter_requests() == 2);
    assert(telegram_auth_fixture_logout_requests() == 1);
    assert(telegram_auth_fixture_phone_requests() == 0);
    assert(!events.has_stale_client_link());

    client->submit_phone_number("+12345");
    events.wait_auth(telegram::AuthState::WaitingForCode);
    assert(telegram_auth_fixture_phone_requests() == 1);
    client->stop();
}

}  // namespace

int main(int argc, char** argv)
{
    assert(argc == 2);
    assert(setenv("LILYGO_UI_TDJSON_LIBRARY", argv[1], 1) == 0);
    test_restart_recreates_client_and_accepts_phone_number();
    test_restart_probe_preserves_an_account_that_just_signed_in();
    test_logout_recreates_client_and_returns_to_phone_login();
    return 0;
}
