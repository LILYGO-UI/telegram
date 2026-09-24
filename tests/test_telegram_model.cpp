#include "domain/telegram_model.hpp"
#include "services/bounded_queue.hpp"

#include <cassert>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

telegram::Chat make_chat(std::int64_t id, std::int64_t order)
{
    telegram::Chat chat;
    chat.id    = id;
    chat.order = order;
    chat.title = "Chat " + std::to_string(id);
    return chat;
}

telegram::Message make_message(std::int64_t id, std::int64_t chat_id, std::int64_t date)
{
    telegram::Message message;
    message.id      = id;
    message.chat_id = chat_id;
    message.date    = date;
    message.kind    = telegram::MessageKind::Text;
    message.text    = "Message " + std::to_string(id);
    return message;
}

void test_states_and_events()
{
    telegram::TelegramModel model;
    assert(model.auth_state() == telegram::AuthState::Unknown);
    assert(model.connection_state() == telegram::ConnectionState::Unknown);
    assert(!model.set_auth_state(telegram::AuthState::Unknown));
    assert(model.set_auth_state(telegram::AuthState::WaitingForPhoneNumber));
    assert(!model.set_auth_state(telegram::AuthState::WaitingForPhoneNumber));
    assert(model.set_connection_state(telegram::ConnectionState::Connecting));

    const auto events = model.take_events();
    assert(events.size() == 2);
    assert(events[0].kind == telegram::ModelEventKind::AuthStateChanged);
    assert(events[1].kind == telegram::ModelEventKind::ConnectionStateChanged);
    assert(model.take_events().empty());
}

void test_chat_update_sort_limit_and_selection()
{
    telegram::TelegramModel model;
    assert(!model.upsert_chat(make_chat(0, 1)));
    for (std::int64_t id = 1; id <= 102; ++id) assert(model.upsert_chat(make_chat(id, id)));

    assert(model.chats().size() == telegram::TelegramModel::kMaxChats);
    assert(model.chats().front().id == 102);
    assert(model.chats().back().id == 3);
    for (std::size_t index = 1; index < model.chats().size(); ++index) {
        assert(model.chats()[index - 1].order >= model.chats()[index].order);
    }
    assert(!model.find_chat(1));
    assert(model.find_chat(42));
    (void)model.take_events();
    assert(!model.upsert_chat(make_chat(1000, -1)));
    assert(!model.find_chat(1000));
    assert(model.take_events().empty());
    assert(model.select_chat(42));
    assert(model.current_chat_id() == 42);
    assert(model.current_chat() && model.current_chat()->id == 42);
    assert(!model.select_chat(42));
    assert(!model.select_chat(999));

    auto updated  = *model.find_chat(42);
    updated.title = "Updated";
    updated.order = 1000;
    assert(model.upsert_chat(updated));
    assert(model.chats().front().id == 42);
    assert(model.find_chat(42)->title == "Updated");
    assert(!model.upsert_chat(updated));

    updated.is_private = true;
    assert(model.upsert_chat(updated));
    assert(model.current_chat()->is_private);
    assert(!model.upsert_chat(updated));

    assert(model.remove_chat(42));
    assert(model.current_chat_id() == 0);
    assert(!model.current_chat());
    assert(!model.remove_chat(42));
    assert(model.clear_chats());
    assert(model.chats().empty());
    assert(!model.clear_chats());
}

