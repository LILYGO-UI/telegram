#include "pages/telegram/telegram_view.hpp"

#include "app_identity.h"
#include "components/components.hpp"

#include <cm0/typography.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <ctime>
#include <utility>

namespace {

constexpr const char* kUserIcon = "\xef\x80\x87";
constexpr const char* kBellIcon = "\xef\x83\xb3";
constexpr const char* kLockIcon = "\xef\x80\xa3";
constexpr const char* kPhoneIcon = "\xef\x84\x8b";
constexpr const char* kBookmarkIcon = "\xef\x80\xae";
constexpr const char* kBrushIcon = "\xef\x87\xbc";
constexpr const char* kInfoIcon = "\xef\x81\x9a";

void account_font(lv_obj_t* object, std::uint32_t size)
{
    if (const auto* font = lilygo_ui_font_get(size)) lv_obj_set_style_text_font(object, font, 0);
}

void account_plain(lv_obj_t* object)
{
    lv_obj_set_style_border_width(object, 0, 0);
    lv_obj_set_style_radius(object, 0, 0);
    lv_obj_set_style_bg_opa(object, LV_OPA_TRANSP, 0);
    lv_obj_set_style_shadow_width(object, 0, 0);
    lv_obj_set_style_pad_all(object, 0, 0);
    lv_obj_set_style_pad_row(object, 0, 0);
    lv_obj_set_style_pad_column(object, 0, 0);
}

lv_obj_t* account_label(lv_obj_t* parent, const std::string& text, std::uint32_t size = 18,
                        std::uint32_t color = components::color_text)
{
    auto* label = lv_label_create(parent);
    account_font(label, size);
    lv_label_set_text(label, text.c_str());
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    return label;
}

void account_section(lv_obj_t* parent, const char* title)
{
    auto* label = account_label(parent, title, 14, components::color_text_secondary);
    lv_obj_set_width(label, LV_PCT(100));
    lv_obj_set_style_pad_left(label, 20, 0);
    lv_obj_set_style_pad_right(label, 20, 0);
    lv_obj_set_style_pad_top(label, 24, 0);
    lv_obj_set_style_pad_bottom(label, 10, 0);
}

void account_message(lv_obj_t* parent, const char* name, const std::string& message,
                     std::uint32_t color = components::color_text_secondary)
{
    auto* label = account_label(parent, message, 14, color);
    lv_obj_set_name_static(label, name);
    lv_obj_set_width(label, LV_PCT(100));
    lv_obj_set_style_pad_hor(label, 20, 0);
    lv_obj_set_style_pad_ver(label, 12, 0);
}

std::string account_phone_number(const std::string& phone_number)
{
    return phone_number.empty() || phone_number.front() == '+' ? phone_number : "+" + phone_number;
}

std::string account_bytes(std::int64_t bytes)
{
    if (bytes <= 0) return "0 B";
    static constexpr std::array<const char*, 4> units{"B", "KB", "MB", "GB"};
    double value = static_cast<double>(bytes);
    std::size_t index = 0;
    while (value >= 1024 && index + 1 < units.size()) {
        value /= 1024;
        ++index;
    }
    char result[48]{};
    std::snprintf(result, sizeof(result), index == 0 ? "%.0f %s" : "%.1f %s", value, units[index]);
    return result;
}

std::string account_date(std::int64_t timestamp)
{
    if (timestamp <= 0) return {};
    const auto raw = static_cast<std::time_t>(timestamp);
    std::tm date{};
    localtime_r(&raw, &date);
    char text[48]{};
    std::strftime(text, sizeof(text), "%d %b %Y, %H:%M", &date);
    return text;
}

const char* privacy_title(telegram::PrivacyVisibility visibility)
{
    switch (visibility) {
        case telegram::PrivacyVisibility::Everyone: return "Everybody";
        case telegram::PrivacyVisibility::Contacts: return "My Contacts";
        case telegram::PrivacyVisibility::Nobody: return "Nobody";
        case telegram::PrivacyVisibility::Custom: return "Custom";
    }
    return "Custom";
}

telegram::AccountRequest account_request(telegram::AccountAction action)
{
    telegram::AccountRequest request;
    request.action = action;
    return request;
}

}  // namespace

void TelegramView::account_ui_event(lv_event_t* event)
{
    auto* context = static_cast<AccountUiAction*>(lv_event_get_user_data(event));
    if (context && context->run) {
        const auto action = context->run;
        action();
    }
}

void TelegramView::account_confirm_event(lv_event_t* event)
{
    auto* view = static_cast<TelegramView*>(lv_event_get_user_data(event));
    if (!view) return;
    auto action = std::move(view->pending_account_confirmation_);
    account_cancel_event(event);
    if (action) action();
}

void TelegramView::account_cancel_event(lv_event_t* event)
{
    auto* view = static_cast<TelegramView*>(lv_event_get_user_data(event));
    if (!view) return;
    view->pending_account_confirmation_ = {};
    if (view->logout_dialog_) {
        lv_obj_delete_async(view->logout_dialog_);
        view->logout_dialog_ = nullptr;
    }
}

