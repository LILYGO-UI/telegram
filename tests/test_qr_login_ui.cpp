#include "pages/telegram/telegram_view.hpp"

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <fstream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

struct TelegramViewVoiceIdentityTestAccess {
    static lv_obj_t* add_focusable_input(TelegramView& view)
    {
        auto* input = lv_textarea_create(view.body_);
        lv_obj_add_event_cb(input, TelegramView::input_focused_event, LV_EVENT_FOCUSED, &view);
        return input;
    }

    static void count_focus_event(lv_event_t* event)
    {
        auto* count = static_cast<std::size_t*>(lv_event_get_user_data(event));
        ++*count;
    }

    static void rebuild(TelegramView& view)
    {
        view.rebuild();
    }

    static lv_obj_t* keyboard_input(const TelegramView& view)
    {
        return lv_keyboard_get_textarea(view.keyboard_);
    }
};

namespace {

std::vector<lv_color32_t> framebuffer;

class QrLoginClient final : public telegram::TelegramClient {
public:
    bool start(const telegram::ClientConfig&, EventCallback callback) override
    {
        callback_ = std::move(callback);
        emit_auth(telegram::AuthState::WaitingForPhoneNumber);
        return true;
    }

    void stop() noexcept override { callback_ = {}; }
    void submit_phone_number(std::string value) override { phone = std::move(value); }
    void request_qr_login() override { ++qr_requests; }
    void restart_phone_login() override { ++phone_restarts; }
    void submit_email_address(std::string) override {}
    void submit_email_code(std::string) override {}
    void submit_code(std::string) override {}
    void submit_password(std::string) override {}
    void log_out() override {}
    void load_chats(std::int32_t) override {}
    void open_chat(std::int64_t) override {}
    void close_chat(std::int64_t) override {}
    void load_history(std::int64_t, std::int64_t, std::int32_t) override {}
    void view_messages(std::int64_t, const std::vector<std::int64_t>&) override {}
    void open_message_content(std::int64_t, std::int64_t) override {}
    void send_text(std::int64_t, std::string) override {}
    void download_file(std::int32_t, std::int32_t) override {}

    void emit_auth(telegram::AuthState state, std::string link = {})
    {
        telegram::ClientEvent event;
        event.kind       = telegram::ClientEventKind::AuthStateChanged;
        event.auth_state = state;
        event.text       = std::move(link);
        callback_(std::move(event));
    }