void test_messages_update_sort_limit_and_media()
{
    telegram::TelegramModel model;
    assert(model.upsert_chat(make_chat(7, 100)));
    assert(model.upsert_chat(make_chat(8, 90)));
    assert(model.select_chat(7));

    assert(!model.upsert_message(make_message(1, 8, 1)));
    assert(!model.upsert_message(make_message(0, 7, 1)));
    for (std::int64_t id = 82; id >= 3; --id) assert(model.upsert_message(make_message(id, 7, id)));
    assert(!model.upsert_message(make_message(2, 7, 2)));
    assert(!model.upsert_message(make_message(1, 7, 1)));

    assert(model.messages().size() == telegram::TelegramModel::kMaxCurrentMessages);
    assert(model.messages().front().id == 3);
    assert(model.messages().back().id == 82);
    for (std::size_t index = 1; index < model.messages().size(); ++index) {
        assert(model.messages()[index - 1].date <= model.messages()[index].date);
    }
    (void)model.take_events();
    assert(!model.upsert_message(make_message(1000, 7, -1)));
    assert(!model.find_message(1000));
    assert(model.take_events().empty());
    assert(model.upsert_message(make_message(2, 7, 2), telegram::TelegramModel::MessageRetention::Oldest));
    assert(model.messages().front().id == 2);
    assert(model.messages().back().id == 81);

    auto updated                 = *model.find_message(40);
    updated.kind                 = telegram::MessageKind::Photo;
    updated.text                 = "caption";
    updated.width                = 800;
    updated.height               = 600;
    updated.file.id              = 123;
    updated.file.size            = 4096;
    updated.file.downloaded_size = 4096;
    updated.file.local_path      = "/tmp/photo.jpg";
    updated.file.is_downloaded   = true;
    assert(model.upsert_message(updated));
    assert(model.messages().size() == telegram::TelegramModel::kMaxCurrentMessages);
    assert(model.find_message(40)->kind == telegram::MessageKind::Photo);
    assert(model.find_message(40)->file.is_downloaded);
    assert(!model.upsert_message(updated));

    auto voice             = make_message(83, 7, 83);
    voice.kind             = telegram::MessageKind::Voice;
    voice.duration_seconds = 12;
    assert(model.upsert_message(voice));
    assert(model.messages().back().kind == telegram::MessageKind::Voice);
    assert(model.messages().back().duration_seconds == 12);

    assert(model.remove_message(40));
    assert(!model.find_message(40));
    assert(!model.remove_message(40));
    assert(model.select_chat(8));
    assert(model.messages().empty());
    assert(model.current_chat_id() == 8);
    assert(!model.clear_messages());
}

void test_selected_chat_trim_and_clear()
{
    telegram::TelegramModel model;
    assert(model.upsert_chat(make_chat(1, 1)));
    assert(model.select_chat(1));
    assert(model.upsert_message(make_message(1, 1, 1)));
    for (std::int64_t id = 2; id <= 101; ++id) assert(model.upsert_chat(make_chat(id, id)));
    assert(model.current_chat_id() == 0);
    assert(model.messages().empty());

    assert(model.set_auth_state(telegram::AuthState::Ready));
    assert(model.set_connection_state(telegram::ConnectionState::Ready));
    model.clear();
    assert(model.auth_state() == telegram::AuthState::Unknown);
    assert(model.connection_state() == telegram::ConnectionState::Unknown);
    assert(model.chats().empty());
    assert(model.messages().empty());
    const auto events = model.take_events();
    assert(events.size() == 1);
    assert(events.front().kind == telegram::ModelEventKind::Cleared);
}

void test_bounded_queue()
{
    bool rejected_zero_capacity = false;
    try {
        telegram::BoundedQueue<int> invalid(0);
    } catch (const std::invalid_argument&) {
        rejected_zero_capacity = true;
    }
    assert(rejected_zero_capacity);

    telegram::BoundedQueue<int> queue(2);
    assert(queue.capacity() == 2);
    assert(queue.try_push(1));
    assert(queue.try_push(2));
    assert(!queue.try_push(3));
    assert(queue.size() == 2);

    int value = 0;
    assert(queue.try_pop(value) && value == 1);
    assert(queue.push(3));
    assert(queue.clear() == 2);
    assert(queue.empty());

    bool pop_result = true;
    std::thread consumer([&queue, &value, &pop_result] { pop_result = queue.pop(value); });
    queue.close();
    consumer.join();
    assert(!pop_result);
    assert(queue.closed());
    assert(!queue.push(4));
    assert(!queue.try_push(4));
    assert(!queue.try_pop(value));
}

}  // namespace

int main()
{
    test_states_and_events();
    test_chat_update_sort_limit_and_selection();
    test_messages_update_sort_limit_and_media();
    test_selected_chat_trim_and_clear();
    test_bounded_queue();
    return 0;
}