lv_obj_t* TelegramView::account_row(lv_obj_t* parent, const char* name, const char* title,
                                    const std::string& detail, const char* icon, std::function<void()> action)
{
    auto* row = lv_obj_create(parent);
    lv_obj_set_name_static(row, name);
    account_plain(row);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_style_min_height(row, detail.empty() ? 60 : 76, 0);
    lv_obj_set_style_pad_hor(row, 20, 0);
    lv_obj_set_style_pad_ver(row, 14, 0);
    lv_obj_set_style_pad_column(row, 14, 0);
    lv_obj_set_style_bg_color(row, lv_color_hex(components::color_container_background), 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(row, 1, 0);
    lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_color(row, lv_color_hex(components::color_border), 0);
    lv_obj_set_style_border_opa(row, LV_OPA_30, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    if (icon && *icon) {
        auto* symbol = account_label(row, icon, 22, components::color_action);
        lv_obj_set_width(symbol, 28);
        lv_obj_set_style_text_align(symbol, LV_TEXT_ALIGN_CENTER, 0);
    }
    auto* text = lv_obj_create(row);
    account_plain(text);
    lv_obj_set_width(text, 0);
    lv_obj_set_height(text, LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(text, 1);
    lv_obj_set_flex_flow(text, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(text, 5, 0);
    lv_obj_remove_flag(text, static_cast<lv_obj_flag_t>(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));
    auto* title_label = account_label(text, title);
    lv_obj_set_width(title_label, LV_PCT(100));
    if (!detail.empty()) {
        auto* value = account_label(text, detail, 14, components::color_text_secondary);
        lv_obj_set_width(value, LV_PCT(100));
    }
    if (action) {
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_bg_color(row, lv_color_hex(components::color_selection), LV_STATE_PRESSED);
        auto context = std::make_unique<AccountUiAction>();
        context->run = std::move(action);
        lv_obj_add_event_cb(row, account_ui_event, LV_EVENT_CLICKED, context.get());
        account_actions_.push_back(std::move(context));
        auto* chevron = account_label(row, LV_SYMBOL_RIGHT, 14, components::color_text_secondary);
        lv_obj_set_width(chevron, 16);
    } else {
        lv_obj_remove_flag(row, LV_OBJ_FLAG_CLICKABLE);
    }
    return row;
}

void TelegramView::confirm_account_action(const char* title, const char* text, std::function<void()> action)
{
    if (logout_dialog_ || !surface_) return;
    hide_keyboard();
    pending_account_confirmation_ = std::move(action);
    auto* overlay = lv_obj_create(surface_);
    logout_dialog_ = overlay;
    lv_obj_set_name_static(overlay, "telegram_account_dialog");
    account_plain(overlay);
    lv_obj_add_flag(overlay, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_set_size(overlay, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(overlay, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(overlay, LV_OPA_40, 0);
    lv_obj_set_flex_flow(overlay, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(overlay, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    auto* panel = components::create_card(overlay);
    lv_obj_set_width(panel, LV_PCT(88));
    lv_obj_set_style_max_width(panel, 440, 0);
    lv_obj_set_height(panel, LV_SIZE_CONTENT);
    lv_obj_set_style_max_height(panel, LV_PCT(94), 0);
    lv_obj_set_style_pad_all(panel, 24, 0);
    lv_obj_set_style_pad_row(panel, 16, 0);
    lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
    auto* heading = account_label(panel, title, 22);
    lv_obj_set_width(heading, LV_PCT(100));
    auto* description = account_label(panel, text, 18, components::color_text_secondary);
    lv_obj_set_width(description, LV_PCT(100));
    auto* confirm = components::create_button(panel, "Confirm", account_confirm_event, this);
    lv_obj_set_name_static(confirm, "telegram_account_confirm");
    lv_obj_set_width(confirm, LV_PCT(100));
    lv_obj_set_style_min_height(confirm, 48, 0);
    lv_obj_set_style_bg_color(confirm, lv_color_hex(components::color_destructive), 0);
    auto* cancel = components::create_button(panel, "Cancel", account_cancel_event, this);
    lv_obj_set_name_static(cancel, "telegram_account_cancel");
    lv_obj_set_width(cancel, LV_PCT(100));
    lv_obj_set_style_min_height(cancel, 48, 0);
}

const char* TelegramView::account_page_title() const
{
    switch (account_page_) {
        case AccountPage::Settings: return "Settings";
        case AccountPage::Profile: return "My Profile";
        case AccountPage::EditName: return "Edit Name";
        case AccountPage::EditUsername: return "Username";
        case AccountPage::EditBio: return "Bio";
        case AccountPage::Notifications: return "Notifications and Sounds";
        case AccountPage::PrivateNotifications: return "Private Chats";
        case AccountPage::GroupNotifications: return "Groups";
        case AccountPage::ChannelNotifications: return "Channels";
        case AccountPage::Privacy: return "Privacy and Security";
        case AccountPage::PhonePrivacy: return "Phone Number";
        case AccountPage::LastSeenPrivacy: return "Last Seen and Online";
        case AccountPage::PhotoPrivacy: return "Profile Photos";
        case AccountPage::Devices: return "Devices";
        case AccountPage::Blocked: return "Blocked Users";
        case AccountPage::Storage: return "Storage Usage";
        case AccountPage::Appearance: return "Appearance";
        case AccountPage::About: return "About";
        case AccountPage::AddContact: return "New Contact";
        case AccountPage::ContactDetails: return "Contact";
    }
    return "Settings";
}

void TelegramView::show_account_page(AccountPage page)
{
    hide_keyboard();
    pending_account_chat_open_ = false;
    account_inputs_.clear();
    account_form_drafts_.clear();
    view_model_.clear_account_feedback();
    account_page_ = page;
    media_changed_ = true;
    auto request = account_request(telegram::AccountAction::LoadProfile);
    bool load = true;
    switch (page) {
        case AccountPage::Settings:
        case AccountPage::Profile:
        case AccountPage::EditName:
        case AccountPage::EditUsername:
        case AccountPage::EditBio: break;
        case AccountPage::PrivateNotifications:
        case AccountPage::GroupNotifications:
        case AccountPage::ChannelNotifications:
            request.action = telegram::AccountAction::LoadNotifications;
            request.scope = page == AccountPage::PrivateNotifications ? telegram::NotificationScope::PrivateChats
                            : page == AccountPage::GroupNotifications ? telegram::NotificationScope::GroupChats
                                                                      : telegram::NotificationScope::Channels;
            break;
        case AccountPage::PhonePrivacy:
        case AccountPage::LastSeenPrivacy:
        case AccountPage::PhotoPrivacy:
            request.action = telegram::AccountAction::LoadPrivacy;
            request.privacy_key = page == AccountPage::PhonePrivacy ? telegram::PrivacyKey::PhoneNumber
                                  : page == AccountPage::LastSeenPrivacy ? telegram::PrivacyKey::LastSeen
                                                                        : telegram::PrivacyKey::ProfilePhoto;
            break;
        case AccountPage::Devices: request.action = telegram::AccountAction::LoadSessions; break;
        case AccountPage::Blocked: request.action = telegram::AccountAction::LoadBlocked; break;
        case AccountPage::Storage: request.action = telegram::AccountAction::LoadStorage; break;
        case AccountPage::ContactDetails:
            chat_details_open_ = false;
            request.action = telegram::AccountAction::LoadContactDetails;
            request.target_id = selected_contact_id_;
            break;
        default: load = false; break;
    }
    if (load) view_model_.account_action(request);
}

void TelegramView::account_back()
{
    switch (account_page_) {
        case AccountPage::EditName:
        case AccountPage::EditUsername:
        case AccountPage::EditBio: show_account_page(AccountPage::Profile); break;
        case AccountPage::PrivateNotifications:
        case AccountPage::GroupNotifications:
        case AccountPage::ChannelNotifications: show_account_page(AccountPage::Notifications); break;
        case AccountPage::PhonePrivacy:
        case AccountPage::LastSeenPrivacy:
        case AccountPage::PhotoPrivacy:
        case AccountPage::Blocked: show_account_page(AccountPage::Privacy); break;
        case AccountPage::AddContact:
        case AccountPage::ContactDetails:
            hide_keyboard();
            account_page_ = AccountPage::Settings;
            main_tab_ = MainTab::Contacts;
            view_model_.clear_account_feedback();
            media_changed_ = true;
            break;
        default: show_account_page(AccountPage::Settings); break;
    }
}

void TelegramView::show_chat_details()
{
    const auto* chat = view_model_.model().current_chat();
    if (!chat) return;
    hide_keyboard();
    if (!chat_details_open_) peer_scroll_y_ = 0;
    chat_details_open_ = true;
    main_tab_ = MainTab::Chats;
    account_page_ = AccountPage::Settings;
    pending_account_chat_open_ = false;
    view_model_.clear_account_feedback();
    auto request = account_request(telegram::AccountAction::LoadChatDetails);
    request.target_id = chat->id;
    view_model_.account_action(request);
    media_changed_ = true;
}

void TelegramView::close_chat_details()
{
    chat_details_open_ = false;
    view_model_.clear_account_feedback();
    media_changed_ = true;
}

void TelegramView::build_peer_details(lv_obj_t* parent, bool from_chat)
{
    using telegram::AccountAction;
    const auto& state = view_model_.account();
    const auto& peer = state.peer_details;
    const auto* current_chat = from_chat ? view_model_.model().current_chat() : nullptr;
    const auto target = from_chat ? (current_chat ? current_chat->id : 0) : selected_contact_id_;
    const bool loaded = peer.loaded && peer.target_id == target && peer.target_is_chat == from_chat;
    const bool busy = view_model_.account_busy();
    const bool is_user = loaded ? peer.kind == telegram::PeerKind::User : !current_chat || current_chat->is_private;
    const bool is_channel = loaded && peer.kind == telegram::PeerKind::Channel;
    telegram::Contact user;
    if (loaded) user = peer.user;
    else if (!from_chat) {
        const auto found = std::find_if(state.contacts.begin(), state.contacts.end(), [target](const auto& contact) {
            return contact.id == target;
        });
        if (found != state.contacts.end()) user = *found;
    }
    telegram::Chat avatar_chat;
    if (current_chat) avatar_chat = *current_chat;
    if (loaded && from_chat) avatar_chat = peer.chat;
    if (is_user && user.id != 0) {
        avatar_chat.id = user.id;
        avatar_chat.title = telegram::contact_name(user);
        avatar_chat.photo = user.photo;
    }
    if (avatar_chat.title.empty()) avatar_chat.title = "Contact";

    auto* details = lv_obj_create(parent);
    lv_obj_set_name_static(details, "telegram_peer_details");
    account_plain(details);
    lv_obj_set_width(details, LV_PCT(100));
    lv_obj_set_height(details, 0);
    lv_obj_set_flex_grow(details, 1);
    lv_obj_set_flex_flow(details, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(details, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scroll_dir(details, LV_DIR_VER);
    lv_obj_set_style_pad_bottom(details, 24, 0);
    lv_obj_set_style_bg_color(details, lv_color_hex(components::color_page_background), 0);
    lv_obj_set_style_bg_opa(details, LV_OPA_COVER, 0);

    auto* toolbar = lv_obj_create(details);
    account_plain(toolbar);
    lv_obj_set_size(toolbar, LV_PCT(100), 48);
    lv_obj_set_style_pad_hor(toolbar, 12, 0);
    lv_obj_set_flex_flow(toolbar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(toolbar, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(toolbar, LV_OBJ_FLAG_SCROLLABLE);
    auto* reload = lv_button_create(toolbar);
    account_plain(reload);
    lv_obj_set_name_static(reload, "telegram_peer_reload");
    lv_obj_set_size(reload, 48, 48);
    auto* reload_icon = account_label(reload, LV_SYMBOL_REFRESH, 22, components::color_action);
    lv_obj_center(reload_icon);
    auto reload_action = std::make_unique<AccountUiAction>();
    reload_action->run = [this, from_chat] {
        if (from_chat) show_chat_details();
        else show_account_page(AccountPage::ContactDetails);
    };
    lv_obj_add_event_cb(reload, account_ui_event, LV_EVENT_CLICKED, reload_action.get());
    account_actions_.push_back(std::move(reload_action));
    if (busy) lv_obj_add_state(reload, LV_STATE_DISABLED);

    auto* hero = lv_obj_create(details);
    account_plain(hero);
    lv_obj_set_name_static(hero, "telegram_peer_header");
    lv_obj_set_width(hero, LV_PCT(100));
    lv_obj_set_style_max_width(hero, 680, 0);
    lv_obj_set_height(hero, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_hor(hero, 20, 0);
    lv_obj_set_style_pad_bottom(hero, 24, 0);
    lv_obj_set_style_pad_row(hero, 10, 0);
    lv_obj_set_flex_flow(hero, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(hero, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(hero, static_cast<lv_obj_flag_t>(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));
    make_avatar(hero, avatar_chat, 96);
    auto* name = account_label(hero, avatar_chat.title, 28);
    lv_obj_set_name_static(name, "telegram_peer_name");
    lv_obj_set_width(name, LV_PCT(100));
    lv_obj_set_style_text_align(name, LV_TEXT_ALIGN_CENTER, 0);
    const auto subtitle = is_user ? user.status : loaded
        ? (peer.member_count > 0 ? std::to_string(peer.member_count) + (is_channel ? " subscribers" : " members")
                                 : is_channel ? "Channel" : "Group") : std::string{};
    if (!subtitle.empty()) {
        auto* status = account_label(hero, subtitle, 14, user.is_online ? components::color_action : components::color_text_secondary);
        lv_obj_set_name_static(status, "telegram_peer_status");
        lv_obj_set_width(status, LV_PCT(100));
        lv_obj_set_style_text_align(status, LV_TEXT_ALIGN_CENTER, 0);
    }

    const auto info = [details](const char* object_name, const char* title, const std::string& value) {
        auto* row = lv_obj_create(details);
        account_plain(row);
        lv_obj_set_name_static(row, object_name);
        lv_obj_set_width(row, LV_PCT(100));
        lv_obj_set_style_max_width(row, 680, 0);
        lv_obj_set_height(row, LV_SIZE_CONTENT);
        lv_obj_set_style_min_height(row, 76, 0);
        lv_obj_set_style_pad_hor(row, 20, 0);
        lv_obj_set_style_pad_ver(row, 14, 0);
        lv_obj_set_style_pad_row(row, 6, 0);
        lv_obj_set_style_bg_color(row, lv_color_hex(components::color_container_background), 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(row, lv_color_hex(components::color_border), 0);
        lv_obj_set_style_border_width(row, 1, 0);
        lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, 0);
        lv_obj_set_style_border_opa(row, LV_OPA_30, 0);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_COLUMN);
        lv_obj_remove_flag(row, static_cast<lv_obj_flag_t>(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));
        lv_obj_set_width(account_label(row, title, 14, components::color_action), LV_PCT(100));
        lv_obj_set_width(account_label(row, value), LV_PCT(100));
    };

    if (!view_model_.account_error().empty())
        account_message(details, "telegram_account_error", view_model_.account_error(), components::color_destructive);
    else if (!view_model_.account_notice().empty())
        account_message(details, "telegram_account_notice", view_model_.account_notice(), components::color_status_ready);
    if (busy) account_message(details, "telegram_peer_loading", "Loading...");
    else if (!loaded && view_model_.account_error().empty())
        account_message(details, "telegram_peer_unavailable", "Info unavailable");

    const auto user_id = user.id;
    if (from_chat || user_id != 0) {
        auto* message = account_row(details, from_chat ? "telegram_peer_message" : "telegram_contact_message",
            is_channel ? "Open Channel" : "Message", {}, "\xef\x81\xb5", [this, from_chat, user_id] {
                if (from_chat) { close_chat_details(); return; }
                auto request = account_request(AccountAction::OpenContact);
                request.target_id = user_id;
                if (view_model_.account_action(request)) {
                    pending_account_chat_open_ = true;
                    media_changed_ = true;
                }
            });
        lv_obj_set_style_max_width(message, 680, 0);
        lv_obj_set_style_margin_bottom(message, 20, 0);
        if (busy && !from_chat) lv_obj_add_state(message, LV_STATE_DISABLED);
    }

    if (is_user && user_id != 0) {
        info("telegram_peer_phone", "phone", user.phone_number.empty() ? "Not shared" :
            account_phone_number(user.phone_number));
        if (loaded) info("telegram_peer_username", "username", peer.username.empty() ? "Not set" : "@" + peer.username);
        if (loaded) info("telegram_peer_description", "bio", peer.description.empty() ? "No bio" : peer.description);
    } else if (loaded) {
        const auto type = is_channel ? (peer.username.empty() ? "Private Channel" : "Public Channel") :
                                      (peer.username.empty() ? "Private Group" : "Public Group");
        info("telegram_peer_type", "type", type);
        info("telegram_peer_members", is_channel ? "subscribers" : "members",
             peer.member_count > 0 ? std::to_string(peer.member_count) : "Not available");
        if (!peer.username.empty()) info("telegram_peer_username", "link", "https://t.me/" + peer.username);
        info("telegram_peer_description", "description", peer.description.empty() ? "No description" : peer.description);
    }
    if (is_user && user.is_contact) {
        auto* remove = account_row(details, "telegram_contact_remove", "Delete Contact", {}, LV_SYMBOL_TRASH,
            [this, user_id, user_name = telegram::contact_name(user)] {
                confirm_account_action("Delete Contact?", user_name.c_str(), [this, user_id] {
                    auto request = account_request(AccountAction::RemoveContact);
                    request.target_id = user_id;
                    view_model_.account_action(request);
                });
            });
        lv_obj_set_style_max_width(remove, 680, 0);
        lv_obj_set_style_margin_top(remove, 24, 0);
        if (busy) lv_obj_add_state(remove, LV_STATE_DISABLED);
    }
}

void TelegramView::build_settings(lv_obj_t* parent)
{
    if (account_page_ == AccountPage::ContactDetails) {
        build_peer_details(parent, false);
        return;
    }
    using telegram::AccountAction;
    auto* settings = lv_obj_create(parent);
    lv_obj_set_name_static(settings, "telegram_settings");
    account_plain(settings);
    lv_obj_set_width(settings, LV_PCT(100));
    lv_obj_set_height(settings, 0);
    lv_obj_set_flex_grow(settings, 1);
    lv_obj_set_style_bg_color(settings, lv_color_hex(components::color_page_background), 0);
    lv_obj_set_style_bg_opa(settings, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_bottom(settings, 20, 0);
    lv_obj_set_flex_flow(settings, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(settings, LV_DIR_VER);

    const auto& state = view_model_.account();
    const bool busy = view_model_.account_busy();
    if (!view_model_.account_error().empty()) {
        account_message(settings, "telegram_account_error", view_model_.account_error(), components::color_destructive);
    } else if (!view_model_.account_notice().empty()) {
        account_message(settings, "telegram_account_notice", view_model_.account_notice(), components::color_status_ready);
    }
    if (busy) account_message(settings, "telegram_account_loading", "Updating...");

    const auto navigate = [this, settings](const char* name, const char* title, const std::string& detail,
                                         const char* icon, AccountPage page) {
        return account_row(settings, name, title, detail, icon, [this, page] { show_account_page(page); });
    };
    const auto action_row = [this, settings, busy](const char* name, const char* title, const std::string& detail,
                                                  const char* icon, std::function<void()> action) {
        auto* row = account_row(settings, name, title, detail, icon, std::move(action));
        if (busy) lv_obj_add_state(row, LV_STATE_DISABLED);
        return row;
    };
    const auto reload = [this, settings, busy] {
        auto* toolbar = lv_obj_create(settings);
        account_plain(toolbar);
        lv_obj_set_size(toolbar, LV_PCT(100), 48);
        lv_obj_set_flex_flow(toolbar, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(toolbar, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_right(toolbar, 12, 0);
        lv_obj_remove_flag(toolbar, LV_OBJ_FLAG_SCROLLABLE);
        auto* button = lv_button_create(toolbar);
        account_plain(button);
        lv_obj_set_name_static(button, "telegram_account_reload");
        lv_obj_set_size(button, 48, 48);
        auto* label = account_label(button, LV_SYMBOL_REFRESH, 22, components::color_action);
        lv_obj_center(label);
        auto context = std::make_unique<AccountUiAction>();
        context->run = [this] { show_account_page(account_page_); };
        lv_obj_add_event_cb(button, account_ui_event, LV_EVENT_CLICKED, context.get());
        account_actions_.push_back(std::move(context));
        if (busy) lv_obj_add_state(button, LV_STATE_DISABLED);
    };
    const auto toggle = [this, settings, busy](const char* name, const char* title, bool checked, bool loaded,
                                               std::function<void(bool)> change) {
        auto* row = account_row(settings, "telegram_account_toggle_row", title, {}, nullptr, {});
        auto* control = lv_switch_create(row);
        lv_obj_set_name_static(control, name);
        lv_obj_set_size(control, 56, 32);
        lv_obj_set_ext_click_area(control, 8);
        lv_obj_set_style_bg_color(control, lv_color_hex(components::color_action), LV_PART_INDICATOR | LV_STATE_CHECKED);
        if (checked) lv_obj_add_state(control, LV_STATE_CHECKED);
        if (busy || !loaded) lv_obj_add_state(control, LV_STATE_DISABLED);
        auto context = std::make_unique<AccountUiAction>();
        context->run = [control, change = std::move(change)] { change(lv_obj_has_state(control, LV_STATE_CHECKED)); };
        lv_obj_add_event_cb(control, account_ui_event, LV_EVENT_VALUE_CHANGED, context.get());
        account_actions_.push_back(std::move(context));
    };
    const auto field = [this, settings](const char* name, const char* title, const std::string& value,
                                        std::uint32_t max_length, bool multiline = false) {
        account_section(settings, title);
        auto* input = lv_textarea_create(settings);
        lv_obj_set_name_static(input, name);
        account_font(input, 22);
        lv_obj_set_width(input, LV_PCT(100));
        lv_obj_set_height(input, multiline ? 128 : 60);
        lv_obj_set_style_pad_hor(input, 20, 0);
        lv_obj_set_style_pad_ver(input, 16, 0);
        lv_obj_set_style_radius(input, 0, 0);
        lv_obj_set_style_border_width(input, 0, 0);
        lv_obj_set_style_bg_color(input, lv_color_hex(components::color_container_background), 0);
        lv_obj_set_style_text_color(input, lv_color_hex(components::color_text), 0);
        lv_textarea_set_one_line(input, !multiline);
        lv_textarea_set_max_length(input, max_length);
        const auto draft = account_form_drafts_.find(name);
        lv_textarea_set_text(input, draft == account_form_drafts_.end() ? value.c_str() : draft->second.c_str());
        lv_obj_add_event_cb(input, input_focused_event, LV_EVENT_FOCUSED, this);
        lv_obj_add_event_cb(input, input_focused_event, LV_EVENT_CLICKED, this);
        account_inputs_.push_back(input);
        return input;
    };
    const auto submit = [this, settings, busy](const char* name, const char* title, std::function<void()> action) {
        account_section(settings, "");
        auto context = std::make_unique<AccountUiAction>();
        context->run = std::move(action);
        auto* button = components::create_button(settings, title, account_ui_event, context.get());
        lv_obj_set_name_static(button, name);
        lv_obj_set_width(button, LV_PCT(100));
        lv_obj_set_height(button, 56);
        lv_obj_set_style_radius(button, 0, 0);
        lv_obj_set_style_shadow_width(button, 0, 0);
        if (busy) lv_obj_add_state(button, LV_STATE_DISABLED);
        account_actions_.push_back(std::move(context));
    };

    switch (account_page_) {
        case AccountPage::Settings: {
            account_section(settings, "ACCOUNT");
            const auto name = state.profile.loaded ? telegram::contact_name(state.profile.user) : "My Profile";
            navigate("telegram_settings_profile", name.c_str(), account_phone_number(state.profile.user.phone_number), kUserIcon, AccountPage::Profile);
            action_row("telegram_settings_saved", "Saved Messages", {}, kBookmarkIcon, [this] {
                if (view_model_.account_action(account_request(AccountAction::OpenSavedMessages))) {
                    hide_keyboard();
                    pending_account_chat_open_ = true;
                    media_changed_ = true;
                }
            });
            account_section(settings, "SETTINGS");
            navigate("telegram_settings_notifications", "Notifications and Sounds", {}, kBellIcon, AccountPage::Notifications);
            navigate("telegram_settings_privacy", "Privacy and Security", {}, kLockIcon, AccountPage::Privacy);
            navigate("telegram_settings_storage", "Data and Storage", {}, LV_SYMBOL_DRIVE, AccountPage::Storage);
            navigate("telegram_settings_appearance", "Appearance", {}, kBrushIcon, AccountPage::Appearance);
            navigate("telegram_settings_devices", "Devices", {}, kPhoneIcon, AccountPage::Devices);
            account_section(settings, "APPLICATION");
            account_row(settings, "telegram_settings_connection", "Connection", view_model_.status_text(), LV_SYMBOL_WIFI, {});
            navigate("telegram_settings_about", "About", LILYGO_UI_TELEGRAM_APP_NAME, kInfoIcon, AccountPage::About);
            auto* logout = components::create_button(settings, "Log Out", logout_event, this);
            lv_obj_set_name_static(logout, "telegram_logout");
            lv_obj_set_size(logout, LV_PCT(100), 60);
            lv_obj_set_style_radius(logout, 0, 0);
            lv_obj_set_style_shadow_width(logout, 0, 0);
            lv_obj_set_style_margin_top(logout, 24, 0);
            lv_obj_set_style_bg_color(logout, lv_color_hex(components::color_container_background), 0);
            lv_obj_set_style_text_color(lv_obj_get_child(logout, 0), lv_color_hex(components::color_destructive), 0);
            if (busy) lv_obj_add_state(logout, LV_STATE_DISABLED);
            break;
        }
        case AccountPage::Profile: {
            reload();
            if (!state.profile.loaded) {
                account_message(settings, "telegram_profile_empty", busy ? "Loading profile..." : "Profile unavailable");
                break;
            }
            const auto& user = state.profile.user;
            account_section(settings, "PROFILE");
            navigate("telegram_profile_name", "Name", telegram::contact_name(user), kUserIcon, AccountPage::EditName);
            account_row(settings, "telegram_profile_phone", "Phone Number", user.phone_number.empty() ? "Not set" : account_phone_number(user.phone_number), kPhoneIcon, {});
            navigate("telegram_profile_username", "Username", user.username.empty() ? "Not set" : "@" + user.username, nullptr, AccountPage::EditUsername);
            navigate("telegram_profile_bio", "Bio", state.profile.bio.empty() ? "Not set" : state.profile.bio, nullptr, AccountPage::EditBio);
            break;
        }
        case AccountPage::EditName:
        case AccountPage::EditUsername:
        case AccountPage::EditBio: {
            if (!state.profile.loaded) {
                account_message(settings, "telegram_profile_empty", busy ? "Loading profile..." : "Profile unavailable");
                reload();
                break;
            }
            if (account_page_ == AccountPage::EditName) {
                auto* first = field("telegram_profile_first_name", "FIRST NAME", state.profile.user.first_name, 64);
                auto* last = field("telegram_profile_last_name", "LAST NAME", state.profile.user.last_name, 64);
                submit("telegram_profile_save", "Save", [this, first, last] {
                    auto request = account_request(AccountAction::SetName);
                    request.profile.user.first_name = lv_textarea_get_text(first);
                    request.profile.user.last_name = lv_textarea_get_text(last);
                    view_model_.account_action(request);
                    hide_keyboard();
                });
            } else if (account_page_ == AccountPage::EditUsername) {
                auto* input = field("telegram_profile_username_input", "USERNAME", state.profile.user.username, 32);
                lv_textarea_set_accepted_chars(input, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_");
                submit("telegram_profile_save", "Save", [this, input] {
                    auto request = account_request(AccountAction::SetUsername);
                    request.profile.user.username = lv_textarea_get_text(input);
                    view_model_.account_action(request);
                    hide_keyboard();
                });
            } else {
                auto* input = field("telegram_profile_bio_input", "BIO", state.profile.bio, 70, true);
                submit("telegram_profile_save", "Save", [this, input] {
                    auto request = account_request(AccountAction::SetBio);
                    request.profile.bio = lv_textarea_get_text(input);
                    view_model_.account_action(request);
                    hide_keyboard();
                });
            }
            break;
        }
        case AccountPage::Notifications: {
            account_section(settings, "MESSAGE NOTIFICATIONS");
            static constexpr std::array<const char*, 3> names{"telegram_notifications_private", "telegram_notifications_groups", "telegram_notifications_channels"};
            static constexpr std::array<const char*, 3> titles{"Private Chats", "Groups", "Channels"};
            static constexpr std::array<AccountPage, 3> pages{AccountPage::PrivateNotifications, AccountPage::GroupNotifications, AccountPage::ChannelNotifications};
            for (std::size_t index = 0; index < pages.size(); ++index) {
                const auto& value = state.notifications[index];
                navigate(names[index], titles[index], value.loaded ? (value.enabled ? "On" : "Off") : "", kBellIcon, pages[index]);
            }
            break;
        }
        case AccountPage::PrivateNotifications:
        case AccountPage::GroupNotifications:
        case AccountPage::ChannelNotifications: {
            reload();
            const auto scope = account_page_ == AccountPage::PrivateNotifications ? telegram::NotificationScope::PrivateChats
                               : account_page_ == AccountPage::GroupNotifications ? telegram::NotificationScope::GroupChats
                                                                                  : telegram::NotificationScope::Channels;
            const auto& value = state.notifications[static_cast<std::size_t>(scope)];
            if (!value.loaded) {
                account_message(settings, "telegram_notifications_empty", busy ? "Loading notifications..." : "Notifications unavailable");
                break;
            }
            account_section(settings, "NOTIFICATIONS");
            const auto change = [this, scope](int property, bool checked) {
                auto request = account_request(AccountAction::SetNotifications);
                request.scope = scope;
                request.notifications = view_model_.account().notifications[static_cast<std::size_t>(scope)];
                if (property == 0) request.notifications.enabled = checked;
                if (property == 1) request.notifications.show_preview = checked;
                if (property == 2) request.notifications.sound_enabled = checked;
                view_model_.account_action(request);
            };
            toggle("telegram_notification_enabled", "Notifications", value.enabled, value.loaded, [change](bool checked) { change(0, checked); });
            toggle("telegram_notification_preview", "Message Preview", value.show_preview, value.loaded, [change](bool checked) { change(1, checked); });
            toggle("telegram_notification_sound", "Sound", value.sound_enabled, value.loaded, [change](bool checked) { change(2, checked); });
            break;
        }
        case AccountPage::Privacy: {
            account_section(settings, "PRIVACY");
            static constexpr std::array<const char*, 3> names{"telegram_privacy_phone", "telegram_privacy_last_seen", "telegram_privacy_photo"};
            static constexpr std::array<const char*, 3> titles{"Phone Number", "Last Seen and Online", "Profile Photos"};
            static constexpr std::array<AccountPage, 3> pages{AccountPage::PhonePrivacy, AccountPage::LastSeenPrivacy, AccountPage::PhotoPrivacy};
            for (std::size_t index = 0; index < pages.size(); ++index) {
                const auto& value = state.privacy[index];
                navigate(names[index], titles[index], value.loaded ? privacy_title(value.visibility) : "", nullptr, pages[index]);
            }
            account_section(settings, "SECURITY");
            navigate("telegram_privacy_blocked", "Blocked Users", state.blocked_loaded ? std::to_string(state.blocked_total) : "", LV_SYMBOL_CLOSE, AccountPage::Blocked);
            navigate("telegram_privacy_devices", "Devices", {}, kPhoneIcon, AccountPage::Devices);
            break;
        }
        case AccountPage::PhonePrivacy:
        case AccountPage::LastSeenPrivacy:
        case AccountPage::PhotoPrivacy: {
            reload();
            const auto key = account_page_ == AccountPage::PhonePrivacy ? telegram::PrivacyKey::PhoneNumber
                             : account_page_ == AccountPage::LastSeenPrivacy ? telegram::PrivacyKey::LastSeen
                                                                            : telegram::PrivacyKey::ProfilePhoto;
            const auto& value = state.privacy[static_cast<std::size_t>(key)];
            if (!value.loaded) {
                account_message(settings, "telegram_privacy_empty", busy ? "Loading privacy settings..." : "Privacy settings unavailable");
                break;
            }
            account_section(settings, key == telegram::PrivacyKey::PhoneNumber ? "WHO CAN SEE MY PHONE NUMBER"
                                      : key == telegram::PrivacyKey::LastSeen ? "WHO CAN SEE MY LAST SEEN"
                                                                             : "WHO CAN SEE MY PROFILE PHOTOS");
            static constexpr std::array<telegram::PrivacyVisibility, 3> choices{telegram::PrivacyVisibility::Everyone, telegram::PrivacyVisibility::Contacts, telegram::PrivacyVisibility::Nobody};
            static constexpr std::array<const char*, 3> names{"telegram_privacy_everybody", "telegram_privacy_contacts", "telegram_privacy_nobody"};
            for (std::size_t index = 0; index < choices.size(); ++index) {
                const auto visibility = choices[index];
                auto* row = action_row(names[index], privacy_title(visibility), {}, nullptr, [this, key, visibility] {
                    auto request = account_request(AccountAction::SetPrivacy);
                    request.privacy_key = key;
                    request.visibility = visibility;
                    view_model_.account_action(request);
                });
                auto* checkmark = lv_obj_get_child(row, -1);
                lv_label_set_text(checkmark, value.visibility == visibility ? LV_SYMBOL_OK : "");
                lv_obj_set_style_text_color(checkmark, lv_color_hex(components::color_action), 0);
                if (value.visibility == visibility) lv_obj_add_state(row, LV_STATE_CHECKED);
            }
            account_section(settings, "EXCEPTIONS");
            account_row(settings, "telegram_privacy_exceptions", "Existing Exceptions", std::to_string(value.exception_count), nullptr, {});
            if (value.visibility == telegram::PrivacyVisibility::Custom) {
                account_message(settings, "telegram_privacy_custom", "Custom privacy rules");
            }
            break;
        }
        case AccountPage::Devices: {
            reload();
            if (!state.sessions_loaded) {
                account_message(settings, "telegram_devices_empty", busy ? "Loading devices..." : "Devices unavailable");
                break;
            }
            account_section(settings, "CURRENT SESSION");
            const auto describe = [](const telegram::AccountSession& session) {
                std::string detail = session.application_name;
                if (!session.platform.empty()) detail += (detail.empty() ? "" : " / ") + session.platform;
                if (!session.location.empty()) detail += "\n" + session.location;
                if (!session.ip_address.empty()) detail += "\n" + session.ip_address;
                if (session.last_active_date > 0 && !session.is_current) detail += "\n" + account_date(session.last_active_date);
                return detail;
            };
            for (const auto& session : state.sessions) {
                if (session.is_current) {
                    account_row(settings, "telegram_session_current", session.device_model.empty() ? "This Device" : session.device_model.c_str(), "This Device\n" + describe(session), kPhoneIcon, {});
                }
            }
            account_section(settings, "OTHER SESSIONS");
            bool others = false;
            for (const auto& session : state.sessions) {
                if (session.is_current) continue;
                others = true;
                const auto id = session.id;
                const auto name = session.device_model.empty() ? session.application_name : session.device_model;
                action_row("telegram_session_terminate", name.empty() ? "Telegram" : name.c_str(), describe(session), kPhoneIcon, [this, id, name] {
                    const auto message = "Log out " + (name.empty() ? std::string("this device") : name) + "?";
                    confirm_account_action("Terminate Session?", message.c_str(), [this, id] {
                        auto request = account_request(AccountAction::TerminateSession);
                        request.target_id = id;
                        view_model_.account_action(request);
                    });
                });
            }
            if (!others) account_message(settings, "telegram_sessions_empty", "No other active sessions");
            break;
        }
        case AccountPage::Blocked: {
            reload();
            account_section(settings, "BLOCKED USERS");
            if (state.blocked.empty()) {
                account_message(settings, "telegram_blocked_empty", !state.blocked_loaded ? (busy ? "Loading blocked users..." : "Blocked users unavailable") : "No blocked users");
            }
            for (const auto& sender : state.blocked) {
                const auto id = sender.id;
                const auto is_chat = sender.is_chat;
                const auto name = sender.name.empty() ? std::to_string(sender.id) : sender.name;
                action_row("telegram_blocked_unblock", name.c_str(), "Unblock", kUserIcon, [this, id, is_chat, name] {
                    confirm_account_action("Unblock?", name.c_str(), [this, id, is_chat] {
                        auto request = account_request(AccountAction::UnblockSender);
                        request.target_id = id;
                        request.target_is_chat = is_chat;
                        view_model_.account_action(request);
                    });
                });
            }
            if (state.blocked_total > static_cast<std::int32_t>(state.blocked.size())) {
                account_message(settings, "telegram_blocked_count", std::to_string(state.blocked.size()) + " of " + std::to_string(state.blocked_total));
            }
            break;
        }
        case AccountPage::Storage: {
            reload();
            account_section(settings, "STORAGE USAGE");
            if (state.storage.loaded) {
                account_row(settings, "telegram_storage_media", "Cached Media", account_bytes(state.storage.files_size), LV_SYMBOL_IMAGE, {});
                account_row(settings, "telegram_storage_files", "Files", std::to_string(state.storage.file_count), LV_SYMBOL_FILE, {});
                account_row(settings, "telegram_storage_database", "Database", account_bytes(state.storage.database_size), LV_SYMBOL_DRIVE, {});
                action_row("telegram_storage_clear", "Clear Media Cache", {}, LV_SYMBOL_TRASH, [this] {
                    confirm_account_action("Clear Media Cache?", "Downloaded photos, videos and voice messages will be removed from this device. Cloud messages stay in Telegram.", [this] {
                        view_model_.account_action(account_request(AccountAction::ClearMediaCache));
                    });
                });
            } else {
                account_message(settings, "telegram_storage_empty", busy ? "Calculating storage..." : "Storage usage unavailable");
            }
            account_section(settings, "AUTOMATIC MEDIA DOWNLOAD");
            toggle("telegram_storage_auto_photos", "Photos", view_model_.preferences().auto_download_photos, true, [this](bool checked) {
                auto preferences = view_model_.preferences();
                preferences.auto_download_photos = checked;
                view_model_.set_preferences(preferences);
            });
            break;
        }
        case AccountPage::Appearance: {
            account_section(settings, "MESSAGE TEXT SIZE");
            auto* row = account_row(settings, "telegram_appearance_size_row", "Text Size", {}, nullptr, {});
            auto* choice = lv_dropdown_create(row);
            lv_obj_set_name_static(choice, "telegram_appearance_font_size");
            account_font(choice, 22);
            lv_obj_set_size(choice, 120, 48);
            lv_dropdown_set_options(choice, "18\n22\n26\n30");
            const auto size = view_model_.preferences().message_font_size;
            lv_dropdown_set_selected(choice, size == 18 ? 0 : size == 26 ? 2 : size == 30 ? 3 : 1);
            if (auto* list = lv_dropdown_get_list(choice)) account_font(list, 22);
            auto context = std::make_unique<AccountUiAction>();
            context->run = [this, choice] {
                static constexpr std::array<std::int32_t, 4> sizes{18, 22, 26, 30};
                auto preferences = view_model_.preferences();
                preferences.message_font_size = sizes[std::min<std::size_t>(lv_dropdown_get_selected(choice), sizes.size() - 1)];
                view_model_.set_preferences(preferences);
                media_changed_ = true;
            };
            lv_obj_add_event_cb(choice, account_ui_event, LV_EVENT_VALUE_CHANGED, context.get());
            account_actions_.push_back(std::move(context));
            account_section(settings, "PREVIEW");
            auto* preview = lv_obj_create(settings);
            account_plain(preview);
            lv_obj_set_name_static(preview, "telegram_appearance_preview");
            lv_obj_set_width(preview, LV_PCT(100));
            lv_obj_set_height(preview, LV_SIZE_CONTENT);
            lv_obj_set_style_bg_color(preview, lv_color_hex(components::color_conversation_background), 0);
            lv_obj_set_style_bg_opa(preview, LV_OPA_COVER, 0);
            lv_obj_set_style_pad_all(preview, 20, 0);
            lv_obj_set_flex_flow(preview, LV_FLEX_FLOW_COLUMN);
            auto* bubble = account_label(preview, "Good morning!", static_cast<std::uint32_t>(size));
            lv_obj_set_width(bubble, LV_PCT(85));
            lv_obj_set_style_bg_color(bubble, lv_color_hex(components::color_outgoing_message), 0);
            lv_obj_set_style_bg_opa(bubble, LV_OPA_COVER, 0);
            lv_obj_set_style_radius(bubble, 8, 0);
            lv_obj_set_style_pad_all(bubble, 14, 0);
            break;
        }
        case AccountPage::About:
            account_section(settings, "APPLICATION");
            account_row(settings, "telegram_about_app", LILYGO_UI_TELEGRAM_APP_NAME, {}, kInfoIcon, {});
            account_message(settings, "telegram_about_disclosure", "Unofficial client. Not affiliated with Telegram.");
#ifdef LILYGO_UI_TELEGRAM_APP_VERSION
            account_row(settings, "telegram_about_version", "Version", LILYGO_UI_TELEGRAM_APP_VERSION, nullptr, {});
#endif
            account_row(settings, "telegram_about_client", "Telegram API", "TDLib", nullptr, {});
            account_row(settings, "telegram_about_connection", "Connection", view_model_.status_text(), LV_SYMBOL_WIFI, {});
            break;
        case AccountPage::AddContact: {
            auto* first = field("telegram_contact_first_name", "FIRST NAME", {}, 64);
            auto* last = field("telegram_contact_last_name", "LAST NAME", {}, 64);
            auto* phone = field("telegram_contact_phone", "PHONE NUMBER", {}, 16);
            lv_textarea_set_accepted_chars(phone, "+0123456789");
            submit("telegram_contact_save", "Create Contact", [this, first, last, phone] {
                auto request = account_request(AccountAction::AddContact);
                request.contact.first_name = lv_textarea_get_text(first);
                request.contact.last_name = lv_textarea_get_text(last);
                request.contact.phone_number = lv_textarea_get_text(phone);
                view_model_.account_action(request);
                hide_keyboard();
            });
            break;
        }
        case AccountPage::ContactDetails: break;
    }
}
