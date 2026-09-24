#include "pages/telegram/telegram_view.hpp"

#include "app_identity.h"
#include "components/components.hpp"
#include "pages/telegram/lvgl_photo_decoder.hpp"

#include <cm0/typography.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string_view>
#include <utility>

namespace {

constexpr std::int32_t kWideBreakpoint      = 960;
constexpr std::size_t kMaxCachedImages      = 3;
constexpr std::size_t kMaxCachedAvatars     = 64;
constexpr std::uint32_t kAvatarDimension   = 120;
constexpr std::size_t kMaxImageCacheBytes   = 6U * 1024U * 1024U;
constexpr std::size_t kMaxEncodedPngBytes   = 8U * 1024U * 1024U;
constexpr std::uint32_t kMaxImageDimension  = 720;
constexpr std::size_t kMaxDecodedImageBytes = 4U * 1024U * 1024U;
constexpr std::uint32_t kMaxComposerLength  = 4096;
#if defined(TELEGRAM_HAS_LVGL_TJPGD)
constexpr std::size_t kMaxEncodedJpegBytes       = 8U * 1024U * 1024U;
constexpr std::uint32_t kMaxDirectJpegDimension  = 2048;
constexpr std::uint64_t kMaxDirectJpegPixelCount = 4U * 1024U * 1024U;
#endif

void set_font(lv_obj_t* object, std::uint32_t size)
{
    if (const auto* font = lilygo_ui_font_get(size)) lv_obj_set_style_text_font(object, font, 0);
}

void make_transparent(lv_obj_t* object)
{
    lv_obj_set_style_border_width(object, 0, 0);
    lv_obj_set_style_radius(object, 0, 0);
    lv_obj_set_style_bg_opa(object, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(object, 0, 0);
    lv_obj_set_style_pad_row(object, 0, 0);
    lv_obj_set_style_pad_column(object, 0, 0);
}

lv_obj_t* make_label(lv_obj_t* parent, const char* text, std::uint32_t size,
                     std::uint32_t color = components::color_text)
{
    auto* label = lv_label_create(parent);
    lv_label_set_text(label, text ? text : "");
    set_font(label, size);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    return label;
}

lv_obj_t* make_icon_button(lv_obj_t* parent, const char* name, const char* symbol, lv_event_cb_t callback,
                           void* context)
{
    auto* button = lv_button_create(parent);
    lv_obj_set_name_static(button, name);
    lv_obj_set_size(button, 48, 48);
    make_transparent(button);
    lv_obj_set_style_shadow_width(button, 0, 0);
    lv_obj_set_style_bg_color(button, lv_color_hex(components::color_selection), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(button, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_add_event_cb(button, callback, LV_EVENT_CLICKED, context);
    auto* icon = make_label(button, symbol, 22, components::color_action);
    lv_obj_center(icon);
    return button;
}

std::string date_text(std::int64_t timestamp, bool short_date = false)
{
    if (timestamp <= 0) return {};
    const auto raw = static_cast<std::time_t>(timestamp);
    const auto now = std::time(nullptr);
    std::tm date{}, today{};
    localtime_r(&raw, &date);
    localtime_r(&now, &today);
    if (date.tm_year == today.tm_year && date.tm_yday == today.tm_yday) return short_date ? std::string{} : "Today";
    char value[40]{};
    std::strftime(value, sizeof(value), short_date ? "%d.%m.%y" : "%B %d, %Y", &date);
    return value;
}

void set_hidden(lv_obj_t* object, bool hidden)
{
    if (!object) return;
    if (hidden) {
        lv_obj_add_flag(object, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_remove_flag(object, LV_OBJ_FLAG_HIDDEN);
    }
}

std::string first_character(const std::string& text)
{
    if (text.empty()) return "?";
    const auto first   = static_cast<unsigned char>(text.front());
    std::size_t length = 1;
    if ((first & 0xe0U) == 0xc0U) {
        length = 2;
    } else if ((first & 0xf0U) == 0xe0U) {
        length = 3;
    } else if ((first & 0xf8U) == 0xf0U) {
        length = 4;
    }
    return text.substr(0, std::min(length, text.size()));
}

std::string format_time(std::int64_t timestamp)
{
    if (timestamp <= 0) return {};
    const auto raw_time = static_cast<std::time_t>(timestamp);
    std::tm local{};
#if defined(_WIN32)
    localtime_s(&local, &raw_time);
#else
    localtime_r(&raw_time, &local);
#endif
    char value[16]{};
    std::strftime(value, sizeof(value), "%H:%M", &local);
    return value;
}

std::string duration_text(std::int32_t seconds)
{
    seconds = std::max(0, seconds);
    char value[16]{};
    std::snprintf(value, sizeof(value), "%d:%02d", seconds / 60, seconds % 60);
    return value;
}

std::string lowercase_extension(const std::string& path)
{
    const auto dot = path.find_last_of('.');
    if (dot == std::string::npos) return {};
    std::string extension = path.substr(dot);
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char character) { return static_cast<char>(std::tolower(character)); });
    return extension;
}

bool png_is_bounded(const std::string& path, std::string* error)
{
    std::error_code filesystem_error;
    const auto file_size = std::filesystem::file_size(path, filesystem_error);
    if (filesystem_error || file_size > kMaxEncodedPngBytes) {
        if (error) *error = "PNG file exceeds the encoded size limit";
        return false;
    }

    std::ifstream file(path, std::ios::binary);
    std::array<unsigned char, 24> header{};
    if (!file.read(reinterpret_cast<char*>(header.data()), static_cast<std::streamsize>(header.size()))) {
        if (error) *error = "PNG header can't be read";
        return false;
    }
    constexpr std::array<unsigned char, 8> signature{0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a};
    if (!std::equal(signature.begin(), signature.end(), header.begin()) || header[12] != 'I' || header[13] != 'H' ||
        header[14] != 'D' || header[15] != 'R') {
        if (error) *error = "Downloaded photo isn't a valid PNG";
        return false;
    }
    const auto read_u32 = [&header](std::size_t offset) {
        return (static_cast<std::uint32_t>(header[offset]) << 24U) |
               (static_cast<std::uint32_t>(header[offset + 1]) << 16U) |
               (static_cast<std::uint32_t>(header[offset + 2]) << 8U) | static_cast<std::uint32_t>(header[offset + 3]);
    };
    const auto width  = read_u32(16);
    const auto height = read_u32(20);
    const auto pixels = static_cast<std::uint64_t>(width) * height;
    if (width == 0 || height == 0 || width > kMaxImageDimension || height > kMaxImageDimension ||
        pixels * 4U > kMaxDecodedImageBytes) {
        if (error) *error = "PNG dimensions exceed the decoded image limit";
        return false;
    }
    return true;
}

#if defined(TELEGRAM_HAS_LVGL_TJPGD)
bool is_jpeg_extension(const std::string& extension)
{
    return extension == ".jpg" || extension == ".jpeg";
}

bool is_start_of_frame_marker(unsigned char marker)
{
    switch (marker) {
        case 0xc0:
        case 0xc1:
        case 0xc2:
        case 0xc3:
        case 0xc5:
        case 0xc6:
        case 0xc7:
        case 0xc9:
        case 0xca:
        case 0xcb:
        case 0xcd:
        case 0xce:
        case 0xcf:
            return true;
        default:
            return false;
    }
}

bool jpeg_is_bounded(const std::string& path, std::string* error)
{
    std::error_code filesystem_error;
    const auto file_size = std::filesystem::file_size(path, filesystem_error);
    if (filesystem_error || file_size > kMaxEncodedJpegBytes) {
        if (error) *error = "JPEG file exceeds the encoded size limit";
        return false;
    }

    std::ifstream file(path, std::ios::binary);
    std::array<unsigned char, 2> bytes{};
    if (!file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size())) ||
        bytes[0] != 0xff || bytes[1] != 0xd8) {
        if (error) *error = "Downloaded photo isn't a valid JPEG";
        return false;
    }

    const auto read_u16 = [&file](std::uint16_t* value) {
        std::array<unsigned char, 2> encoded{};
        if (!file.read(reinterpret_cast<char*>(encoded.data()), static_cast<std::streamsize>(encoded.size()))) {
            return false;
        }
        *value = static_cast<std::uint16_t>((static_cast<std::uint16_t>(encoded[0]) << 8U) | encoded[1]);
        return true;
    };

    for (;;) {
        int prefix = file.get();
        while (prefix != std::char_traits<char>::eof() && prefix != 0xff) prefix = file.get();
        if (prefix == std::char_traits<char>::eof()) break;

        int marker = file.get();
        while (marker == 0xff) marker = file.get();
        if (marker == std::char_traits<char>::eof()) break;
        if (marker == 0x00 || marker == 0xd8) continue;
        if (marker == 0xd9 || marker == 0xda) break;
        if (marker == 0x01 || (marker >= 0xd0 && marker <= 0xd7)) continue;

        std::uint16_t segment_length = 0;
        if (!read_u16(&segment_length) || segment_length < 2) break;
        if (is_start_of_frame_marker(static_cast<unsigned char>(marker))) {
            if (marker != 0xc0) {
                if (error) *error = "The built-in JPEG decoder supports baseline JPEG only";
                return false;
            }
            std::array<unsigned char, 5> frame{};
            if (segment_length < 7 ||
                !file.read(reinterpret_cast<char*>(frame.data()), static_cast<std::streamsize>(frame.size()))) {
                break;
            }
            const auto height = static_cast<std::uint32_t>((static_cast<std::uint16_t>(frame[1]) << 8U) | frame[2]);
            const auto width  = static_cast<std::uint32_t>((static_cast<std::uint16_t>(frame[3]) << 8U) | frame[4]);
            const auto pixels = static_cast<std::uint64_t>(width) * height;
            if (width == 0 || height == 0 || width > kMaxDirectJpegDimension || height > kMaxDirectJpegDimension ||
                pixels > kMaxDirectJpegPixelCount) {
                if (error) *error = "JPEG dimensions exceed the built-in decoder limit";
                return false;
            }
            return true;
        }

        file.seekg(static_cast<std::streamoff>(segment_length - 2U), std::ios::cur);
        if (!file) break;
    }

    if (error) *error = "JPEG dimensions can't be read";
    return false;
}
#endif

const char* connection_text(telegram::ConnectionState state)
{
    switch (state) {
        case telegram::ConnectionState::Ready:
            return "Online";
        case telegram::ConnectionState::WaitingForNetwork:
            return "No network";
        case telegram::ConnectionState::ConnectingToProxy:
            return "Proxy";
        case telegram::ConnectionState::Connecting:
            return "Connecting";
        case telegram::ConnectionState::Updating:
            return "Updating";
        case telegram::ConnectionState::Unknown:
            return "Starting";
    }
    return "Starting";
}

std::uint32_t connection_color(telegram::ConnectionState state)
{
    if (state == telegram::ConnectionState::Ready) return components::color_status_ready;
    if (state == telegram::ConnectionState::WaitingForNetwork) return components::color_status_warning;
    return components::color_text_secondary;
}

bool auth_accepts_input(telegram::AuthState state)
{
    using telegram::AuthState;
    return state == AuthState::WaitingForPhoneNumber || state == AuthState::WaitingForEmailAddress ||
           state == AuthState::WaitingForEmailCode || state == AuthState::WaitingForCode ||
           state == AuthState::WaitingForPassword;
}

const char* auth_title(telegram::AuthState state)
{
    using telegram::AuthState;
    switch (state) {
        case AuthState::WaitingForPhoneNumber:
            return "Sign in to Telegram";
        case AuthState::WaitingForEmailAddress:
            return "Recovery email";
        case AuthState::WaitingForEmailCode:
            return "Check your email";
        case AuthState::WaitingForCode:
            return "Verification code";
        case AuthState::WaitingForPassword:
            return "Two-step verification";
        case AuthState::WaitingForRegistration:
            return "Registration required";
        case AuthState::WaitingForOtherDeviceConfirmation:
            return "Confirm this sign-in";
        case AuthState::WaitingForPremiumPurchase:
            return "Premium required";
        case AuthState::Error:
            return "Telegram unavailable";
        case AuthState::Closed:
            return "Signed out";
        default:
            return "Opening Telegram";
    }
}

const char* auth_placeholder(telegram::AuthState state)
{
    using telegram::AuthState;
    switch (state) {
        case AuthState::WaitingForPhoneNumber:
            return "+86 138 0000 0000";
        case AuthState::WaitingForEmailAddress:
            return "Email address";
        case AuthState::WaitingForEmailCode:
            return "Email code";
        case AuthState::WaitingForCode:
            return "Telegram code";
        case AuthState::WaitingForPassword:
            return "Password";
        default:
            return {};
    }
}

std::uint32_t auth_max_length(telegram::AuthState state)
{
    using telegram::AuthState;
    switch (state) {
        case AuthState::WaitingForPhoneNumber:
            return 32;
        case AuthState::WaitingForEmailAddress:
            return 254;
        case AuthState::WaitingForEmailCode:
        case AuthState::WaitingForCode:
            return 128;
        case AuthState::WaitingForPassword:
            return 256;
        default:
            return 256;
    }
}

std::uint32_t avatar_color(std::int64_t id)
{
    constexpr std::array<std::uint32_t, 6> colors{components::color_avatar_red,    components::color_avatar_orange,
                                                  components::color_avatar_violet, components::color_avatar_green,
                                                  components::color_avatar_cyan,   components::color_action};
    const auto normalized = id < 0 ? static_cast<std::uint64_t>(-(id + 1)) + 1U : static_cast<std::uint64_t>(id);
    return colors[normalized % colors.size()];
}

}  // namespace

