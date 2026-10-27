#include "services/telegram/telegram_client.hpp"
#include "tdjson_auth_fixture.hpp"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <initializer_list>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace {

using telegram::ClientEvent;
using telegram::ClientEventKind;
constexpr const char* processed =
    R"({"@type":"updateConnectionState","state":{"@type":"connectionStateReady"}})";
constexpr const char* not_found = R"({"@type":"error","code":404,"message":"Not Found"})";

class EventLog {
public:
    void append(ClientEvent event)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        events_.push_back(std::move(event));
        changed_.notify_all();
    }

    void wait(ClientEventKind kind)
    {
        std::unique_lock<std::mutex> lock(mutex_);
        const bool found = changed_.wait_for(lock, std::chrono::seconds(3), [&] {
            return std::any_of(events_.begin(), events_.end(), [&](const auto& event) { return event.kind == kind; });
        });
        assert(found);
    }

    std::vector<ClientEvent> of_kind(ClientEventKind kind) const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<ClientEvent> found;
        for (const auto& event : events_) if (event.kind == kind) found.push_back(event);
        return found;
    }

private:
    mutable std::mutex mutex_;
    std::condition_variable changed_;
    std::vector<ClientEvent> events_;
};

void emit(std::initializer_list<std::string> updates)
{
    std::vector<const char*> payloads;
    for (const auto& update : updates) payloads.push_back(update.c_str());
    telegram_auth_fixture_emit_updates(payloads.data(), static_cast<int>(payloads.size()));
}

std::string message(std::int64_t id, const std::string& text)
{
    return R"({"@type":"message","chat_id":42,"id":)" + std::to_string(id) +
        R"(,"content":{"@type":"messageText","text":{"@type":"formattedText","text":")" + text + R"(","entities":[]}}})";
}

std::string content_update(const std::string& type = "updateMessageContent")
{
    return R"({"@type":")" + type + R"(","chat_id":42,"message_id":101})";
}

struct Scenario {
    Scenario()
    {
        telegram_auth_fixture_reset();
        client = telegram::make_tdlib_client();
        telegram::ClientConfig config;
        config.api_id = 1;
        config.api_hash = "fixture-does-not-connect-to-Telegram";
        assert(client->start(config, [&](auto event) { events.append(std::move(event)); }));
        events.wait(ClientEventKind::AuthStateChanged);
    }

    ~Scenario() { client->stop(); }

    void seed()
    {
        emit({R"({"@type":"updateNewMessage","message":)" + message(101, "original") + "}"});
        events.wait(ClientEventKind::MessageUpserted);
    }

    void response(const char* type, const std::string& payload)
    {
        // The following update proves the preceding response has been handled.
        telegram_auth_fixture_set_response(type, payload.c_str(), processed);
    }

    void finish_response() { events.wait(ClientEventKind::ConnectionStateChanged); }

    EventLog events;
    std::unique_ptr<telegram::TelegramClient> client;
};

void test_missing_message_is_removed(const char* update_type)
{
    Scenario scenario;
    scenario.seed();
    scenario.response("getMessage", not_found);
    emit({content_update(update_type)});
    scenario.finish_response();
    const auto removed = scenario.events.of_kind(ClientEventKind::MessageRemoved);
    assert(removed.size() == 1);
    assert(removed.front().chat_id == 42 && removed.front().message_id == 101);
    assert(scenario.events.of_kind(ClientEventKind::Error).empty());
    assert(telegram_auth_fixture_request_count("getMessage") == 1);
}

void test_uncached_updates_do_not_fetch_messages()
{
    Scenario scenario;
    scenario.response("getMessage", not_found);
    emit({content_update(), content_update("updateMessageEphemeralContent"), processed});
    scenario.finish_response();
    assert(telegram_auth_fixture_request_count("getMessage") == 0);
    assert(scenario.events.of_kind(ClientEventKind::Error).empty());
}

void test_other_refresh_errors_are_reported()
{
    Scenario scenario;
    scenario.seed();
    scenario.response("getMessage", R"({"@type":"error","code":500,"message":"Server error"})");
    emit({content_update()});
    scenario.finish_response();
    const auto errors = scenario.events.of_kind(ClientEventKind::Error);
    assert(errors.size() == 1);
    assert(errors.front().chat_id == 42 && errors.front().message_id == 101);
    assert(errors.front().text == "get message failed (code 500): Server error");
    assert(scenario.events.of_kind(ClientEventKind::MessageRemoved).empty());
}

void test_user_request_not_found_is_reported(bool history)
{
    Scenario scenario;
    scenario.response(history ? "getChatHistory" : "sendMessage", not_found);
    if (history) scenario.client->load_history(42, 0, 20);
    else scenario.client->send_text(42, "hello");
    scenario.finish_response();
    const auto errors = scenario.events.of_kind(ClientEventKind::Error);
    assert(errors.size() == 1 && errors.front().chat_id == 42);
    assert(errors.front().text == (history ? "load chat history failed (code 404): Not Found"
                                          : "send text message failed (code 404): Not Found"));
}

void test_successful_refresh_updates_message()
{
    Scenario scenario;
    scenario.seed();
    scenario.response("getMessage", message(101, "edited"));
    emit({content_update()});
    scenario.finish_response();
    const auto messages = scenario.events.of_kind(ClientEventKind::MessageUpserted);
    assert(messages.size() == 2 && messages.back().message.text == "edited");
    assert(scenario.events.of_kind(ClientEventKind::Error).empty());
}

void test_late_refresh_cannot_restore_removed_message(bool replaced)
{
    Scenario scenario;
    scenario.seed();
    scenario.response("getMessage", message(101, "stale refresh"));
    const auto removal = replaced
        ? R"({"@type":"updateMessageSendSucceeded","old_message_id":101,"message":)" + message(102, "sent") + "}"
        : std::string(R"({"@type":"updateDeleteMessages","chat_id":42,"message_ids":[101]})");
    // Queue both atomically: deletion/replacement is handled before the reply.
    emit({content_update(), removal});
    scenario.finish_response();
    const auto messages = scenario.events.of_kind(ClientEventKind::MessageUpserted);
    assert(messages.size() == (replaced ? 2 : 1));
    if (replaced) assert(messages.back().message.id == 102);
    const auto removed = scenario.events.of_kind(ClientEventKind::MessageRemoved);
    assert(removed.size() == 1 && removed.front().message_id == 101);
    assert(scenario.events.of_kind(ClientEventKind::Error).empty());
}

}  // namespace

int main(int argc, char** argv)
{
    assert(argc == 2);
    assert(setenv("LILYGO_UI_TDJSON_LIBRARY", argv[1], 1) == 0);
    test_missing_message_is_removed("updateMessageContent");
    test_missing_message_is_removed("updateMessageEphemeralContent");
    test_uncached_updates_do_not_fetch_messages();
    test_other_refresh_errors_are_reported();
    test_user_request_not_found_is_reported(false);
    test_user_request_not_found_is_reported(true);
    test_successful_refresh_updates_message();
    test_late_refresh_cannot_restore_removed_message(false);
    test_late_refresh_cannot_restore_removed_message(true);
}
