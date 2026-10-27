#include "pages/telegram/telegram_view.hpp"

#include <cm0/app.h>
#include <SDL.h>
#include <src/drivers/sdl/lv_sdl_private.h>
#include <src/drivers/sdl/lv_sdl_window.h>

#include <cassert>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <utility>

struct TelegramViewVoiceIdentityTestAccess {
    static void tick(TelegramView& view) { view.tick(); }
};

namespace {

// This client never opens TDLib or a network connection. Model events still
// travel through the real view-model queue and trigger normal view rebuilds.
class KeyboardClient final : public telegram::TelegramClient {
public:
    bool start(const telegram::ClientConfig&, EventCallback callback) override
    {
        callback_ = std::move(callback);
        telegram::ClientEvent event;
        event.kind = telegram::ClientEventKind::AuthStateChanged;
        event.auth_state = telegram::AuthState::WaitingForPhoneNumber;
        callback_(std::move(event));
        return true;
    }

    void stop() noexcept override { callback_ = {}; }
    void submit_phone_number(std::string value) override
    {
        phone = std::move(value);
        ++submissions;
    }
    void request_qr_login() override {}
    void restart_phone_login() override {}
    void submit_email_address(std::string) override {}
    void submit_email_code(std::string) override {}
    void submit_code(std::string) override {}
    void submit_password(std::string) override {}
    void log_out() override {}
    void load_chats(std::int32_t) override
    {
        for (const auto& chat : {std::pair<std::int64_t, const char*>{1001, "Alpha"}, {1002, "Beta"}}) {
            telegram::ClientEvent event;
            event.kind = telegram::ClientEventKind::ChatUpserted;
            event.chat.id = chat.first;
            event.chat.order = 2000 - chat.first;
            event.chat.title = chat.second;
            event.chat.is_private = true;
            callback_(std::move(event));
        }
    }
    void open_chat(std::int64_t) override {}
    void close_chat(std::int64_t) override {}
    void load_history(std::int64_t, std::int64_t, std::int32_t) override {}
    void view_messages(std::int64_t, const std::vector<std::int64_t>&) override {}
    void open_message_content(std::int64_t, std::int64_t) override {}
    void send_text(std::int64_t chat_id, std::string value) override
    {
        sent_chat = chat_id;
        sent_text = std::move(value);
        ++sends;
        telegram::ClientEvent event;
        event.kind = telegram::ClientEventKind::MessageUpserted;
        event.message.id = 1;
        event.message.chat_id = chat_id;
        event.message.kind = telegram::MessageKind::Text;
        event.message.text = sent_text;
        event.message.is_outgoing = true;
        callback_(std::move(event));
    }
    void download_file(std::int32_t, std::int32_t) override {}

    void update_status()
    {
        telegram::ClientEvent event;
        event.kind = telegram::ClientEventKind::Status;
        event.text = "Keyboard integration test update " + std::to_string(++status_updates_);
        callback_(std::move(event));
    }

    void move_beta_to_top()
    {
        telegram::ClientEvent event;
        event.kind = telegram::ClientEventKind::ChatUpserted;
        event.chat.id = 1002;
        event.chat.order = 1000;
        event.chat.title = "Beta";
        event.chat.is_private = true;
        callback_(std::move(event));
    }

    void become_ready()
    {
        telegram::ClientEvent event;
        event.kind = telegram::ClientEventKind::AuthStateChanged;
        event.auth_state = telegram::AuthState::Ready;
        callback_(std::move(event));
    }