lv_obj_t* TelegramView::make_avatar(lv_obj_t* parent, const telegram::Chat& chat, std::int32_t size)
{
    auto* avatar = lv_obj_create(parent);
    lv_obj_set_name_static(avatar, "telegram_avatar");
    lv_obj_set_size(avatar, size, size);
    make_transparent(avatar);
    lv_obj_set_style_radius(avatar, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_clip_corner(avatar, true, 0);
    lv_obj_set_style_bg_color(avatar, lv_color_hex(avatar_color(chat.id)), 0);
    lv_obj_set_style_bg_opa(avatar, LV_OPA_COVER, 0);
    lv_obj_remove_flag(avatar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(avatar, LV_OBJ_FLAG_CLICKABLE);
    bool rendered = false;
    const auto cached = avatar_cache_.find(chat.photo.id);
    if (chat.photo.is_downloaded && !chat.photo.local_path.empty() && cached != avatar_cache_.end() &&
        cached->second.path == chat.photo.local_path && cached->second.image) {
        auto* image = lv_image_create(avatar);
        lv_obj_set_name_static(image, "telegram_avatar_image");
        lv_obj_set_size(image, size, size);
        lv_image_set_src(image, &cached->second.image->descriptor);
        lv_image_set_inner_align(image, LV_IMAGE_ALIGN_COVER);
        lv_obj_center(image);
        rendered = true;
    }
    if (!rendered) {
        auto* initial = make_label(avatar, first_character(chat.title).c_str(), size < 42 ? 18 : 22,
                                   components::color_container_background);
        lv_obj_set_name_static(initial, "telegram_avatar_initial");
        lv_obj_center(initial);
    }
    if (chat.photo.id > 0) avatar_contexts_.push_back({avatar, chat.photo});
    return avatar;
}

TelegramView::TelegramView(telegram::TelegramViewModel& view_model) : view_model_(view_model)
{
}

TelegramView::~TelegramView()
{
    destroy();
}

lv_obj_t* TelegramView::create(lv_obj_t* parent, const char* application_name)
{
    if (surface_) return surface_;
    application_name_ = application_name ? application_name : LILYGO_UI_TELEGRAM_APP_NAME;
    surface_          = components::create_page(parent);
    lv_obj_add_event_cb(surface_, size_changed_event, LV_EVENT_SIZE_CHANGED, this);

    build_header();

    body_ = lv_obj_create(surface_);
    lv_obj_set_name_static(body_, "telegram_body");
    lv_obj_set_width(body_, LV_PCT(100));
    lv_obj_set_flex_grow(body_, 1);
    make_transparent(body_);
    lv_obj_clear_flag(body_, LV_OBJ_FLAG_SCROLLABLE);

    keyboard_ = lv_keyboard_create(surface_);
    lv_obj_set_name_static(keyboard_, "telegram_keyboard");
    lv_obj_set_width(keyboard_, LV_PCT(100));
    lv_obj_set_height(keyboard_, LV_PCT(42));
    set_font(keyboard_, 22);
    lv_obj_add_event_cb(keyboard_, keyboard_event, LV_EVENT_READY, this);
    lv_obj_add_event_cb(keyboard_, keyboard_event, LV_EVENT_CANCEL, this);
    set_hidden(keyboard_, true);

    lv_obj_update_layout(surface_);
    update_responsive_state();
    rebuild();
    timer_ = lv_timer_create(timer_tick, 50, this);
    return surface_;
}

void TelegramView::destroy() noexcept
{
    if (timer_) lv_timer_delete(timer_);
    timer_ = nullptr;
    image_decoder_.stop();
    avatar_decoder_.stop();
    voice_player_.stop();
    image_results_.close();
    avatar_results_.close();
    voice_results_.close();
    if (surface_) lv_obj_delete(surface_);
    surface_       = nullptr;
    header_        = nullptr;
    header_back_   = nullptr;
    header_title_  = nullptr;
    header_status_ = nullptr;
    header_logout_ = nullptr;
    body_          = nullptr;
    keyboard_      = nullptr;
    auth_input_    = nullptr;
    composer_      = nullptr;
    message_list_  = nullptr;
    chat_list_     = nullptr;
    search_input_  = nullptr;
    send_button_   = nullptr;
    logout_dialog_ = nullptr;
    pending_account_chat_open_ = false;
    pending_account_confirmation_ = {};
    account_form_drafts_.clear();
    clear_dynamic_contexts();
    image_cache_.clear();
    avatar_cache_.clear();
    pending_avatars_.clear();
    image_cache_order_.clear();
    pending_images_.clear();
    requested_downloads_.clear();
    download_failures_.clear();
    download_retry_at_.clear();
    requested_images_.clear();
    image_failures_.clear();
    image_retry_at_.clear();
    image_cache_bytes_ = 0;
}

void TelegramView::timer_tick(lv_timer_t* timer)
{
    auto* view = static_cast<TelegramView*>(lv_timer_get_user_data(timer));
    if (view) view->tick();
}

void TelegramView::size_changed_event(lv_event_t* event)
{
    auto* view = static_cast<TelegramView*>(lv_event_get_user_data(event));
    if (!view || !view->surface_) return;
    const bool previous = view->wide_;
    view->update_responsive_state();
    if (previous != view->wide_) view->media_changed_ = true;
}

void TelegramView::chat_clicked_event(lv_event_t* event)
{
    auto* context = static_cast<ChatContext*>(lv_event_get_user_data(event));
    if (context && context->view) {
        context->view->hide_keyboard();
        context->view->chat_details_open_ = false;
        context->view->media_changed_ = true;
        context->view->view_model_.select_chat(context->chat_id);
    }
}

void TelegramView::back_clicked_event(lv_event_t* event)
{
    auto* view = static_cast<TelegramView*>(lv_event_get_user_data(event));
    if (view) {
        view->hide_keyboard();
        if (view->chat_details_open_) view->close_chat_details();
        else if (view->main_tab_ == MainTab::Chats) view->view_model_.back_to_chats();
        else view->account_back();
    }
}

void TelegramView::send_clicked_event(lv_event_t* event)
{
    auto* view = static_cast<TelegramView*>(lv_event_get_user_data(event));
    if (view) view->send_message();
}

void TelegramView::conversation_back_event(lv_event_t* event)
{
    auto* view = static_cast<TelegramView*>(lv_event_get_user_data(event));
    if (!view) return;
    view->hide_keyboard();
    if (view->chat_details_open_) view->close_chat_details();
    else view->view_model_.back_to_chats();
}

void TelegramView::chat_info_event(lv_event_t* event)
{
    auto* view = static_cast<TelegramView*>(lv_event_get_user_data(event));
    if (view) view->show_chat_details();
}

void TelegramView::auth_submit_event(lv_event_t* event)
{
    auto* view = static_cast<TelegramView*>(lv_event_get_user_data(event));
    if (view) view->submit_auth();
}

void TelegramView::logout_event(lv_event_t* event)
{
    auto* view = static_cast<TelegramView*>(lv_event_get_user_data(event));
    if (!view || view->logout_dialog_) return;
    view->hide_keyboard();
    auto* overlay        = lv_obj_create(view->surface_);
    view->logout_dialog_ = overlay;
    lv_obj_set_name_static(overlay, "telegram_logout_dialog");
    lv_obj_add_flag(overlay, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_set_size(overlay, LV_PCT(100), LV_PCT(100));
    make_transparent(overlay);
    lv_obj_set_style_bg_color(overlay, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(overlay, LV_OPA_40, 0);
    lv_obj_set_flex_flow(overlay, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(overlay, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    auto* panel = components::create_card(overlay);
    lv_obj_set_width(panel, LV_PCT(88));
    lv_obj_set_style_max_width(panel, 400, 0);
    lv_obj_set_height(panel, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(panel, 24, 0);
    lv_obj_set_style_pad_row(panel, 16, 0);
    lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
    auto* title = make_label(panel, "Log Out?", 22);
    lv_obj_set_width(title, LV_PCT(100));
    auto* text =
        make_label(panel, "You can sign back in with your phone number.", 14, components::color_text_secondary);
    lv_obj_set_width(text, LV_PCT(100));
    auto* confirm = components::create_button(panel, "Log Out", logout_confirm_event, view);
    lv_obj_set_name_static(confirm, "telegram_logout_confirm");
    lv_obj_set_width(confirm, LV_PCT(100));
    lv_obj_set_style_bg_color(confirm, lv_color_hex(components::color_destructive), 0);
    auto* cancel = components::create_button(panel, "Cancel", logout_cancel_event, view);
    lv_obj_set_name_static(cancel, "telegram_logout_cancel");
    lv_obj_set_width(cancel, LV_PCT(100));
}

void TelegramView::logout_confirm_event(lv_event_t* event)
{
    auto* view = static_cast<TelegramView*>(lv_event_get_user_data(event));
    if (!view) return;
    logout_cancel_event(event);
    if (view->view_model_.log_out()) {
        view->composer_draft_.clear();
        if (view->composer_) lv_textarea_set_text(view->composer_, "");
    }
}

void TelegramView::logout_cancel_event(lv_event_t* event)
{
    auto* view = static_cast<TelegramView*>(lv_event_get_user_data(event));
    if (view && view->logout_dialog_) {
        lv_obj_delete_async(view->logout_dialog_);
        view->logout_dialog_ = nullptr;
    }
}

void TelegramView::chats_tab_event(lv_event_t* event)
{
    auto* view = static_cast<TelegramView*>(lv_event_get_user_data(event));
    if (!view || view->main_tab_ == MainTab::Chats) return;
    view->hide_keyboard();
    view->main_tab_ = MainTab::Chats;
    view->chat_details_open_ = false;
    view->pending_account_chat_open_ = false;
    view->account_page_ = AccountPage::Settings;
    view->account_inputs_.clear();
    view->account_form_drafts_.clear();
    view->media_changed_    = true;
}

void TelegramView::settings_tab_event(lv_event_t* event)
{
    auto* view = static_cast<TelegramView*>(lv_event_get_user_data(event));
    if (!view || view->main_tab_ == MainTab::Settings) return;
    view->hide_keyboard();
    view->main_tab_ = MainTab::Settings;
    view->chat_details_open_ = false;
    view->show_account_page(AccountPage::Settings);
}

void TelegramView::contacts_tab_event(lv_event_t* event)
{
    auto* view = static_cast<TelegramView*>(lv_event_get_user_data(event));
    if (!view || view->main_tab_ == MainTab::Contacts) return;
    view->hide_keyboard();
    view->main_tab_ = MainTab::Contacts;
    view->chat_details_open_ = false;
    view->pending_account_chat_open_ = false;
    view->account_page_ = AccountPage::Settings;
    view->account_inputs_.clear();
    view->account_form_drafts_.clear();
    view->view_model_.clear_account_feedback();
    if (!view->view_model_.account().contacts_loaded) {
        telegram::AccountRequest request;
        request.action = telegram::AccountAction::LoadContacts;
        view->view_model_.account_action(request);
    }
    view->media_changed_ = true;
}

void TelegramView::contact_search_event(lv_event_t* event)
{
    auto* view = static_cast<TelegramView*>(lv_event_get_user_data(event));
    if (view && view->search_input_) {
        view->view_model_.set_contact_search(lv_textarea_get_text(view->search_input_));
        if (auto* list = lv_obj_find_by_name(view->body_, "telegram_contact_list"))
            lv_obj_scroll_to_y(list, 0, LV_ANIM_OFF);
    }
}

void TelegramView::contact_clear_event(lv_event_t* event)
{
    auto* view = static_cast<TelegramView*>(lv_event_get_user_data(event));
    if (view && view->search_input_) lv_textarea_set_text(view->search_input_, "");
}

void TelegramView::search_changed_event(lv_event_t* event)
{
    auto* view = static_cast<TelegramView*>(lv_event_get_user_data(event));
    if (!view || !view->search_input_) return;
    view->view_model_.set_chat_search(lv_textarea_get_text(view->search_input_));
    view->chat_scroll_y_ = 0;
    if (view->chat_list_) lv_obj_scroll_to_y(view->chat_list_, 0, LV_ANIM_OFF);
}

void TelegramView::search_clear_event(lv_event_t* event)
{
    auto* view = static_cast<TelegramView*>(lv_event_get_user_data(event));
    if (view && view->search_input_) lv_textarea_set_text(view->search_input_, "");
}

void TelegramView::composer_changed_event(lv_event_t* event)
{
    auto* view = static_cast<TelegramView*>(lv_event_get_user_data(event));
    if (!view || !view->composer_ || !view->send_button_) return;
    const std::string text = lv_textarea_get_text(view->composer_);
    const bool empty       = std::all_of(text.begin(), text.end(), [](unsigned char ch) { return std::isspace(ch); });
    if (empty)
        lv_obj_add_state(view->send_button_, LV_STATE_DISABLED);
    else
        lv_obj_remove_state(view->send_button_, LV_STATE_DISABLED);
}

void TelegramView::older_clicked_event(lv_event_t* event)
{
    auto* view = static_cast<TelegramView*>(lv_event_get_user_data(event));
    if (view) view->view_model_.load_older();
}

void TelegramView::input_focused_event(lv_event_t* event)
{
    auto* view = static_cast<TelegramView*>(lv_event_get_user_data(event));
    if (view) view->show_keyboard(static_cast<lv_obj_t*>(lv_event_get_target(event)));
}

void TelegramView::keyboard_event(lv_event_t* event)
{
    auto* view = static_cast<TelegramView*>(lv_event_get_user_data(event));
    if (!view) return;
    if (lv_event_get_code(event) == LV_EVENT_READY) {
        if (lv_keyboard_get_textarea(view->keyboard_) == view->auth_input_) {
            view->submit_auth();
        } else if (lv_keyboard_get_textarea(view->keyboard_) == view->composer_) {
            view->send_message();
        } else {
            view->hide_keyboard();
        }
    } else {
        view->hide_keyboard();
    }
}

void TelegramView::voice_clicked_event(lv_event_t* event)
{
    auto* context = static_cast<VoiceContext*>(lv_event_get_user_data(event));
    if (context && context->view) context->view->play_voice(context->message);
}

void TelegramView::tick()
{
    view_model_.pump();
    if (pending_account_chat_open_ && !view_model_.account_busy()) {
        pending_account_chat_open_ = false;
        if (view_model_.account_error().empty() && view_model_.model().current_chat_id() != 0) {
            main_tab_ = MainTab::Chats;
            chat_details_open_ = false;
            account_page_ = AccountPage::Settings;
            account_inputs_.clear();
            account_form_drafts_.clear();
            media_changed_ = true;
        }
    }

    ImageResult image;
    while (image_results_.try_pop(image)) {
        pending_images_.push_back(std::move(image));
        media_changed_ = true;
    }
    AvatarResult avatar;
    while (avatar_results_.try_pop(avatar)) {
        pending_avatars_.push_back(std::move(avatar));
        media_changed_ = true;
    }

    VoiceResult voice;
    while (voice_results_.try_pop(voice)) {
        if (!voice_result_matches(voice, active_voice_identity_, active_voice_request_id_)) continue;
        voice_state_ = voice.event.state;
        if (voice.event.state == telegram::media::VoicePlaybackState::error) {
            media_status_ = voice.event.message;
        } else if (voice.event.state == telegram::media::VoicePlaybackState::playing) {
            (void)view_model_.mark_message_content_opened(voice.identity.chat_id, voice.identity.message_id);
        } else if (voice.event.state == telegram::media::VoicePlaybackState::finished ||
                   voice.event.state == telegram::media::VoicePlaybackState::stopped) {
            media_status_.clear();
        }
        if (voice.event.state == telegram::media::VoicePlaybackState::error ||
            voice.event.state == telegram::media::VoicePlaybackState::finished ||
            voice.event.state == telegram::media::VoicePlaybackState::stopped) {
            active_voice_identity_   = {};
            active_voice_request_id_ = 0;
        }
        media_changed_ = true;
    }

    const auto now = lv_tick_get();
    for (const auto file_id : view_model_.take_failed_downloads()) {
        const auto avatar = avatar_cache_.find(file_id);
        if (avatar != avatar_cache_.end() && avatar->second.downloading) {
            avatar->second.downloading = false;
            if (avatar->second.failures < 2) ++avatar->second.failures;
            avatar->second.retry_at = now + (avatar->second.failures >= 2 ? 300000U : 60000U);
        }
        if (requested_downloads_.erase(file_id) == 0) continue;
        auto& failures = download_failures_[file_id];
        if (failures < std::numeric_limits<std::uint8_t>::max()) ++failures;
        if (failures < 2) download_retry_at_[file_id] = now + 2000U;
        media_changed_ = true;
    }
    for (auto iterator = download_retry_at_.begin(); iterator != download_retry_at_.end();) {
        if (static_cast<std::int32_t>(now - iterator->second) < 0) {
            ++iterator;
            continue;
        }
        iterator       = download_retry_at_.erase(iterator);
        media_changed_ = true;
    }
    for (auto iterator = image_retry_at_.begin(); iterator != image_retry_at_.end();) {
        if (static_cast<std::int32_t>(now - iterator->second) < 0) {
            ++iterator;
            continue;
        }
        requested_images_.erase(iterator->first);
        iterator       = image_retry_at_.erase(iterator);
        media_changed_ = true;
    }

    if (rendered_revision_ != view_model_.revision() || media_changed_) rebuild();
    if (message_list_ && lv_obj_get_scroll_bottom(message_list_) <= 24)
        (void)view_model_.mark_current_messages_viewed();
    request_visible_photos();
    request_visible_avatars();
}

void TelegramView::rebuild()
{
    if (!surface_ || !body_) return;

    const auto current_chat_id   = view_model_.model().current_chat_id();
    const auto& current_messages = view_model_.model().messages();
    const auto current_oldest_id = current_messages.empty() ? 0 : current_messages.front().id;
    const auto current_newest_id = current_messages.empty() ? 0 : current_messages.back().id;
    if (current_chat_id != rendered_chat_id_) {
        chat_details_open_ = false;
        requested_downloads_.clear();
        download_failures_.clear();
        download_retry_at_.clear();
        requested_images_.clear();
        image_failures_.clear();
        image_retry_at_.clear();
        pending_images_.clear();
        media_status_.clear();
    }
    scroll_restore_mode_ = current_chat_id == rendered_chat_id_ && rendered_chat_details_open_
                               ? ScrollRestoreMode::Position : ScrollRestoreMode::Bottom;
    if (message_list_ && current_chat_id == rendered_chat_id_) {
        saved_scroll_y_      = lv_obj_get_scroll_y(message_list_);
        saved_scroll_bottom_ = lv_obj_get_scroll_bottom(message_list_);
        if (current_oldest_id != rendered_oldest_message_id_ && current_newest_id == rendered_newest_message_id_) {
            scroll_restore_mode_ = ScrollRestoreMode::BottomDistance;
        } else if (current_newest_id == rendered_newest_message_id_ || saved_scroll_bottom_ > 80) {
            scroll_restore_mode_ = ScrollRestoreMode::Position;
        }
    }

    bool restore_auth_keyboard     = false;
    bool restore_composer_keyboard = false;
    bool restore_search_keyboard   = false;
    std::string restore_account_input;
    std::uint32_t cursor_position  = 0;
    const auto keyboard_mode       = lv_keyboard_get_mode(keyboard_);
    const bool same_auth_state     = rendered_auth_state_ == view_model_.model().auth_state();
    if (keyboard_ && !lv_obj_has_flag(keyboard_, LV_OBJ_FLAG_HIDDEN)) {
        auto* active              = lv_keyboard_get_textarea(keyboard_);
        restore_auth_keyboard     = active && active == auth_input_;
        restore_composer_keyboard = active && active == composer_;
        restore_search_keyboard   = active && active == search_input_;
        if (active && std::find(account_inputs_.begin(), account_inputs_.end(), active) != account_inputs_.end())
            restore_account_input = lv_obj_get_name(active);
        if (active) cursor_position = lv_textarea_get_cursor_pos(active);
    }
    const auto auth_state = view_model_.model().auth_state();
    if (auth_input_ && rendered_auth_state_ == auth_state)
        auth_draft_ = lv_textarea_get_text(auth_input_);
    else
        auth_draft_.clear();
    if (auth_state != telegram::AuthState::Ready) {
        main_tab_ = MainTab::Chats;
        account_page_ = AccountPage::Settings;
        account_form_drafts_.clear();
        account_inputs_.clear();
        pending_account_confirmation_ = {};
        pending_account_chat_open_ = false;
        chat_details_open_ = false;
        peer_scroll_y_ = 0;
        if (logout_dialog_) {
            lv_obj_delete_async(logout_dialog_);
            logout_dialog_ = nullptr;
        }
    }
    if (composer_ && view_model_.model().auth_state() == telegram::AuthState::Ready)
        view_model_.set_draft(rendered_chat_id_, lv_textarea_get_text(composer_));
    composer_draft_ = view_model_.draft(current_chat_id);
    if (chat_list_) chat_scroll_y_ = lv_obj_get_scroll_y(chat_list_);
    auto* account_scroll = lv_obj_find_by_name(body_, "telegram_settings");
    if (!account_scroll) account_scroll = lv_obj_find_by_name(body_, "telegram_contact_list");
    auto* peer_scroll = lv_obj_find_by_name(body_, "telegram_peer_details");
    if (rendered_chat_details_open_ && peer_scroll) peer_scroll_y_ = lv_obj_get_scroll_y(peer_scroll);
    else if (!account_scroll) account_scroll = peer_scroll;
    if (account_scroll)
        account_scroll_positions_[{rendered_main_tab_, rendered_account_page_}] = lv_obj_get_scroll_y(account_scroll);
    for (auto* input : account_inputs_) {
        const auto* name = lv_obj_get_name(input);
        if (name) account_form_drafts_[name] = lv_textarea_get_text(input);
    }
    hide_keyboard();
    std::unordered_set<std::int32_t> visible_avatars;
    for (const auto& avatar : avatar_contexts_) {
        if (lv_obj_is_visible(avatar.object)) visible_avatars.insert(avatar.file.id);
    }
    lv_obj_clean(body_);
    if (header_) lv_obj_clean(header_);
    clear_dynamic_contexts();
    auth_input_   = nullptr;
    composer_     = nullptr;
    message_list_ = nullptr;
    chat_list_    = nullptr;
    search_input_ = nullptr;
    send_button_  = nullptr;
    if (auth_state != telegram::AuthState::Ready ||
        rendered_cache_generation_ != view_model_.account().media_cache_generation) {
        ++avatar_generation_;
        avatar_cache_.clear();
        pending_avatars_.clear();
    }
    if (rendered_cache_generation_ != view_model_.account().media_cache_generation) {
        requested_images_.clear();
        pending_images_.clear();
        image_cache_.clear();
        image_cache_order_.clear();
        image_cache_bytes_ = 0;
        requested_downloads_.clear();
        voice_player_.stop();
        rendered_cache_generation_ = view_model_.account().media_cache_generation;
    }
    integrate_media_results();
    integrate_avatar_results();
    trim_avatar_cache(visible_avatars);
    build_header();

    if (view_model_.model().auth_state() == telegram::AuthState::Ready) {
        build_workspace();
    } else {
        build_auth();
    }

    rendered_revision_          = view_model_.revision();
    rendered_main_tab_          = main_tab_;
    rendered_account_page_      = account_page_;
    rendered_chat_details_open_ = chat_details_open_;
    rendered_auth_state_        = auth_state;
    rendered_chat_id_           = current_chat_id;
    rendered_oldest_message_id_ = current_oldest_id;
    rendered_newest_message_id_ = current_newest_id;
    media_changed_              = false;
    account_scroll = lv_obj_find_by_name(body_, "telegram_settings");
    if (!account_scroll) account_scroll = lv_obj_find_by_name(body_, "telegram_contact_list");
    peer_scroll = lv_obj_find_by_name(body_, "telegram_peer_details");
    if (chat_details_open_ && peer_scroll) {
        lv_obj_update_layout(peer_scroll);
        lv_obj_scroll_to_y(peer_scroll, peer_scroll_y_, LV_ANIM_OFF);
    } else if (!account_scroll) account_scroll = peer_scroll;
    if (account_scroll) {
        lv_obj_update_layout(account_scroll);
        lv_obj_scroll_to_y(account_scroll, account_scroll_positions_[{main_tab_, account_page_}], LV_ANIM_OFF);
    }
    lv_obj_t* restore_input     = restore_auth_keyboard       ? auth_input_
                                  : restore_composer_keyboard ? composer_
                                  : restore_search_keyboard   ? search_input_
                                                              : nullptr;
    if (!restore_account_input.empty()) restore_input = lv_obj_find_by_name(body_, restore_account_input.c_str());
    if (restore_input) {
        lv_textarea_set_cursor_pos(restore_input, cursor_position);
        lv_obj_add_state(restore_input, LV_STATE_FOCUSED);
        show_keyboard(restore_input);
        if (!restore_auth_keyboard || same_auth_state) lv_keyboard_set_mode(keyboard_, keyboard_mode);
    }
}

void TelegramView::build_header()
{
    if (!header_) {
        header_ = lv_obj_create(surface_);
        lv_obj_set_name_static(header_, "telegram_header");
        lv_obj_set_size(header_, LV_PCT(100), 72);
        lv_obj_set_style_radius(header_, 0, 0);
        lv_obj_set_style_border_width(header_, 0, 0);
        lv_obj_set_style_border_side(header_, LV_BORDER_SIDE_BOTTOM, 0);
        lv_obj_set_style_border_width(header_, 1, 0);
        lv_obj_set_style_border_color(header_, lv_color_hex(components::color_border), 0);
        lv_obj_set_style_bg_color(header_, lv_color_hex(components::color_page_background), 0);
        lv_obj_set_style_bg_opa(header_, LV_OPA_COVER, 0);
        lv_obj_set_style_pad_hor(header_, 16, 0);
        lv_obj_set_style_pad_ver(header_, 10, 0);
        lv_obj_clear_flag(header_, LV_OBJ_FLAG_SCROLLABLE);
    }
    lv_obj_clean(header_);
    const auto& model  = view_model_.model();
    const bool ready   = model.auth_state() == telegram::AuthState::Ready;
    const auto* chat   = model.current_chat();
    const bool in_chat = ready && chat && main_tab_ == MainTab::Chats;
    set_hidden(header_, ready && wide_);
    header_back_ = make_icon_button(header_, "telegram_back", LV_SYMBOL_LEFT, back_clicked_event, this);
    if (chat_details_open_) lv_obj_set_name_static(header_back_, "telegram_peer_back");
    lv_obj_align(header_back_, LV_ALIGN_LEFT_MID, 0, 0);
    set_hidden(header_back_, !in_chat && account_page_ == AccountPage::Settings);
    const std::string title = !ready              ? application_name_
                              : main_tab_ == MainTab::Settings ? account_page_title()
                              : main_tab_ == MainTab::Contacts ? (account_page_ == AccountPage::Settings ? "Contacts" : account_page_title())
                              : in_chat           ? (chat_details_open_ ? "Info" : chat->title)
                                                  : "Chats";
    header_title_           = make_label(header_, title.c_str(), 22, components::color_text);
    lv_obj_set_name_static(header_title_, "telegram_header_title");
    lv_obj_set_width(header_title_, LV_PCT(60));
    lv_label_set_long_mode(header_title_, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(header_title_, LV_TEXT_ALIGN_CENTER, 0);
    const bool show_status = ready && model.connection_state() != telegram::ConnectionState::Ready;
    lv_obj_align(header_title_, LV_ALIGN_CENTER, 0, show_status ? -10 : 0);
    header_status_ = make_label(header_, show_status ? connection_text(model.connection_state()) : "", 14,
                                connection_color(model.connection_state()));
    lv_obj_set_width(header_status_, LV_PCT(68));
    lv_label_set_long_mode(header_status_, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(header_status_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(header_status_, LV_ALIGN_BOTTOM_MID, 0, 2);
    if (in_chat && !chat_details_open_) {
        lv_obj_set_name_static(header_title_, "telegram_chat_info");
        lv_obj_add_flag(header_title_, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_ext_click_area(header_title_, 10);
        lv_obj_add_event_cb(header_title_, chat_info_event, LV_EVENT_CLICKED, this);
        auto* avatar = make_avatar(header_, *chat, 42);
        lv_obj_add_flag(avatar, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(avatar, chat_info_event, LV_EVENT_CLICKED, this);
        lv_obj_align(avatar, LV_ALIGN_RIGHT_MID, 0, 0);
    }
    header_logout_ = nullptr;
}

void TelegramView::build_auth()
{
    lv_obj_add_flag(body_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(body_, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(body_, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_style_bg_color(body_, lv_color_hex(components::color_page_background), 0);
    lv_obj_set_style_bg_opa(body_, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(body_, wide_ ? 36 : 20, 0);
    lv_obj_set_flex_flow(body_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(body_, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    auto* panel = lv_obj_create(body_);
    make_transparent(panel);
    lv_obj_remove_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_name_static(panel, "telegram_auth_panel");
    lv_obj_set_width(panel, wide_ ? 460 : LV_PCT(94));
    lv_obj_set_height(panel, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(panel, wide_ ? 32 : 24, 0);
    lv_obj_set_style_pad_row(panel, 18, 0);
    lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
    auto* logo = make_label(panel, "\xef\x81\xb5", 48, components::color_action);
    lv_obj_set_width(logo, LV_PCT(100));
    lv_obj_set_style_text_align(logo, LV_TEXT_ALIGN_CENTER, 0);

    const auto state = view_model_.model().auth_state();
    auto* title      = make_label(panel, auth_title(state), 28, components::color_container_title);
    lv_obj_set_name_static(title, "telegram_auth_title");
    lv_obj_set_width(title, LV_PCT(100));
    lv_label_set_long_mode(title, LV_LABEL_LONG_WRAP);

    auto* disclosure = make_label(panel, "Unofficial client using the Telegram API.", 14,
                                  components::color_text_secondary);
    lv_obj_set_width(disclosure, LV_PCT(100));
    lv_label_set_long_mode(disclosure, LV_LABEL_LONG_WRAP);

    auto* status = make_label(panel, view_model_.status_text().c_str(), 14, components::color_text_secondary);
    lv_obj_set_name_static(status, "telegram_auth_status");
    lv_obj_set_width(status, LV_PCT(100));
    lv_label_set_long_mode(status, LV_LABEL_LONG_WRAP);

    if (!view_model_.error_text().empty()) {
        auto* error = make_label(panel, view_model_.error_text().c_str(), 14, components::color_destructive);
        lv_obj_set_name_static(error, "telegram_auth_error");
        lv_obj_set_width(error, LV_PCT(100));
        lv_label_set_long_mode(error, LV_LABEL_LONG_WRAP);
    }

    if (!auth_accepts_input(state)) return;

    auth_input_ = lv_textarea_create(panel);
    lv_obj_set_name_static(auth_input_, "telegram_auth_input");
    lv_obj_set_size(auth_input_, LV_PCT(100), 58);
    lv_textarea_set_one_line(auth_input_, true);
    lv_textarea_set_max_length(auth_input_, auth_max_length(state));
    lv_textarea_set_placeholder_text(auth_input_, auth_placeholder(state));
    lv_textarea_set_text(auth_input_, auth_draft_.c_str());
    lv_textarea_set_password_mode(auth_input_, state == telegram::AuthState::WaitingForPassword);
    set_font(auth_input_, 22);
    lv_obj_set_style_radius(auth_input_, 8, 0);
    lv_obj_set_style_border_color(auth_input_, lv_color_hex(components::color_border), 0);
    lv_obj_add_event_cb(auth_input_, input_focused_event, LV_EVENT_FOCUSED, this);

    auto* submit = components::create_button(panel, "Continue", auth_submit_event, this);
    lv_obj_set_name_static(submit, "telegram_auth_submit");
    lv_obj_set_width(submit, LV_PCT(100));
}

void TelegramView::build_workspace()
{
    lv_obj_clear_flag(body_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(body_, 0, 0);
    lv_obj_set_style_bg_color(body_, lv_color_hex(components::color_page_background), 0);
    lv_obj_set_style_bg_opa(body_, LV_OPA_COVER, 0);
    lv_obj_set_flex_flow(body_, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(body_, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

    const bool has_chat = view_model_.model().current_chat_id() != 0;
    if (wide_ || !has_chat || main_tab_ != MainTab::Chats) build_chat_list(body_);
    if (wide_ || (has_chat && main_tab_ == MainTab::Chats)) build_conversation(body_);
}

void TelegramView::build_chat_list(lv_obj_t* parent)
{
    auto* sidebar = lv_obj_create(parent);
    lv_obj_set_name_static(sidebar, "telegram_chat_sidebar");
    lv_obj_set_height(sidebar, LV_PCT(100));
    lv_obj_set_width(sidebar, wide_ ? 380 : LV_PCT(100));
    lv_obj_set_style_radius(sidebar, 0, 0);
    lv_obj_set_style_border_width(sidebar, wide_ ? 1 : 0, 0);
    lv_obj_set_style_border_side(sidebar, LV_BORDER_SIDE_RIGHT, 0);
    lv_obj_set_style_border_color(sidebar, lv_color_hex(components::color_border), 0);
    lv_obj_set_style_bg_color(sidebar, lv_color_hex(components::color_page_background), 0);
    lv_obj_set_style_bg_opa(sidebar, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(sidebar, 0, 0);
    lv_obj_clear_flag(sidebar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(sidebar, LV_FLEX_FLOW_COLUMN);

    if (wide_) {
        auto* toolbar = lv_obj_create(sidebar);
        lv_obj_set_size(toolbar, LV_PCT(100), 72);
        make_transparent(toolbar);
        lv_obj_remove_flag(toolbar, LV_OBJ_FLAG_SCROLLABLE);
        const char* title_text = main_tab_ == MainTab::Chats ? "Chats" :
                                 main_tab_ == MainTab::Contacts && account_page_ == AccountPage::Settings ? "Contacts" :
                                 account_page_title();
        auto* title = make_label(toolbar, title_text, 22);
        lv_obj_set_width(title, LV_PCT(70));
        lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_center(title);
        if (main_tab_ != MainTab::Chats && account_page_ != AccountPage::Settings) {
            auto* back = make_icon_button(toolbar, "telegram_account_back", LV_SYMBOL_LEFT, back_clicked_event, this);
            lv_obj_align(back, LV_ALIGN_LEFT_MID, 10, 0);
        }
        if (view_model_.model().connection_state() != telegram::ConnectionState::Ready) {
            lv_obj_align(title, LV_ALIGN_CENTER, 0, -10);
            auto* status = make_label(toolbar, connection_text(view_model_.model().connection_state()), 14,
                                      components::color_text_secondary);
            lv_obj_align(status, LV_ALIGN_BOTTOM_MID, 0, -6);
        }
    }
    if (main_tab_ == MainTab::Settings || (main_tab_ == MainTab::Contacts && account_page_ != AccountPage::Settings)) {
        build_settings(sidebar);
        build_tabs(sidebar);
        return;
    }
    if (main_tab_ == MainTab::Contacts) {
        build_contacts(sidebar);
        build_tabs(sidebar);
        return;
    }

    auto* search_bar = lv_obj_create(sidebar);
    lv_obj_set_size(search_bar, LV_PCT(100), 64);
    make_transparent(search_bar);
    lv_obj_set_style_pad_hor(search_bar, 16, 0);
    lv_obj_set_style_pad_bottom(search_bar, 12, 0);
    lv_obj_remove_flag(search_bar, LV_OBJ_FLAG_SCROLLABLE);
    auto* search_field = lv_obj_create(search_bar);
    lv_obj_set_size(search_field, LV_PCT(100), LV_PCT(100));
    make_transparent(search_field);
    lv_obj_set_style_radius(search_field, 8, 0);
    lv_obj_set_style_bg_color(search_field, lv_color_hex(components::color_search_background), 0);
    lv_obj_set_style_bg_opa(search_field, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_left(search_field, 12, 0);
    lv_obj_set_flex_flow(search_field, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(search_field, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(search_field, LV_OBJ_FLAG_SCROLLABLE);
    make_label(search_field, "\xef\x80\x82", 22, components::color_text_secondary);
    search_input_ = lv_textarea_create(search_field);
    lv_obj_set_name_static(search_input_, "telegram_chat_search");
    lv_obj_set_height(search_input_, LV_PCT(100));
    lv_obj_set_flex_grow(search_input_, 1);
    make_transparent(search_input_);
    lv_obj_set_style_pad_ver(search_input_, 12, 0);
    lv_obj_set_style_pad_left(search_input_, 8, 0);
    set_font(search_input_, 22);
    lv_textarea_set_one_line(search_input_, true);
    lv_textarea_set_max_length(search_input_, 128);
    lv_textarea_set_placeholder_text(search_input_, "Search");
    lv_textarea_set_text(search_input_, view_model_.chat_search().c_str());
    lv_obj_add_event_cb(search_input_, search_changed_event, LV_EVENT_VALUE_CHANGED, this);
    lv_obj_add_event_cb(search_input_, input_focused_event, LV_EVENT_FOCUSED, this);
    auto* clear = make_icon_button(search_field, "telegram_search_clear", LV_SYMBOL_CLOSE, search_clear_event, this);
    set_hidden(clear, view_model_.chat_search().empty());

    auto* list = lv_obj_create(sidebar);
    chat_list_ = list;
    lv_obj_set_name_static(list, "telegram_chat_list");
    lv_obj_set_width(list, LV_PCT(100));
    lv_obj_set_flex_grow(list, 1);
    make_transparent(list);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_AUTO);

    const auto selected       = view_model_.model().current_chat_id();
    std::size_t visible_chats = 0;
    for (const auto& chat : view_model_.model().chats()) {
        if (!view_model_.chat_matches_search(chat)) continue;
        ++visible_chats;
        auto context     = std::make_unique<ChatContext>();
        context->view    = this;
        context->chat_id = chat.id;

        auto* row = lv_button_create(list);
        lv_obj_set_name_static(row, "telegram_chat_row");
        lv_obj_set_width(row, LV_PCT(100));
        lv_obj_set_height(row, 94);
        lv_obj_set_style_radius(row, 0, 0);
        lv_obj_set_style_shadow_width(row, 0, 0);
        lv_obj_set_style_bg_color(row, lv_color_hex(components::color_selection), 0);
        lv_obj_set_style_bg_opa(row, chat.id == selected ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_50, LV_STATE_PRESSED);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_pad_hor(row, 16, 0);
        lv_obj_set_style_pad_ver(row, 10, 0);
        lv_obj_set_style_pad_column(row, 12, 0);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_add_event_cb(row, chat_clicked_event, LV_EVENT_CLICKED, context.get());

        make_avatar(row, chat, 60);

        auto* text_column = lv_obj_create(row);
        lv_obj_set_height(text_column, LV_PCT(100));
        lv_obj_set_flex_grow(text_column, 1);
        make_transparent(text_column);
        lv_obj_remove_flag(text_column, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_remove_flag(text_column, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_pad_top(text_column, 4, 0);
        lv_obj_set_style_pad_row(text_column, 6, 0);
        lv_obj_set_flex_flow(text_column, LV_FLEX_FLOW_COLUMN);

        auto* name = make_label(text_column, chat.title.c_str(), 22, components::color_container_title);
        lv_obj_set_width(name, LV_PCT(100));
        lv_obj_set_height(name, 28);
        lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
        auto* preview =
            make_label(text_column, chat.last_message_preview.c_str(), 18, components::color_text_secondary);
        lv_obj_set_width(preview, LV_PCT(100));
        lv_obj_set_height(preview, 24);
        lv_label_set_long_mode(preview, LV_LABEL_LONG_DOT);

        auto* meta = lv_obj_create(row);
        lv_obj_set_size(meta, LV_SIZE_CONTENT, LV_PCT(100));
        make_transparent(meta);
        lv_obj_remove_flag(meta, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_remove_flag(meta, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_pad_top(meta, 6, 0);
        lv_obj_set_style_pad_row(meta, 5, 0);
        lv_obj_set_flex_flow(meta, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(meta, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
        auto time = date_text(chat.last_message_date, true);
        if (time.empty()) time = format_time(chat.last_message_date);
        make_label(meta, time.c_str(), 14, components::color_text_secondary);
        if (chat.unread_count > 0) {
            char unread_text[12]{};
            if (chat.unread_count > 999)
                std::snprintf(unread_text, sizeof(unread_text), "999+");
            else
                std::snprintf(unread_text, sizeof(unread_text), "%d", chat.unread_count);
            auto* unread = make_label(meta, unread_text, 14, components::color_container_background);
            lv_obj_set_name_static(unread, "telegram_unread_badge");
            lv_obj_set_style_bg_color(
                unread, lv_color_hex(chat.is_muted ? components::color_muted_badge : components::color_action), 0);
            lv_obj_set_style_bg_opa(unread, LV_OPA_COVER, 0);
            lv_obj_set_style_radius(unread, LV_RADIUS_CIRCLE, 0);
            lv_obj_set_style_pad_hor(unread, 7, 0);
            lv_obj_set_style_pad_ver(unread, 2, 0);
        } else if (chat.is_muted) {
            make_label(meta, LV_SYMBOL_MUTE, 14, components::color_text_secondary);
        }

        auto* separator = lv_obj_create(row);
        make_transparent(separator);
        lv_obj_add_flag(separator, LV_OBJ_FLAG_IGNORE_LAYOUT);
        lv_obj_remove_flag(separator, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_remove_flag(separator, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_event_cb(
            row,
            [](lv_event_t* event) {
                auto* line  = static_cast<lv_obj_t*>(lv_event_get_user_data(event));
                auto* owner = static_cast<lv_obj_t*>(lv_event_get_current_target(event));
                lv_obj_set_width(line, std::max<std::int32_t>(0, lv_obj_get_content_width(owner) - 72));
            },
            LV_EVENT_SIZE_CHANGED, separator);
        lv_obj_set_height(separator, 1);
        lv_obj_set_style_bg_color(separator, lv_color_hex(components::color_border), 0);
        lv_obj_set_style_bg_opa(separator, LV_OPA_COVER, 0);
        lv_obj_align(separator, LV_ALIGN_BOTTOM_RIGHT, 0, 10);

        chat_contexts_.push_back(std::move(context));
    }

    if (visible_chats == 0) {
        auto* empty = make_label(list, view_model_.chat_search().empty() ? "No chats yet" : "No Results", 22,
                                 components::color_text_secondary);
        lv_obj_set_name_static(empty, "telegram_empty_chats");
        lv_obj_set_width(empty, LV_PCT(100));
        lv_obj_set_style_text_align(empty, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_margin_top(empty, 48, 0);
        lv_obj_set_style_align(empty, LV_ALIGN_TOP_MID, 0);
    }
    build_tabs(sidebar);
    lv_obj_update_layout(sidebar);
    lv_obj_scroll_to_y(list, chat_scroll_y_, LV_ANIM_OFF);
}

void TelegramView::build_tabs(lv_obj_t* parent)
{
    auto* tabs = lv_obj_create(parent);
    lv_obj_set_name_static(tabs, "telegram_tab_bar");
    lv_obj_set_size(tabs, LV_PCT(100), 72);
    make_transparent(tabs);
    lv_obj_set_style_border_side(tabs, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_border_width(tabs, 1, 0);
    lv_obj_set_style_border_color(tabs, lv_color_hex(components::color_border), 0);
    lv_obj_remove_flag(tabs, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(tabs, LV_FLEX_FLOW_ROW);
    constexpr std::array<const char*, 3> names{"telegram_tab_contacts", "telegram_tab_chats", "telegram_tab_settings"};
    constexpr std::array<const char*, 3> titles{"Contacts", "Chats", "Settings"};
    constexpr std::array<const char*, 3> icons{"\xef\x80\x87", "\xef\x81\xb5", LV_SYMBOL_SETTINGS};
    const std::array<lv_event_cb_t, 3> callbacks{contacts_tab_event, chats_tab_event, settings_tab_event};
    for (std::size_t index = 0; index < names.size(); ++index) {
        const bool selected = index == static_cast<std::size_t>(main_tab_);
        auto* tab           = lv_button_create(tabs);
        lv_obj_set_name_static(tab, names[index]);
        make_transparent(tab);
        lv_obj_set_style_shadow_width(tab, 0, 0);
        lv_obj_set_flex_grow(tab, 1);
        lv_obj_set_height(tab, LV_PCT(100));
        lv_obj_set_style_pad_row(tab, 3, 0);
        lv_obj_set_flex_flow(tab, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(tab, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_add_event_cb(tab, callbacks[index], LV_EVENT_CLICKED, this);
        const auto color = selected ? components::color_action : components::color_text_secondary;
        make_label(tab, icons[index], 22, color);
        make_label(tab, titles[index], 14, color);
    }
}

void TelegramView::build_contacts(lv_obj_t* parent)
{
    auto* panel = lv_obj_create(parent);
    lv_obj_set_name_static(panel, "telegram_contacts");
    make_transparent(panel);
    lv_obj_set_width(panel, LV_PCT(100));
    lv_obj_set_flex_grow(panel, 1);
    lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_remove_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    const auto action_button = [this](lv_obj_t* owner, const char* name, const char* symbol, std::function<void()> action) {
        auto context = std::make_unique<AccountUiAction>();
        context->run = std::move(action);
        auto* button = make_icon_button(owner, name, symbol, account_ui_event, context.get());
        account_actions_.push_back(std::move(context));
        return button;
    };
    auto* toolbar = lv_obj_create(panel);
    make_transparent(toolbar);
    lv_obj_set_size(toolbar, LV_PCT(100), 64);
    lv_obj_set_style_pad_hor(toolbar, 12, 0);
    lv_obj_set_style_pad_column(toolbar, 6, 0);
    lv_obj_set_flex_flow(toolbar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(toolbar, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(toolbar, LV_OBJ_FLAG_SCROLLABLE);
    search_input_ = lv_textarea_create(toolbar);
    lv_obj_set_name_static(search_input_, "telegram_contact_search");
    lv_obj_set_height(search_input_, 48);
    lv_obj_set_flex_grow(search_input_, 1);
    lv_obj_set_style_radius(search_input_, 8, 0);
    lv_obj_set_style_border_width(search_input_, 0, 0);
    lv_obj_set_style_bg_color(search_input_, lv_color_hex(components::color_search_background), 0);
    lv_obj_set_style_pad_ver(search_input_, 10, 0);
    set_font(search_input_, 22);
    lv_textarea_set_one_line(search_input_, true);
    lv_textarea_set_max_length(search_input_, 128);
    lv_textarea_set_placeholder_text(search_input_, "Search");
    lv_textarea_set_text(search_input_, view_model_.contact_search().c_str());
    lv_obj_add_event_cb(search_input_, contact_search_event, LV_EVENT_VALUE_CHANGED, this);
    lv_obj_add_event_cb(search_input_, input_focused_event, LV_EVENT_FOCUSED, this);
    auto* clear = make_icon_button(toolbar, "telegram_contact_search_clear", LV_SYMBOL_CLOSE, contact_clear_event, this);
    set_hidden(clear, view_model_.contact_search().empty());
    action_button(toolbar, "telegram_contact_add", LV_SYMBOL_PLUS, [this] { show_account_page(AccountPage::AddContact); });

    auto* controls = lv_obj_create(panel);
    make_transparent(controls);
    lv_obj_set_size(controls, LV_PCT(100), 54);
    lv_obj_set_style_pad_hor(controls, 16, 0);
    lv_obj_set_style_pad_column(controls, 8, 0);
    lv_obj_set_flex_flow(controls, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(controls, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(controls, LV_OBJ_FLAG_SCROLLABLE);
    auto* sort_label = make_label(controls, "Sort by", 14, components::color_text_secondary);
    lv_obj_set_flex_grow(sort_label, 1);
    auto* sort = lv_dropdown_create(controls);
    lv_obj_set_name_static(sort, "telegram_contacts_sort");
    lv_obj_set_size(sort, 140, 48);
    set_font(sort, 14);
    lv_dropdown_set_options(sort, "Name\nOnline First");
    lv_dropdown_set_selected(sort, view_model_.preferences().contacts_sort_online ? 1 : 0);
    lv_obj_add_event_cb(sort, [](lv_event_t* event) {
        auto* view = static_cast<TelegramView*>(lv_event_get_user_data(event));
        auto preferences = view->view_model_.preferences();
        preferences.contacts_sort_online = lv_dropdown_get_selected(static_cast<lv_obj_t*>(lv_event_get_target(event))) == 1;
        view->view_model_.set_preferences(preferences);
    }, LV_EVENT_VALUE_CHANGED, this);
    auto* refresh = action_button(controls, "telegram_contacts_refresh", LV_SYMBOL_REFRESH, [this] {
        telegram::AccountRequest request;
        request.action = telegram::AccountAction::LoadContacts;
        view_model_.account_action(request);
    });
    if (view_model_.account_busy()) lv_obj_add_state(refresh, LV_STATE_DISABLED);
    if (!view_model_.account_error().empty()) {
        auto* error = make_label(panel, view_model_.account_error().c_str(), 14, components::color_destructive);
        lv_obj_set_width(error, LV_PCT(100));
        lv_obj_set_style_pad_hor(error, 16, 0);
    }
    auto* list = lv_obj_create(panel);
    lv_obj_set_name_static(list, "telegram_contact_list");
    make_transparent(list);
    lv_obj_set_width(list, LV_PCT(100));
    lv_obj_set_flex_grow(list, 1);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    const auto contacts = view_model_.filtered_contacts();
    for (const auto& contact : contacts) {
        const auto name = telegram::contact_name(contact);
        auto context = std::make_unique<AccountUiAction>();
        context->run = [this, id = contact.id] {
            if (view_model_.account_busy() || pending_account_chat_open_) return;
            hide_keyboard();
            telegram::AccountRequest request;
            request.action = telegram::AccountAction::OpenContact;
            request.target_id = id;
            if (view_model_.account_action(request)) {
                pending_account_chat_open_ = true;
            }
        };
        auto* row = lv_button_create(list);
        lv_obj_set_name_static(row, "telegram_contact_row");
        make_transparent(row);
        if (view_model_.account_busy() || pending_account_chat_open_) lv_obj_add_state(row, LV_STATE_DISABLED);
        lv_obj_set_size(row, LV_PCT(100), 88);
        lv_obj_set_style_shadow_width(row, 0, 0);
        lv_obj_set_style_bg_color(row, lv_color_hex(components::color_selection), LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_STATE_PRESSED);
        lv_obj_set_style_pad_hor(row, 16, 0);
        lv_obj_set_style_pad_column(row, 12, 0);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_add_event_cb(row, account_ui_event, LV_EVENT_CLICKED, context.get());
        account_actions_.push_back(std::move(context));
        telegram::Chat avatar_chat;
        avatar_chat.id = contact.id;
        avatar_chat.title = name;
        avatar_chat.photo = contact.photo;
        make_avatar(row, avatar_chat, 52);
        auto* texts = lv_obj_create(row);
        make_transparent(texts);
        lv_obj_set_flex_grow(texts, 1);
        lv_obj_set_height(texts, 60);
        lv_obj_set_flex_flow(texts, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(texts, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
        lv_obj_set_style_pad_row(texts, 4, 0);
        lv_obj_remove_flag(texts, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_remove_flag(texts, LV_OBJ_FLAG_SCROLLABLE);
        auto* title = make_label(texts, name.c_str(), 22);
        lv_obj_set_size(title, LV_PCT(100), 28);
        lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);
        auto* status = make_label(texts, contact.status.c_str(), 14,
                                  contact.is_online ? components::color_action : components::color_text_secondary);
        lv_obj_set_size(status, LV_PCT(100), 20);
        lv_label_set_long_mode(status, LV_LABEL_LONG_DOT);
        action_button(row, "telegram_contact_info", "\xef\x81\x9a", [this, id = contact.id] {
            selected_contact_id_ = id;
            account_scroll_positions_[{MainTab::Contacts, AccountPage::ContactDetails}] = 0;
            show_account_page(AccountPage::ContactDetails);
        });
    }
    if (contacts.empty()) {
        auto* empty = make_label(list, !view_model_.account().contacts_loaded ?
            (view_model_.account_busy() ? "Loading..." : "Contacts unavailable") :
            view_model_.contact_search().empty() ? "No Contacts" : "No Results", 22, components::color_text_secondary);
        lv_obj_set_name_static(empty, "telegram_contacts_empty");
        lv_obj_set_width(empty, LV_PCT(100));
        lv_obj_set_style_text_align(empty, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_margin_top(empty, 32, 0);
    }
    if (view_model_.account().contacts_loaded) {
        const auto& account = view_model_.account();
        const auto count = account.contacts.size() < static_cast<std::size_t>(account.contacts_total) ?
            std::to_string(account.contacts.size()) + " of " + std::to_string(account.contacts_total) + " contacts" :
            std::to_string(account.contacts.size()) + " contacts";
        auto* total = make_label(panel, count.c_str(), 14, components::color_text_secondary);
        lv_obj_set_width(total, LV_PCT(100));
        lv_obj_set_style_pad_ver(total, 8, 0);
        lv_obj_set_style_text_align(total, LV_TEXT_ALIGN_CENTER, 0);
    }
}

void TelegramView::build_conversation(lv_obj_t* parent)
{
    auto* conversation = lv_obj_create(parent);
    lv_obj_set_name_static(conversation, "telegram_conversation");
    lv_obj_set_height(conversation, LV_PCT(100));
    lv_obj_set_flex_grow(conversation, 1);
    make_transparent(conversation);
    lv_obj_set_style_bg_color(conversation, lv_color_hex(components::color_conversation_background), 0);
    lv_obj_set_style_bg_opa(conversation, LV_OPA_COVER, 0);
    lv_obj_set_flex_flow(conversation, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(conversation, LV_OBJ_FLAG_SCROLLABLE);

    const auto* chat = view_model_.model().current_chat();
    if (!chat) {
        lv_obj_set_flex_align(conversation, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        auto* icon = make_label(conversation, "\xef\x81\xb5", 48, components::color_action);
        lv_obj_set_style_margin_bottom(icon, 16, 0);
        auto* empty = make_label(conversation, "Select a chat", 22, components::color_text_secondary);
        lv_obj_set_name_static(empty, "telegram_empty_conversation");
        return;
    }

    if (wide_) {
        auto* toolbar = lv_obj_create(conversation);
        lv_obj_set_name_static(toolbar, "telegram_conversation_header");
        lv_obj_set_size(toolbar, LV_PCT(100), 72);
        lv_obj_set_style_radius(toolbar, 0, 0);
        lv_obj_set_style_border_width(toolbar, 1, 0);
        lv_obj_set_style_border_side(toolbar, LV_BORDER_SIDE_BOTTOM, 0);
        lv_obj_set_style_border_color(toolbar, lv_color_hex(components::color_border), 0);
        lv_obj_set_style_bg_color(toolbar, lv_color_hex(components::color_page_background), 0);
        lv_obj_set_style_bg_opa(toolbar, LV_OPA_COVER, 0);
        lv_obj_set_style_pad_hor(toolbar, 18, 0);
        lv_obj_set_style_pad_ver(toolbar, 6, 0);
        lv_obj_clear_flag(toolbar, LV_OBJ_FLAG_SCROLLABLE);
        auto* back = make_icon_button(toolbar, "telegram_conversation_back", LV_SYMBOL_LEFT, conversation_back_event, this);
        if (chat_details_open_) lv_obj_set_name_static(back, "telegram_peer_back");
        lv_obj_align(back, LV_ALIGN_LEFT_MID, 0, 0);
        auto* title = make_label(toolbar, chat_details_open_ ? "Info" : chat->title.c_str(), 22, components::color_container_title);
        lv_obj_set_width(title, LV_PCT(72));
        lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);
        lv_obj_center(title);
        if (!chat_details_open_) {
            lv_obj_set_name_static(title, "telegram_chat_info");
            lv_obj_add_flag(title, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_set_ext_click_area(title, 10);
            lv_obj_add_event_cb(title, chat_info_event, LV_EVENT_CLICKED, this);
            auto* avatar = make_avatar(toolbar, *chat, 42);
            lv_obj_add_flag(avatar, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_add_event_cb(avatar, chat_info_event, LV_EVENT_CLICKED, this);
            lv_obj_align(avatar, LV_ALIGN_RIGHT_MID, 0, 0);
        }
    }

    if (chat_details_open_) {
        build_peer_details(conversation, true);
        return;
    }

    auto* messages = lv_obj_create(conversation);
    message_list_  = messages;
    lv_obj_set_name_static(messages, "telegram_message_list");
    lv_obj_set_width(messages, LV_PCT(100));
    lv_obj_set_flex_grow(messages, 1);
    lv_obj_set_style_radius(messages, 0, 0);
    lv_obj_set_style_border_width(messages, 0, 0);
    lv_obj_set_style_bg_color(messages, lv_color_hex(components::color_conversation_background), 0);
    lv_obj_set_style_bg_opa(messages, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(messages, wide_ ? 18 : 12, 0);
    lv_obj_set_style_pad_row(messages, 6, 0);
    lv_obj_set_flex_flow(messages, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(messages, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(messages, LV_SCROLLBAR_MODE_AUTO);

    auto* older =
        components::create_button(messages, LV_SYMBOL_UP "  Load Earlier Messages", older_clicked_event, this);
    lv_obj_set_name_static(older, "telegram_load_older");
    lv_obj_set_size(older, LV_PCT(100), 44);
    lv_obj_set_style_bg_opa(older, LV_OPA_TRANSP, 0);
    lv_obj_set_style_shadow_width(older, 0, 0);
    auto* older_label = lv_obj_get_child(older, 0);
    set_font(older_label, 14);
    lv_obj_set_style_text_color(older_label, lv_color_hex(components::color_action), 0);
    lv_obj_update_layout(messages);
    std::string previous_date;
    for (const auto& message : view_model_.model().messages()) {
        const auto date = date_text(message.date);
        if (!date.empty() && date != previous_date) {
            auto* date_row = lv_obj_create(messages);
            make_transparent(date_row);
            lv_obj_set_size(date_row, LV_PCT(100), 36);
            lv_obj_remove_flag(date_row, LV_OBJ_FLAG_SCROLLABLE);
            auto* label = make_label(date_row, date.c_str(), 14, components::color_outgoing_meta);
            lv_obj_set_name_static(label, "telegram_date_separator");
            lv_obj_set_style_bg_color(label, lv_color_hex(components::color_date_background), 0);
            lv_obj_set_style_bg_opa(label, LV_OPA_COVER, 0);
            lv_obj_set_style_radius(label, LV_RADIUS_CIRCLE, 0);
            lv_obj_set_style_pad_hor(label, 12, 0);
            lv_obj_set_style_pad_ver(label, 5, 0);
            lv_obj_center(label);
        }
        previous_date = date;
        build_message(messages, message);
    }
    if (view_model_.model().messages().empty()) {
        auto* empty = make_label(messages, "No messages", 22, components::color_text_secondary);
        lv_obj_set_style_margin_top(empty, 36, 0);
        lv_obj_set_style_align(empty, LV_ALIGN_TOP_MID, 0);
    }

    if (!view_model_.error_text().empty() || !media_status_.empty()) {
        const auto& issue = !view_model_.error_text().empty() ? view_model_.error_text() : media_status_;
        auto* warning     = make_label(conversation, issue.c_str(), 14, components::color_destructive);
        lv_obj_set_width(warning, LV_PCT(100));
        lv_obj_set_style_bg_color(warning, lv_color_hex(components::color_error_background), 0);
        lv_obj_set_style_bg_opa(warning, LV_OPA_COVER, 0);
        lv_obj_set_style_pad_hor(warning, 12, 0);
        lv_obj_set_style_pad_ver(warning, 6, 0);
        lv_label_set_long_mode(warning, LV_LABEL_LONG_DOT);
    }

    auto* composer_bar = lv_obj_create(conversation);
    lv_obj_set_name_static(composer_bar, "telegram_composer_bar");
    lv_obj_set_size(composer_bar, LV_PCT(100), 72);
    lv_obj_set_style_radius(composer_bar, 0, 0);
    lv_obj_set_style_border_width(composer_bar, 1, 0);
    lv_obj_set_style_border_side(composer_bar, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_border_color(composer_bar, lv_color_hex(components::color_border), 0);
    lv_obj_set_style_bg_color(composer_bar, lv_color_hex(components::color_page_background), 0);
    lv_obj_set_style_bg_opa(composer_bar, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(composer_bar, 8, 0);
    lv_obj_set_style_pad_column(composer_bar, 8, 0);
    lv_obj_clear_flag(composer_bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(composer_bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(composer_bar, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    composer_ = lv_textarea_create(composer_bar);
    lv_obj_set_name_static(composer_, "telegram_composer");
    lv_obj_set_height(composer_, 54);
    lv_obj_set_flex_grow(composer_, 1);
    lv_textarea_set_one_line(composer_, true);
    lv_textarea_set_max_length(composer_, kMaxComposerLength);
    lv_textarea_set_placeholder_text(composer_, "Message");
    lv_textarea_set_text(composer_, composer_draft_.c_str());
    set_font(composer_, 22);
    lv_obj_set_style_radius(composer_, 26, 0);
    lv_obj_set_style_pad_hor(composer_, 18, 0);
    lv_obj_set_style_pad_ver(composer_, 13, 0);
    lv_obj_set_style_border_width(composer_, 1, 0);
    lv_obj_set_style_border_color(composer_, lv_color_hex(components::color_border), 0);
    lv_obj_add_event_cb(composer_, input_focused_event, LV_EVENT_FOCUSED, this);
    lv_obj_add_event_cb(composer_, composer_changed_event, LV_EVENT_VALUE_CHANGED, this);

    auto* send   = components::create_button(composer_bar, LV_SYMBOL_UP, send_clicked_event, this);
    send_button_ = send;
    lv_obj_set_name_static(send, "telegram_send");
    lv_obj_set_size(send, 48, 48);
    lv_obj_set_style_radius(send, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_shadow_width(send, 0, 0);
    lv_obj_set_style_bg_color(send, lv_color_hex(components::color_muted_badge), LV_STATE_DISABLED);
    if (composer_draft_.empty()) lv_obj_add_state(send, LV_STATE_DISABLED);

    lv_obj_update_layout(messages);
    switch (scroll_restore_mode_) {
        case ScrollRestoreMode::Position:
            lv_obj_scroll_to_y(messages, saved_scroll_y_, LV_ANIM_OFF);
            break;
        case ScrollRestoreMode::BottomDistance: {
            const auto maximum_scroll = lv_obj_get_scroll_bottom(messages);
            lv_obj_scroll_to_y(messages, std::max<std::int32_t>(0, maximum_scroll - saved_scroll_bottom_), LV_ANIM_OFF);
            break;
        }
        case ScrollRestoreMode::Bottom:
            lv_obj_scroll_to_y(messages, LV_COORD_MAX, LV_ANIM_OFF);
            break;
    }
}

void TelegramView::build_message(lv_obj_t* parent, const telegram::Message& message)
{
    const auto* chat = view_model_.model().current_chat();
    const bool show_sender_name = !message.is_outgoing && !message.sender_name.empty() &&
                                  !(chat && chat->is_private);
    if (message.file.id > 0 && message.file.is_downloaded && !message.file.local_path.empty()) {
        requested_downloads_.erase(message.file.id);
        download_failures_.erase(message.file.id);
        download_retry_at_.erase(message.file.id);
    }

    auto* row = lv_obj_create(parent);
    lv_obj_set_name_static(row, "telegram_message_row");
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    make_transparent(row);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, message.is_outgoing ? LV_FLEX_ALIGN_END : LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);
    const auto avatar_size = wide_ ? 40 : 36;
    const auto avatar_space = message.is_outgoing ? 0 : avatar_size + 8;
    if (!message.is_outgoing) {
        telegram::Chat sender;
        sender.id = message.sender_id;
        sender.title = message.sender_name;
        sender.photo = message.sender_photo;
        auto* avatar = make_avatar(row, sender, avatar_size);
        lv_obj_set_name_static(avatar, "telegram_sender_avatar");
        lv_obj_set_style_pad_column(row, 8, 0);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_START);
    }

    auto* bubble = lv_obj_create(row);
    lv_obj_set_name_static(bubble, "telegram_message_bubble");
    const auto content_width = lv_obj_get_content_width(parent);
    const auto maximum_width = std::max<std::int32_t>(140, std::min<std::int32_t>(
        std::min<std::int32_t>(560, content_width * 86 / 100), content_width - avatar_space));
    std::int32_t bubble_width = maximum_width;
    if (message.kind == telegram::MessageKind::Photo) bubble_width = std::min<std::int32_t>(maximum_width, 360);
    if (message.kind == telegram::MessageKind::Text) {
        const auto* font = lilygo_ui_font_get(view_model_.preferences().message_font_size);
        lv_point_t text_size{};
        if (font) lv_text_get_size(&text_size, message.text.c_str(), font, 0, 0, maximum_width - 24, LV_TEXT_FLAG_NONE);
        bubble_width = std::clamp<std::int32_t>(text_size.x + 24, 140, maximum_width);
        if (show_sender_name) {
            lv_point_t sender_size{};
            const auto* sender_font = lilygo_ui_font_get(14);
            if (sender_font)
                lv_text_get_size(&sender_size, message.sender_name.c_str(), sender_font, 0, 0, maximum_width - 24,
                                 LV_TEXT_FLAG_NONE);
            bubble_width = std::min(maximum_width, std::max(bubble_width, sender_size.x + 24));
        }
    }
    lv_obj_set_width(bubble, bubble_width);
    lv_obj_set_style_max_width(bubble, LV_PCT(86), 0);
    lv_obj_set_height(bubble, LV_SIZE_CONTENT);
    lv_obj_set_style_radius(bubble, 16, 0);
    lv_obj_set_style_border_width(bubble, 0, 0);
    lv_obj_set_style_border_color(bubble, lv_color_hex(components::color_border), 0);
    lv_obj_set_style_bg_color(
        bubble,
        lv_color_hex(message.is_outgoing ? components::color_outgoing_message : components::color_container_background),
        0);
    lv_obj_set_style_bg_opa(bubble, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(bubble, 12, 0);
    lv_obj_set_style_pad_row(bubble, 4, 0);
    lv_obj_clear_flag(bubble, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(bubble, LV_FLEX_FLOW_COLUMN);

    if (show_sender_name) {
        auto* sender = make_label(bubble, message.sender_name.c_str(), 14, components::color_action);
        lv_obj_set_width(sender, LV_PCT(100));
        lv_label_set_long_mode(sender, LV_LABEL_LONG_DOT);
    }

    if (message.kind == telegram::MessageKind::Photo) {
        bool rendered = false;
        if (message.file.is_downloaded && !message.file.local_path.empty()) {
            const auto extension = lowercase_extension(message.file.local_path);
            auto cached          = image_cache_.find(message.file.id);
            if (cached != image_cache_.end()) {
                auto* image = lv_image_create(bubble);
                lv_obj_set_name_static(image, "telegram_photo");
                lv_obj_set_size(image, LV_PCT(100), wide_ ? 160 : 220);
                lv_image_set_src(image, &cached->second->descriptor);
                lv_image_set_inner_align(image, LV_IMAGE_ALIGN_CONTAIN);
                rendered = true;
            } else if (extension == ".png") {
                std::string validation_error;
                const bool valid = png_is_bounded(message.file.local_path, &validation_error);
                if (valid) {
                    file_sources_.push_back("A:" + message.file.local_path);
                    auto* image = lv_image_create(bubble);
                    lv_obj_set_name_static(image, "telegram_photo");
                    lv_obj_set_size(image, LV_PCT(100), wide_ ? 160 : 220);
                    lv_image_set_src(image, file_sources_.back().c_str());
                    lv_image_set_inner_align(image, LV_IMAGE_ALIGN_CONTAIN);
                    rendered = true;
                } else {
                    media_status_ = std::move(validation_error);
                }
            }
        }
        if (!rendered) {
            auto* placeholder = lv_obj_create(bubble);
            lv_obj_set_size(placeholder, LV_PCT(100), 170);
            lv_obj_set_style_radius(placeholder, 6, 0);
            lv_obj_set_style_border_width(placeholder, 0, 0);
            lv_obj_set_style_bg_color(placeholder, lv_color_hex(components::color_selection), 0);
            lv_obj_set_style_bg_opa(placeholder, LV_OPA_COVER, 0);
            lv_obj_clear_flag(placeholder, LV_OBJ_FLAG_SCROLLABLE);
            if (message.file.id > 0 && !message.file.is_downloaded) {
                auto action = std::make_unique<AccountUiAction>();
                action->run = [this, id = message.file.id] {
                    download_failures_.erase(id);
                    download_retry_at_.erase(id);
                    if (requested_downloads_.insert(id).second && !view_model_.request_file(id, 12))
                        requested_downloads_.erase(id);
                    media_changed_ = true;
                };
                auto* download = make_icon_button(placeholder, "telegram_photo_download", LV_SYMBOL_DOWNLOAD,
                                                  account_ui_event, action.get());
                if (requested_downloads_.count(message.file.id)) lv_obj_add_state(download, LV_STATE_DISABLED);
                lv_obj_center(download);
                account_actions_.push_back(std::move(action));
            } else {
                auto* icon = make_label(placeholder, LV_SYMBOL_IMAGE, 36, components::color_action);
                lv_obj_center(icon);
            }
        }
        photo_contexts_.push_back(PhotoContext{bubble, message});
    } else if (message.kind == telegram::MessageKind::Voice) {
        auto context       = std::make_unique<VoiceContext>();
        context->view      = this;
        context->message   = message;
        auto* voice_button = lv_button_create(bubble);
        lv_obj_set_name_static(voice_button, "telegram_voice");
        lv_obj_set_size(voice_button, LV_PCT(100), 52);
        lv_obj_set_style_radius(voice_button, 8, 0);
        lv_obj_set_style_shadow_width(voice_button, 0, 0);
        lv_obj_set_style_bg_color(voice_button, lv_color_hex(components::color_selection), 0);
        lv_obj_set_style_bg_opa(voice_button, LV_OPA_COVER, 0);
        lv_obj_set_style_pad_hor(voice_button, 14, 0);
        lv_obj_set_style_pad_ver(voice_button, 0, 0);
        lv_obj_set_style_pad_column(voice_button, 12, 0);
        lv_obj_set_flex_flow(voice_button, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(voice_button, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_add_event_cb(voice_button, voice_clicked_event, LV_EVENT_CLICKED, context.get());

        const bool active = voice_identity(message) == active_voice_identity_ &&
                            (voice_state_ == telegram::media::VoicePlaybackState::opening ||
                             voice_state_ == telegram::media::VoicePlaybackState::playing);
        make_label(voice_button,
                   active ? LV_SYMBOL_STOP : (message.file.is_downloaded ? LV_SYMBOL_PLAY : LV_SYMBOL_DOWNLOAD), 22,
                   components::color_action);
        auto* voice_title =
            make_label(voice_button, active ? "Playing voice" : "Voice message", 14, components::color_container_title);
        lv_obj_set_flex_grow(voice_title, 1);
        lv_obj_set_height(voice_title, 20);
        lv_label_set_long_mode(voice_title, LV_LABEL_LONG_DOT);
        const auto duration = duration_text(message.duration_seconds);
        make_label(voice_button, duration.c_str(), 14, components::color_text_secondary);
        voice_contexts_.push_back(std::move(context));
    } else if (message.kind == telegram::MessageKind::Unsupported) {
        auto* unsupported = make_label(bubble, message.text.empty() ? "Unsupported message" : message.text.c_str(), 14,
                                       components::color_text_secondary);
        lv_obj_set_width(unsupported, LV_PCT(100));
    }

    if (message.kind != telegram::MessageKind::Unsupported && !message.text.empty()) {
        auto* text = make_label(bubble, message.text.c_str(), view_model_.preferences().message_font_size, components::color_text);
        lv_obj_set_name_static(text, "telegram_message_text");
        lv_obj_set_width(text, LV_PCT(100));
        lv_label_set_long_mode(text, LV_LABEL_LONG_WRAP);
    }

    std::string meta = format_time(message.date);
    if (message.is_sending)
        meta += "  " LV_SYMBOL_REFRESH;
    else if (message.is_failed)
        meta += "  " LV_SYMBOL_WARNING;
    else if (message.is_outgoing)
        meta += "  " LV_SYMBOL_OK;
    auto* timestamp = make_label(bubble, meta.c_str(), 14,
                                 message.is_failed     ? components::color_destructive
                                 : message.is_outgoing ? components::color_outgoing_meta
                                                       : components::color_text_secondary);
    lv_obj_set_width(timestamp, LV_PCT(100));
    lv_obj_set_style_text_align(timestamp, LV_TEXT_ALIGN_RIGHT, 0);
}

void TelegramView::show_keyboard(lv_obj_t* textarea)
{
    if (!keyboard_ || !textarea) return;
    lv_keyboard_set_textarea(keyboard_, textarea);
    const auto state       = view_model_.model().auth_state();
    const bool number_mode = textarea == auth_input_ && state == telegram::AuthState::WaitingForEmailCode;
    lv_keyboard_set_mode(keyboard_, number_mode ? LV_KEYBOARD_MODE_NUMBER : LV_KEYBOARD_MODE_TEXT_LOWER);
    const auto height = lv_obj_get_content_height(surface_);
    lv_obj_set_height(keyboard_, height > 0 && height < 700 ? LV_PCT(34) : LV_PCT(42));
    set_hidden(keyboard_, false);
    if (textarea == auth_input_ && body_) {
        lv_obj_set_flex_align(body_, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_scroll_to_view_recursive(textarea, LV_ANIM_OFF);
    } else if (textarea != composer_) {
        lv_obj_scroll_to_view_recursive(textarea, LV_ANIM_OFF);
    }
}

void TelegramView::hide_keyboard()
{
    if (!keyboard_) return;
    lv_keyboard_set_textarea(keyboard_, nullptr);
    set_hidden(keyboard_, true);
    if (body_ && view_model_.model().auth_state() != telegram::AuthState::Ready) {
        lv_obj_set_flex_align(body_, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    }
}

void TelegramView::submit_auth()
{
    if (!auth_input_) return;
    auth_draft_ = lv_textarea_get_text(auth_input_);
    if (view_model_.submit_auth_input(auth_draft_)) {
        auth_draft_.clear();
        lv_textarea_set_text(auth_input_, "");
        hide_keyboard();
    }
}

void TelegramView::send_message()
{
    if (!composer_) return;
    composer_draft_ = lv_textarea_get_text(composer_);
    if (view_model_.send_text(composer_draft_)) {
        view_model_.set_draft(view_model_.model().current_chat_id(), "");
        composer_draft_.clear();
        lv_textarea_set_text(composer_, "");
        hide_keyboard();
    }
}

void TelegramView::play_voice(const telegram::Message& message)
{
    if (!message.file.is_downloaded || message.file.local_path.empty()) {
        download_failures_.erase(message.file.id);
        download_retry_at_.erase(message.file.id);
        if (message.file.id > 0 && requested_downloads_.insert(message.file.id).second) {
            if (!view_model_.request_file(message.file.id, 24)) requested_downloads_.erase(message.file.id);
        }
        media_status_  = "Downloading voice message";
        media_changed_ = true;
        return;
    }

    requested_downloads_.erase(message.file.id);
    download_failures_.erase(message.file.id);
    download_retry_at_.erase(message.file.id);

    const VoiceIdentity identity = voice_identity(message);
    if (active_voice_identity_ == identity && (voice_state_ == telegram::media::VoicePlaybackState::opening ||
                                               voice_state_ == telegram::media::VoicePlaybackState::playing)) {
        voice_player_.stop();
        voice_state_   = telegram::media::VoicePlaybackState::stopping;
        media_changed_ = true;
        return;
    }

    std::string error;
    const auto request_id = voice_player_.play_file(
        message.file.local_path,
        [this, identity](const telegram::media::VoicePlaybackEvent& event) {
            voice_results_.try_push(VoiceResult{identity, event});
        },
        &error);
    if (request_id == 0) {
        media_status_ = std::move(error);
        if (active_voice_request_id_ == 0) voice_state_ = telegram::media::VoicePlaybackState::error;
    } else {
        active_voice_identity_   = identity;
        active_voice_request_id_ = request_id;
        voice_state_             = telegram::media::VoicePlaybackState::opening;
    }
    media_changed_ = true;
}

TelegramView::VoiceIdentity TelegramView::voice_identity(const telegram::Message& message) noexcept
{
    return {message.chat_id, message.id};
}

bool TelegramView::voice_result_matches(const VoiceResult& result, const VoiceIdentity& active_identity,
                                        telegram::media::VoiceRequestId active_request_id) noexcept
{
    return active_request_id != 0 && result.identity == active_identity && result.event.request_id == active_request_id;
}

void TelegramView::request_visible_photos()
{
    if (!message_list_) return;
    lv_area_t viewport{};
    lv_obj_get_coords(message_list_, &viewport);
    std::size_t visible_count = 0;
    for (auto iterator = photo_contexts_.rbegin(); iterator != photo_contexts_.rend(); ++iterator) {
        lv_area_t area{};
        lv_obj_get_coords(iterator->bubble, &area);
        if (area.y2 < viewport.y1 || area.y1 > viewport.y2) continue;
        request_photo(iterator->message);
        if (++visible_count == kMaxCachedImages) break;
    }
}

void TelegramView::request_visible_avatars()
{
    const auto now = lv_tick_get();
    std::size_t downloads = 0;
    for (auto& pair : avatar_cache_) {
        auto& entry = pair.second;
        // TDLib may wait out server flood limits; keep its in-flight slot until completion.
        if (entry.downloading && !view_model_.background_download_pending(pair.first)) entry.downloading = false;
        if (entry.downloading) ++downloads;
    }
    std::size_t decoded = 0;
    for (const auto& context : avatar_contexts_) {
        if (!lv_obj_is_visible(context.object)) continue;
        const auto& file = context.file;
        auto found = avatar_cache_.find(file.id);
        if (found == avatar_cache_.end()) {
            if (avatar_cache_.size() >= kMaxCachedAvatars) {
                media_changed_ = true;
                continue;
            }
            found = avatar_cache_.try_emplace(file.id).first;
        }
        auto& entry = found->second;
        entry.last_used = now;
        if (!file.is_downloaded || file.local_path.empty()) {
            if (entry.downloading || file.is_downloading || downloads >= 3 ||
                static_cast<std::int32_t>(now - entry.retry_at) < 0) continue;
            if (view_model_.request_file(file.id, 8, true)) {
                entry.downloading = true;
                ++downloads;
            } else {
                if (entry.failures < 2) ++entry.failures;
                entry.retry_at = now + (entry.failures >= 2 ? 300000U : 60000U);
            }
            continue;
        }
        if (entry.downloading) {
            entry.downloading = false;
            if (downloads > 0) --downloads;
        }
        if (entry.path == file.local_path && entry.image) continue;
        if (entry.requested_path != file.local_path) {
            entry.requested_path = file.local_path;
            entry.failures = 0;
            entry.retry_at = 0;
            entry.decoding = false;
        }
        if (entry.decoding || entry.failures >= 2 || decoded >= 2 ||
            static_cast<std::int32_t>(now - entry.retry_at) < 0) continue;
        const auto extension = lowercase_extension(file.local_path);
        entry.decoding = true;
        ++decoded;
        if (extension == ".png") {
            auto result = telegram::ui::decode_png_avatar_with_lvgl(file.local_path);
            pending_avatars_.push_back({file.id, avatar_generation_, std::move(result)});
            media_changed_ = true;
            continue;
        }
#if defined(TELEGRAM_HAS_LVGL_TJPGD)
        if (is_jpeg_extension(extension)) {
            auto result = telegram::ui::decode_jpeg_with_lvgl(file.local_path, kAvatarDimension);
            pending_avatars_.push_back({file.id, avatar_generation_, std::move(result)});
            media_changed_ = true;
            continue;
        }
#endif
        telegram::media::ImageDecodeOptions options;
        options.max_width = kAvatarDimension;
        options.max_height = kAvatarDimension;
        options.max_output_bytes = kAvatarDimension * kAvatarDimension * 4U;
        std::string error;
        const bool submitted = avatar_decoder_.submit_file(file.local_path, options,
            [this, id = file.id, generation = avatar_generation_](telegram::media::ImageDecodeResult result) {
                avatar_results_.try_push({id, generation, std::move(result)});
            }, nullptr, &error);
        if (!submitted) {
            entry.decoding = false;
            if (avatar_decoder_.outstanding() < 2) ++entry.failures;
            entry.retry_at = now + 1000U;
        }
    }
}

void TelegramView::integrate_avatar_results()
{
    for (auto& pending : pending_avatars_) {
        if (pending.generation != avatar_generation_) continue;
        const auto found = avatar_cache_.find(pending.file_id);
        if (found == avatar_cache_.end() || found->second.requested_path != pending.result.source_path) continue;
        auto& entry = found->second;
        entry.decoding = false;
        if (!pending.result.error.empty()) {
            if (entry.failures < 2) ++entry.failures;
            entry.retry_at = lv_tick_get() + 5000U;
            continue;
        }
        if (pending.result.image) {
            const auto& pixels = *pending.result.image;
            if (pixels.width == 0 || pixels.height == 0 || pixels.width > kAvatarDimension ||
                pixels.height > kAvatarDimension || pixels.stride != pixels.width * 4U ||
                pixels.bgra.size() != pixels.stride * pixels.height) {
                entry.failures = 2;
                continue;
            }
            auto cached = std::make_unique<CachedImage>();
            cached->pixels = std::move(pending.result.image);
            cached->descriptor.header.magic = LV_IMAGE_HEADER_MAGIC;
            cached->descriptor.header.cf = LV_COLOR_FORMAT_ARGB8888;
            cached->descriptor.header.w = static_cast<std::uint16_t>(pixels.width);
            cached->descriptor.header.h = static_cast<std::uint16_t>(pixels.height);
            cached->descriptor.header.stride = static_cast<std::uint16_t>(pixels.stride);
            cached->descriptor.data_size = static_cast<std::uint32_t>(pixels.bgra.size());
            cached->descriptor.data = pixels.bgra.data();
            entry.image = std::move(cached);
        } else {
            entry.failures = 2;
            continue;
        }
        entry.path = pending.result.source_path;
        entry.failures = 0;
    }
    pending_avatars_.clear();
}

void TelegramView::trim_avatar_cache(const std::unordered_set<std::int32_t>& visible)
{
    // Called after deleting old widgets so their image descriptors remain valid until then.
    while (avatar_cache_.size() >= kMaxCachedAvatars) {
        auto oldest = avatar_cache_.end();
        for (auto it = avatar_cache_.begin(); it != avatar_cache_.end(); ++it) {
            if (visible.count(it->first)) continue;
            if (oldest == avatar_cache_.end() ||
                static_cast<std::int32_t>(it->second.last_used - oldest->second.last_used) < 0) oldest = it;
        }
        if (oldest == avatar_cache_.end()) break;
        avatar_cache_.erase(oldest);
    }
}

void TelegramView::request_photo(const telegram::Message& message)
{
    if (message.file.id <= 0) return;
    if (!message.file.is_downloaded || message.file.local_path.empty()) {
        if (!view_model_.preferences().auto_download_photos) return;
        const auto failures = download_failures_.find(message.file.id);
        if (download_retry_at_.count(message.file.id) != 0 ||
            (failures != download_failures_.end() && failures->second >= 2)) {
            return;
        }
        if (requested_downloads_.insert(message.file.id).second && !view_model_.request_file(message.file.id, 12)) {
            requested_downloads_.erase(message.file.id);
        }
        return;
    }
    requested_downloads_.erase(message.file.id);
    download_failures_.erase(message.file.id);
    download_retry_at_.erase(message.file.id);
    const auto extension = lowercase_extension(message.file.local_path);
    const auto failures  = image_failures_.find(message.file.id);
    if (image_retry_at_.count(message.file.id) != 0 || (failures != image_failures_.end() && failures->second >= 2)) {
        return;
    }
    if (extension == ".png" || image_cache_.count(message.file.id) != 0 ||
        requested_images_.size() >= kMaxCachedImages || !requested_images_.insert(message.file.id).second) {
        return;
    }

#if defined(TELEGRAM_HAS_LVGL_TJPGD)
    if (is_jpeg_extension(extension)) {
        std::string error;
        if (!jpeg_is_bounded(message.file.local_path, &error)) {
            schedule_image_retry(message.file.id, std::move(error));
            return;
        }
        // Assemble decoder tiles before LVGL scales the image; tiled transforms corrupt JPEGs.
        auto result = telegram::ui::decode_jpeg_with_lvgl(message.file.local_path);
        if (!image_results_.try_push(ImageResult{message.file.id, std::move(result)}))
            schedule_image_retry(message.file.id, "Photo decode queue is full");
        return;
    }
#endif

    telegram::media::ImageDecodeOptions options;
    options.max_width        = 720;
    options.max_height       = kMaxImageDimension;
    options.max_output_bytes = 4U * 1024U * 1024U;
    std::string error;
    const bool submitted = image_decoder_.submit_file(
        message.file.local_path, options,
        [this, file_id = message.file.id](telegram::media::ImageDecodeResult result) {
            image_results_.try_push(ImageResult{file_id, std::move(result)});
        },
        nullptr, &error);
    if (!submitted) {
        schedule_image_retry(message.file.id, std::move(error));
    }
}

void TelegramView::schedule_image_retry(std::int32_t file_id, std::string error)
{
    media_status_ = std::move(error);
    requested_images_.erase(file_id);
    auto& failures = image_failures_[file_id];
    if (failures < std::numeric_limits<std::uint8_t>::max()) ++failures;
    if (failures < 2) image_retry_at_[file_id] = lv_tick_get() + 2000U;
    media_changed_ = true;
}

void TelegramView::integrate_media_results()
{
    for (auto& pending : pending_images_) {
        if (requested_images_.erase(pending.file_id) == 0) continue;
        if (!pending.result.ok()) {
            schedule_image_retry(pending.file_id, std::move(pending.result.error));
            continue;
        }

        auto cached    = std::make_unique<CachedImage>();
        cached->pixels = std::move(pending.result.image);
        if (!cached->pixels || cached->pixels->width == 0 || cached->pixels->height == 0 ||
            cached->pixels->bgra.empty() || cached->pixels->stride > std::numeric_limits<std::uint16_t>::max() ||
            cached->pixels->width > std::numeric_limits<std::uint16_t>::max() ||
            cached->pixels->height > std::numeric_limits<std::uint16_t>::max() ||
            cached->pixels->bgra.size() > std::numeric_limits<std::uint32_t>::max()) {
            schedule_image_retry(pending.file_id, "Decoded photo has an unsupported size");
            continue;
        }
        cached->descriptor.header.magic  = LV_IMAGE_HEADER_MAGIC;
        cached->descriptor.header.cf     = LV_COLOR_FORMAT_ARGB8888;
        cached->descriptor.header.flags  = 0;
        cached->descriptor.header.w      = static_cast<std::uint16_t>(cached->pixels->width);
        cached->descriptor.header.h      = static_cast<std::uint16_t>(cached->pixels->height);
        cached->descriptor.header.stride = static_cast<std::uint16_t>(cached->pixels->stride);
        cached->descriptor.data_size     = static_cast<std::uint32_t>(cached->pixels->bgra.size());
        cached->descriptor.data          = cached->pixels->bgra.data();

        const auto existing = image_cache_.find(pending.file_id);
        if (existing != image_cache_.end()) {
            image_cache_bytes_ -= existing->second->pixels->bgra.size();
            image_cache_.erase(existing);
            image_cache_order_.erase(std::remove(image_cache_order_.begin(), image_cache_order_.end(), pending.file_id),
                                     image_cache_order_.end());
        }
        image_cache_bytes_ += cached->pixels->bgra.size();
        image_cache_[pending.file_id] = std::move(cached);
        image_cache_order_.push_back(pending.file_id);
        image_failures_.erase(pending.file_id);
        image_retry_at_.erase(pending.file_id);
        media_status_.clear();
    }
    pending_images_.clear();
    trim_image_cache();
}

void TelegramView::trim_image_cache()
{
    while ((!image_cache_order_.empty()) &&
           (image_cache_.size() > kMaxCachedImages || image_cache_bytes_ > kMaxImageCacheBytes)) {
        const auto file_id = image_cache_order_.front();
        image_cache_order_.pop_front();
        const auto item = image_cache_.find(file_id);
        if (item == image_cache_.end()) continue;
        image_cache_bytes_ -= item->second->pixels->bgra.size();
        image_cache_.erase(item);
        requested_images_.erase(file_id);
    }
}

void TelegramView::clear_dynamic_contexts()
{
    chat_contexts_.clear();
    voice_contexts_.clear();
    photo_contexts_.clear();
    avatar_contexts_.clear();
    account_actions_.clear();
    account_inputs_.clear();
    file_sources_.clear();
}

void TelegramView::update_responsive_state()
{
    if (!surface_) return;
    const auto width = lv_obj_get_content_width(surface_);
    if (width > 0) wide_ = width >= kWideBreakpoint;
}

bool TelegramView::is_wide() const noexcept
{
    return wide_;
}