    std::size_t qr_requests = 0;
    std::size_t phone_restarts = 0;
    std::string phone;

private:
    EventCallback callback_;
};

void flush_display(lv_display_t* display, const lv_area_t* area, std::uint8_t* pixels)
{
    const auto width = lv_display_get_horizontal_resolution(display);
    const auto row_width = area->x2 - area->x1 + 1;
    const auto* source = reinterpret_cast<const lv_color32_t*>(pixels);
    for (int y = area->y1; y <= area->y2; ++y)
        std::copy_n(source + (y - area->y1) * row_width, row_width, framebuffer.begin() + y * width + area->x1);
    lv_display_flush_ready(display);
}

void capture(lv_display_t* display, const std::string& prefix, const char* suffix)
{
    if (prefix.empty()) return;
    lv_refr_now(display);
    std::ofstream output(prefix + suffix, std::ios::binary);
    output << "P6\n" << lv_display_get_horizontal_resolution(display) << ' '
           << lv_display_get_vertical_resolution(display) << "\n255\n";
    for (const auto& pixel : framebuffer) {
        output.put(static_cast<char>(pixel.red));
        output.put(static_cast<char>(pixel.green));
        output.put(static_cast<char>(pixel.blue));
    }
    assert(output.good());
}

void advance_ui()
{
    for (int tick = 0; tick < 3; ++tick) {
        lv_tick_inc(60);
        lv_timer_handler();
    }
}

void assert_login_focus_frame(lv_obj_t* button, const std::string& prefix = {}, const char* suffix = "")
{
    assert(button);
    auto* display = lv_display_get_default();
    lv_obj_scroll_to_view_recursive(button, LV_ANIM_OFF);
    advance_ui();
    lv_obj_update_layout(lv_screen_active());
    lv_area_t before;
    lv_obj_get_coords(button, &before);
    lv_obj_add_state(button, static_cast<lv_state_t>(LV_STATE_FOCUSED | LV_STATE_FOCUS_KEY));
    advance_ui();
    lv_refr_now(display);
    lv_area_t area;
    lv_obj_get_coords(button, &area);
    assert(area.x1 == before.x1 && area.x2 == before.x2 && area.y1 == before.y1 && area.y2 == before.y2);
    // Launcher retains the ordinary LVGL button theme outline. Check that
    // reference and actual exterior pixels, so clipping cannot pass unnoticed.
    auto* reference = lv_button_create(lv_screen_active());
    lv_obj_add_flag(reference, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_state(reference, LV_STATE_FOCUS_KEY);
    const auto width = lv_obj_get_style_outline_width(reference, LV_PART_MAIN);
    const auto pad = lv_obj_get_style_outline_pad(reference, LV_PART_MAIN);
    assert(width > 0 && pad > 0);
    assert(lv_obj_get_style_border_width(button, LV_PART_MAIN) == 0);
    assert(lv_obj_get_style_outline_width(button, LV_PART_MAIN) == width);
    assert(lv_obj_get_style_outline_pad(button, LV_PART_MAIN) == pad);
    assert(lv_obj_get_style_outline_opa(button, LV_PART_MAIN) ==
           lv_obj_get_style_outline_opa(reference, LV_PART_MAIN));
    assert(lv_color_eq(lv_obj_get_style_outline_color(button, LV_PART_MAIN),
                       lv_obj_get_style_outline_color(reference, LV_PART_MAIN)));
    lv_obj_delete(reference);
    const auto screen_width = lv_display_get_horizontal_resolution(display);
    const auto screen_height = lv_display_get_vertical_resolution(display);
    const auto x = (area.x1 + area.x2) / 2;
    const auto y = (area.y1 + area.y2) / 2;
    const lv_point_t samples[] = {{area.x1 - pad - 1, y}, {area.x2 + pad + 1, y},
                                  {x, area.y1 - pad - 1}, {x, area.y2 + pad + 1}};
    std::vector<lv_color32_t> focused;
    for (const auto& point : samples) {
        assert(point.x >= 0 && point.x < screen_width && point.y >= 0 && point.y < screen_height);
        focused.push_back(framebuffer[point.y * screen_width + point.x]);
    }
    capture(display, prefix, suffix);
    lv_obj_remove_state(button, static_cast<lv_state_t>(LV_STATE_FOCUSED | LV_STATE_FOCUS_KEY));
    advance_ui();
    lv_refr_now(display);
    for (std::size_t i = 0; i < focused.size(); ++i) {
        const auto& plain = framebuffer[samples[i].y * screen_width + samples[i].x];
        assert(focused[i].red != plain.red || focused[i].green != plain.green || focused[i].blue != plain.blue);
    }
}

}  // namespace

int main(int argc, char** argv)
{
    constexpr std::int32_t width  = 568;
    constexpr std::int32_t height = 1232;
    std::vector<lv_color32_t> draw_buffer(static_cast<std::size_t>(width) * 80U);
    framebuffer.resize(static_cast<std::size_t>(width) * height);
    const std::string capture_prefix = argc > 1 ? argv[1] : "";

    lv_init();
    auto* display = lv_display_create(width, height);
    assert(display);
    lv_display_set_buffers(display, draw_buffer.data(), nullptr,
                           static_cast<std::uint32_t>(draw_buffer.size() * sizeof(lv_color32_t)),
                           LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(display, flush_display);
    lv_display_set_default(display);

    {
        auto client = std::make_unique<QrLoginClient>();
        auto* fake  = client.get();
        telegram::TelegramViewModel view_model(std::move(client), {});
        assert(view_model.start());
        assert(view_model.pump() == 1);

        TelegramView view(view_model);
        auto* root = lv_screen_active();
        view.create(root, "Telegram");
        assert(lv_obj_find_by_name(root, "telegram_auth_qr_login"));
        assert_login_focus_frame(lv_obj_find_by_name(root, "telegram_auth_submit"), capture_prefix, ".submit-focus.ppm");
        assert_login_focus_frame(lv_obj_find_by_name(root, "telegram_auth_qr_login"), capture_prefix, ".qr-button-focus.ppm");

        // Deleting the focused textarea can make LVGL focus another textarea
        // while rebuild() is cleaning the body; neither may be left attached.
        auto* auth_input = lv_obj_find_by_name(root, "telegram_auth_input");
        auto* next_input = TelegramViewVoiceIdentityTestAccess::add_focusable_input(view);
        std::size_t next_input_focus_events = 0;
        lv_obj_add_event_cb(next_input, TelegramViewVoiceIdentityTestAccess::count_focus_event,
                            LV_EVENT_FOCUSED, &next_input_focus_events);
        auto* focus_group = lv_group_create();
        assert(focus_group);
        lv_group_add_obj(focus_group, auth_input);
        lv_group_add_obj(focus_group, next_input);
        lv_group_focus_obj(auth_input);
        assert(TelegramViewVoiceIdentityTestAccess::keyboard_input(view) == auth_input);
        TelegramViewVoiceIdentityTestAccess::rebuild(view);
        auto* rebuilt_auth_input = lv_obj_find_by_name(root, "telegram_auth_input");
        assert(next_input_focus_events > 0);
        assert(rebuilt_auth_input);
        assert(TelegramViewVoiceIdentityTestAccess::keyboard_input(view) == rebuilt_auth_input);
        TelegramViewVoiceIdentityTestAccess::rebuild(view);
        assert(TelegramViewVoiceIdentityTestAccess::keyboard_input(view) == rebuilt_auth_input);
        lv_group_delete(focus_group);

        lv_textarea_set_text(lv_obj_find_by_name(root, "telegram_auth_input"), "+123456789");
        capture(display, capture_prefix, ".phone.ppm");

        lv_obj_send_event(lv_obj_find_by_name(root, "telegram_auth_qr_login"), LV_EVENT_CLICKED, nullptr);
        assert(fake->qr_requests == 1);
        assert(view_model.qr_login_requested());
        advance_ui();
        assert(lv_obj_find_by_name(root, "telegram_auth_phone_login"));
        assert(!lv_obj_find_by_name(root, "telegram_auth_input"));
        assert_login_focus_frame(lv_obj_find_by_name(root, "telegram_auth_phone_login"), capture_prefix, ".phone-button-focus.ppm");

        // Switching back while the QR request is in flight keeps the phone draft.
        lv_obj_send_event(lv_obj_find_by_name(root, "telegram_auth_phone_login"), LV_EVENT_CLICKED, nullptr);
        advance_ui();
        assert(fake->phone_restarts == 1);
        assert(lv_obj_find_by_name(root, "telegram_auth_qr_login"));
        assert(lv_obj_has_state(lv_obj_find_by_name(root, "telegram_auth_submit"), LV_STATE_DISABLED));
        assert(std::string(lv_textarea_get_text(lv_obj_find_by_name(root, "telegram_auth_input"))) == "+123456789");

        // A late QR result must not restore the QR page after selecting phone.
        fake->emit_auth(telegram::AuthState::WaitingForOtherDeviceConfirmation, "tg://login?token=old");
        assert(view_model.pump() == 1);
        advance_ui();
        assert(lv_obj_find_by_name(root, "telegram_auth_input"));
        assert(!lv_obj_find_by_name(root, "telegram_auth_qr_code"));
        fake->emit_auth(telegram::AuthState::WaitingForPhoneNumber);
        assert(view_model.pump() == 1);
        advance_ui();
        assert(!lv_obj_has_state(lv_obj_find_by_name(root, "telegram_auth_submit"), LV_STATE_DISABLED));

        lv_obj_send_event(lv_obj_find_by_name(root, "telegram_auth_qr_login"), LV_EVENT_CLICKED, nullptr);
        advance_ui();
        assert(fake->qr_requests == 2);

        fake->emit_auth(telegram::AuthState::WaitingForOtherDeviceConfirmation, "tg://login?token=first");
        assert(view_model.pump() == 1);
        advance_ui();

        auto* qr = lv_obj_find_by_name(root, "telegram_auth_qr_code");
        assert(qr);
        assert(lv_obj_get_width(qr) == 256);
        assert(lv_obj_get_height(qr) == 256);
        assert(view_model.status_text().find("tg://") == std::string::npos);
        capture(display, capture_prefix, ".qr.ppm");

        framebuffer.resize(1232U * 568U);
        lv_display_set_resolution(display, 1232, 568);
        advance_ui();
        capture(display, capture_prefix, ".qr-landscape.ppm");
        auto* phone_button = lv_obj_find_by_name(root, "telegram_auth_phone_login");
        assert(phone_button);
        lv_obj_update_layout(root);
        lv_area_t button_area;
        lv_obj_get_coords(phone_button, &button_area);
        assert(button_area.y1 >= 0 && button_area.y2 < 568);
        assert_login_focus_frame(phone_button, capture_prefix, ".landscape-phone-focus.ppm");
        framebuffer.resize(static_cast<std::size_t>(width) * height);
        lv_display_set_resolution(display, width, height);
        advance_ui();

        fake->emit_auth(telegram::AuthState::WaitingForOtherDeviceConfirmation, "tg://login?token=refreshed");
        assert(view_model.pump() == 1);
        advance_ui();
        assert(lv_obj_find_by_name(root, "telegram_auth_qr_code"));
        assert(view_model.qr_login_link() == "tg://login?token=refreshed");

        lv_obj_send_event(lv_obj_find_by_name(root, "telegram_auth_phone_login"), LV_EVENT_CLICKED, nullptr);
        advance_ui();
        assert(fake->phone_restarts == 2);
        assert(std::string(lv_textarea_get_text(lv_obj_find_by_name(root, "telegram_auth_input"))) == "+123456789");
        fake->emit_auth(telegram::AuthState::WaitingForPhoneNumber);
        assert(view_model.pump() == 1);
        advance_ui();
        lv_obj_send_event(lv_obj_find_by_name(root, "telegram_auth_submit"), LV_EVENT_CLICKED, nullptr);
        assert(fake->phone == "+123456789");
    }

    lv_deinit();
    return 0;
}