    std::string phone;
    unsigned submissions = 0;
    std::int64_t sent_chat = 0;
    std::string sent_text;
    unsigned sends = 0;

private:
    EventCallback callback_;
    unsigned status_updates_ = 0;
};

struct TestState {
    cm0_app_context_t context{};
    KeyboardClient* client = nullptr;
    std::unique_ptr<telegram::TelegramViewModel> model;
    std::unique_ptr<TelegramView> view;
    lv_timer_t* timer = nullptr;
    std::uint32_t started = 0;
    unsigned step = 0;
    bool passed = false;
};

TestState state;

lv_obj_t* required(const char* name)
{
    auto* object = lv_obj_find_by_name(state.context.root, name);
    if (!object) std::fprintf(stderr, "Missing keyboard test control: %s\n", name);
    assert(object);
    return object;
}

bool contains_label(lv_obj_t* object, const char* value)
{
    if (lv_obj_check_type(object, &lv_label_class) && std::strcmp(lv_label_get_text(object), value) == 0)
        return true;
    for (std::uint32_t index = 0; index < lv_obj_get_child_count(object); ++index)
        if (contains_label(lv_obj_get_child(object, static_cast<std::int32_t>(index)), value)) return true;
    return false;
}

lv_obj_t* required_chat_row(const char* title)
{
    auto* list = required("telegram_chat_list");
    for (std::uint32_t index = 0; index < lv_obj_get_child_count(list); ++index) {
        auto* row = lv_obj_get_child(list, static_cast<std::int32_t>(index));
        if (contains_label(row, title)) return row;
    }
    std::fprintf(stderr, "Missing keyboard test chat: %s\n", title);
    assert(false);
    return nullptr;
}

void expect_focused(const char* name)
{
    auto* expected = required(name);
    auto* focused = lv_group_get_focused(lv_group_get_default());
    if (focused != expected)
        std::fprintf(stderr, "Keyboard test expected focus on %s, got %s (step %u)\n", name,
                     focused && lv_obj_get_name(focused) ? lv_obj_get_name(focused) : "<unnamed or none>",
                     state.step);
    assert(focused == expected);
}

void expect_login_outline(const char* name)
{
    auto* button = required(name);
    assert(lv_obj_has_state(button, LV_STATE_FOCUS_KEY));
    // Verify the real runtime preserves the exterior outline after navigation.
    assert(lv_obj_get_style_outline_width(button, LV_PART_MAIN) > 0);
    assert(lv_obj_get_style_outline_pad(button, LV_PART_MAIN) > 0);
    assert(lv_obj_get_style_outline_opa(button, LV_PART_MAIN) == LV_OPA_50);
    assert(lv_obj_get_style_border_width(button, LV_PART_MAIN) == 0);
}

void expect_text(const char* name, const char* value)
{
    expect_focused(name);
    assert(std::string(lv_textarea_get_text(required(name))) == value);
}

Uint32 window_id()
{
    auto* window = lv_sdl_window_get_window(lv_display_get_default());
    assert(window);
    return SDL_GetWindowID(window);
}

void key(SDL_Keycode code)
{
    SDL_Event event{};
    event.type = SDL_KEYDOWN;
    event.key.windowID = window_id();
    event.key.state = SDL_PRESSED;
    event.key.keysym.sym = code;
    assert(SDL_PushEvent(&event) == 1);
    event.type = SDL_KEYUP;
    event.key.state = SDL_RELEASED;
    assert(SDL_PushEvent(&event) == 1);
}

void text(const char* value)
{
    SDL_Event event{};
    event.type = SDL_TEXTINPUT;
    event.text.windowID = window_id();
    assert(std::strlen(value) < sizeof(event.text.text));
    std::strcpy(event.text.text, value);
    // SDL2-compat 2.32.70 rejects synthetic SDL_TEXTINPUT and passes a null
    // event to SDL3. Deliver the real SDL event to LVGL's SDL entry point;
    // navigation/editing keys still travel through SDL's event queue.
    lv_sdl_keyboard_handler(&event);
}

void drive_keyboard(lv_timer_t*)
{
    if (lv_tick_elaps(state.started) >= 8000) {
        auto* focused = lv_group_get_focused(lv_group_get_default());
        std::fprintf(stderr, "Keyboard integration timed out at step %u with focus on %s\n", state.step,
                     focused && lv_obj_get_name(focused) ? lv_obj_get_name(focused) : "<unnamed or none>");
    }
    assert(lv_tick_elaps(state.started) < 8000);
    switch (state.step) {
        case 0:
            if (!lv_obj_find_by_name(state.context.root, "telegram_auth_input")) return;
            // Reach the first editor from whichever control LVGL focused
            // while the asynchronous initial login page was created.
            if (lv_group_get_focused(lv_group_get_default()) != required("telegram_auth_input")) {
                key(SDLK_TAB);
                return;
            }
            key(SDLK_TAB);
            break;
        case 1:
            expect_focused("telegram_auth_submit");
            expect_login_outline("telegram_auth_submit");
            key(SDLK_DOWN);
            break;
        case 2:
            expect_focused("telegram_auth_qr_login");
            expect_login_outline("telegram_auth_qr_login");
            state.client->update_status();
            TelegramViewVoiceIdentityTestAccess::tick(*state.view);
            expect_login_outline("telegram_auth_qr_login");
            key(SDLK_UP);
            break;
        case 3:
            expect_focused("telegram_auth_submit");
            expect_login_outline("telegram_auth_submit");
            lv_group_focus_obj(required("telegram_auth_input"));
            text("+123456789\xe4\xb8\xad");
            break;
        case 4:
            expect_focused("telegram_auth_input");
            assert(std::string(lv_textarea_get_text(required("telegram_auth_input"))) ==
                   "+123456789\xe4\xb8\xad");
            key(SDLK_BACKSPACE);
            break;
        case 5:
            assert(std::string(lv_textarea_get_text(required("telegram_auth_input"))) == "+123456789");
            assert(lv_obj_has_flag(required("telegram_keyboard"), LV_OBJ_FLAG_HIDDEN));
            state.client->update_status();
            break;
        case 6:
            // Runtime hides the software keyboard for SDL as well as CM0.
            // A model update must preserve the editor and cursor independently
            // of the software keyboard's visibility.
            expect_focused("telegram_auth_input");
            text("0");
            break;
        case 7:
            assert(std::string(lv_textarea_get_text(required("telegram_auth_input"))) == "+1234567890");
            key(SDLK_RETURN);
            break;
        case 8:
            assert(state.client->phone == "+1234567890");
            assert(state.client->submissions == 1);
            state.client->become_ready();
            break;
        case 9:
            if (!lv_obj_find_by_name(state.context.root, "telegram_chat_search")) return;
            if (lv_group_get_focused(lv_group_get_default()) != required("telegram_chat_search")) {
                key(SDLK_TAB);
                return;
            }
            text("a");
            break;
        case 10:
            // Each search edit changes the view model and rebuilds the chat
            // list. Focus must survive every individual character.
            expect_text("telegram_chat_search", "a");
            assert(state.model->chat_search() == "a");
            text("l");
            break;
        case 11:
            expect_text("telegram_chat_search", "al");
            assert(state.model->chat_search() == "al");
            text("p");
            break;
        case 12:
            expect_text("telegram_chat_search", "alp");
            assert(state.model->chat_search() == "alp");
            key(SDLK_BACKSPACE);
            break;
        case 13:
            expect_text("telegram_chat_search", "al");
            assert(state.model->chat_search() == "al");
            key(SDLK_TAB);
            break;
        case 14:
            if (lv_group_get_focused(lv_group_get_default()) != required("telegram_chat_row")) {
                key(SDLK_TAB);
                return;
            }
            // Background updates must not reset Tab traversal to the first
            // control while the user works through the chat list.
            state.client->update_status();
            break;
        case 15:
            expect_focused("telegram_chat_row");
            key(SDLK_TAB);
            break;
        case 16:
            expect_focused("telegram_tab_contacts");
            state.client->update_status();
            break;
        case 17:
            expect_focused("telegram_tab_contacts");
            key(SDLK_TAB);
            break;
        case 18:
            expect_focused("telegram_tab_chats");
            state.client->update_status();
            break;
        case 19:
            expect_focused("telegram_tab_chats");
            key(SDLK_TAB);
            break;
        case 20:
            expect_focused("telegram_tab_settings");
            state.client->update_status();
            break;
        case 21:
            expect_focused("telegram_tab_settings");
            state.client->update_status();
            break;
        case 22:
            expect_focused("telegram_tab_settings");
            // A keyboard may still reference its last editor after Tab has
            // moved to a button. Its visibility must not steal group focus
            // when an update rebuilds the page. Tick immediately to exercise
            // that state before hardware housekeeping hides the keyboard.
            lv_group_focus_obj(required("telegram_chat_search"));
            lv_group_focus_obj(required("telegram_tab_settings"));
            assert(lv_keyboard_get_textarea(required("telegram_keyboard")) == required("telegram_chat_search"));
            lv_obj_remove_flag(required("telegram_keyboard"), LV_OBJ_FLAG_HIDDEN);
            state.client->update_status();
            TelegramViewVoiceIdentityTestAccess::tick(*state.view);
            expect_focused("telegram_tab_settings");
            break;
        case 23:
            expect_focused("telegram_tab_settings");
            lv_group_focus_obj(required("telegram_search_clear"));
            key(SDLK_RETURN);
            break;
        case 24:
            assert(state.model->chat_search().empty());
            // Both rows have the same LVGL object name. Preserve the chat's
            // identity even if a live update changes its position in the list.
            lv_group_focus_obj(required_chat_row("Beta"));
            state.client->move_beta_to_top();
            break;
        case 25:
            assert(lv_group_get_focused(lv_group_get_default()) == required_chat_row("Beta"));
            key(SDLK_TAB);
            break;
        case 26:
            assert(lv_group_get_focused(lv_group_get_default()) == required_chat_row("Alpha"));
            state.client->update_status();
            break;
        case 27:
            assert(lv_group_get_focused(lv_group_get_default()) == required_chat_row("Alpha"));
            key(SDLK_RETURN);
            break;
        case 28:
            assert(state.model->model().current_chat_id() == 1001);
            if (lv_group_get_focused(lv_group_get_default()) != required("telegram_composer")) {
                key(SDLK_TAB);
                return;
            }
            text("hello");
            break;
        case 29:
            expect_text("telegram_composer", "hello");
            key(SDLK_LEFT);
            break;
        case 30:
            expect_focused("telegram_composer");
            assert(lv_textarea_get_cursor_pos(required("telegram_composer")) == 4);
            assert(lv_obj_has_flag(required("telegram_keyboard"), LV_OBJ_FLAG_HIDDEN));
            state.client->update_status();
            break;
        case 31:
            expect_text("telegram_composer", "hello");
            assert(lv_textarea_get_cursor_pos(required("telegram_composer")) == 4);
            text("!");
            break;
        case 32:
            expect_text("telegram_composer", "hell!o");
            key(SDLK_RETURN);
            break;
        case 33:
            assert(state.client->sent_chat == 1001);
            assert(state.client->sent_text == "hell!o");
            assert(state.client->sends == 1);
            assert(std::string(lv_textarea_get_text(required("telegram_composer"))).empty());
            state.client->update_status();
            break;
        case 34:
            assert(state.client->submissions == 1);
            assert(state.client->sends == 1);
            assert(std::string(lv_textarea_get_text(required("telegram_composer"))).empty());
            state.passed = true;
            state.context.request_exit(state.context.user_data);
            break;
        default:
            assert(false);
    }
    ++state.step;
}

void open(const cm0_app_context_t* context)
{
    state.context = *context;
    auto client = std::make_unique<KeyboardClient>();
    state.client = client.get();
    state.model = std::make_unique<telegram::TelegramViewModel>(std::move(client), telegram::ClientConfig{});
    state.view = std::make_unique<TelegramView>(*state.model);
    state.view->create(context->root, "Keyboard integration test");
    assert(state.model->start());
    state.started = lv_tick_get();
    // SDL's event timer and the view's normal 50 ms timer run between steps.
    state.timer = lv_timer_create(drive_keyboard, 100, nullptr);
    assert(state.timer);
}

void close()
{
    lv_timer_delete(state.timer);
    state.view.reset();
    state.model.reset();
}

}  // namespace

int main()
{
    SDL_setenv("SDL_VIDEODRIVER", "dummy", 1);
    SDL_setenv("SDL_AUDIODRIVER", "dummy", 1);
    const cm0_app_descriptor_t descriptor{CM0_APP_API_VERSION, sizeof(cm0_app_descriptor_t),
                                         "cc.lilygo.ui.Telegram.KeyboardTest", open, close};
    char executable[] = "telegram-keyboard-tests";
    char width_option[] = "--width";
    char width[] = "568";
    char height_option[] = "--height";
    char height[] = "1232";
    char orientation_option[] = "--orientation";
    char orientation[] = "landscape";
    char* arguments[] = {executable, width_option, width, height_option, height, orientation_option, orientation};
    const int result = cm0_app_run(7, arguments, &descriptor);
    assert(result == 0);
    assert(state.passed);
    return result;
}
