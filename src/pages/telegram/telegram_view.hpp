#ifndef LILYGO_UI_TELEGRAM_PAGES_TELEGRAM_TELEGRAM_VIEW_HPP
#define LILYGO_UI_TELEGRAM_PAGES_TELEGRAM_TELEGRAM_VIEW_HPP

#include "pages/telegram/telegram_view_model.hpp"
#include "services/bounded_queue.hpp"
#include "services/media/image_decoder.hpp"
#include "services/media/voice_player.hpp"

#include <lvgl.h>

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <map>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

struct TelegramViewVoiceIdentityTestAccess;

class TelegramView {
public:
    explicit TelegramView(telegram::TelegramViewModel& view_model);
    ~TelegramView();

    TelegramView(const TelegramView&)            = delete;
    TelegramView& operator=(const TelegramView&) = delete;

    lv_obj_t* create(lv_obj_t* parent, const char* application_name);
    void destroy() noexcept;

private:
    friend struct TelegramViewVoiceIdentityTestAccess;

    struct ChatContext {
        TelegramView* view   = nullptr;
        std::int64_t chat_id = 0;
    };

    enum class MainTab { Contacts, Chats, Settings };
    enum class AccountPage {
        Settings, Profile, EditName, EditUsername, EditBio,
        Notifications, PrivateNotifications, GroupNotifications, ChannelNotifications,
        Privacy, PhonePrivacy, LastSeenPrivacy, PhotoPrivacy,
        Devices, Blocked, Storage, Appearance, About, AddContact, ContactDetails,
    };

    struct AccountUiAction {
        std::function<void()> run;
    };

    struct VoiceContext {
        TelegramView* view = nullptr;
        telegram::Message message;
    };

    struct PhotoContext {
        lv_obj_t* bubble = nullptr;
        telegram::Message message;
    };

    struct ImageResult {
        std::int32_t file_id = 0;
        telegram::media::ImageDecodeResult result;
    };

    struct AvatarResult {
        std::int32_t file_id = 0;
        std::uint64_t generation = 0;
        telegram::media::ImageDecodeResult result;
    };

    struct AvatarContext {
        lv_obj_t* object = nullptr;
        telegram::FileInfo file;
    };

    struct VoiceIdentity {
        std::int64_t chat_id    = 0;
        std::int64_t message_id = 0;

        [[nodiscard]] bool operator==(const VoiceIdentity& other) const noexcept
        {
            return chat_id == other.chat_id && message_id == other.message_id;
        }
    };

    struct VoiceResult {
        VoiceIdentity identity;
        telegram::media::VoicePlaybackEvent event;
    };

    struct CachedImage {
        std::shared_ptr<const telegram::media::DecodedImage> pixels;
        lv_image_dsc_t descriptor{};
    };

    struct AvatarEntry {
        std::unique_ptr<CachedImage> image;
        std::string path;
        std::string requested_path;
        std::uint32_t last_used = 0;
        std::uint32_t retry_at = 0;
        std::uint8_t failures = 0;
        bool downloading = false;
        bool decoding = false;
    };

    enum class ScrollRestoreMode {
        Bottom,
        Position,
        BottomDistance,
    };

    static void timer_tick(lv_timer_t* timer);
    static void size_changed_event(lv_event_t* event);
    static void chat_clicked_event(lv_event_t* event);
    static void back_clicked_event(lv_event_t* event);
    static void conversation_back_event(lv_event_t* event);
    static void chat_info_event(lv_event_t* event);
    static void send_clicked_event(lv_event_t* event);
    static void auth_submit_event(lv_event_t* event);
    static void logout_event(lv_event_t* event);
    static void older_clicked_event(lv_event_t* event);
    static void input_focused_event(lv_event_t* event);
    static void keyboard_event(lv_event_t* event);
    static void voice_clicked_event(lv_event_t* event);
    static void search_changed_event(lv_event_t* event);
    static void search_clear_event(lv_event_t* event);
    static void chats_tab_event(lv_event_t* event);
    static void settings_tab_event(lv_event_t* event);
    static void contacts_tab_event(lv_event_t* event);
    static void contact_search_event(lv_event_t* event);
    static void contact_clear_event(lv_event_t* event);
    static void account_ui_event(lv_event_t* event);
    static void account_confirm_event(lv_event_t* event);
    static void account_cancel_event(lv_event_t* event);
    static void logout_confirm_event(lv_event_t* event);
    static void logout_cancel_event(lv_event_t* event);
    static void composer_changed_event(lv_event_t* event);

    void tick();
    void rebuild();
    void build_header();
    void build_auth();
    void build_workspace();
    void build_chat_list(lv_obj_t* parent);
    void build_tabs(lv_obj_t* parent);
    void build_settings(lv_obj_t* parent);
    void build_contacts(lv_obj_t* parent);
    void build_peer_details(lv_obj_t* parent, bool from_chat);
    void show_chat_details();
    void close_chat_details();
    void show_account_page(AccountPage page);
    void account_back();
    [[nodiscard]] const char* account_page_title() const;
    lv_obj_t* account_row(lv_obj_t* parent, const char* name, const char* title,
                          const std::string& detail, const char* icon, std::function<void()> action);
    void confirm_account_action(const char* title, const char* text, std::function<void()> action);
    void build_conversation(lv_obj_t* parent);
    void build_message(lv_obj_t* parent, const telegram::Message& message);
    lv_obj_t* make_avatar(lv_obj_t* parent, const telegram::Chat& chat, std::int32_t size);
    void request_visible_avatars();
    void integrate_avatar_results();
    void trim_avatar_cache(const std::unordered_set<std::int32_t>& visible);
    void show_keyboard(lv_obj_t* textarea);
    void hide_keyboard();
    void submit_auth();
    void send_message();
    void play_voice(const telegram::Message& message);
    void request_photo(const telegram::Message& message);
    void request_visible_photos();
    void schedule_image_retry(std::int32_t file_id, std::string error);
    void integrate_media_results();
    void trim_image_cache();
    void clear_dynamic_contexts();
    void update_responsive_state();

    [[nodiscard]] static VoiceIdentity voice_identity(const telegram::Message& message) noexcept;
    [[nodiscard]] static bool voice_result_matches(const VoiceResult& result, const VoiceIdentity& active_identity,
                                                   telegram::media::VoiceRequestId active_request_id) noexcept;
    [[nodiscard]] bool is_wide() const noexcept;

    telegram::TelegramViewModel& view_model_;
    telegram::BoundedQueue<ImageResult> image_results_{8};
    telegram::BoundedQueue<AvatarResult> avatar_results_{8};
    telegram::BoundedQueue<VoiceResult> voice_results_{16};
    telegram::media::ImageDecoder image_decoder_{3};
    telegram::media::ImageDecoder avatar_decoder_{2};
    telegram::media::VoicePlayer voice_player_;

    lv_obj_t* surface_       = nullptr;
    lv_obj_t* header_        = nullptr;
    lv_obj_t* header_back_   = nullptr;
    lv_obj_t* header_title_  = nullptr;
    lv_obj_t* header_status_ = nullptr;
    lv_obj_t* header_logout_ = nullptr;
    lv_obj_t* body_          = nullptr;
    lv_obj_t* keyboard_      = nullptr;
    lv_obj_t* auth_input_    = nullptr;
    lv_obj_t* composer_      = nullptr;
    lv_obj_t* message_list_  = nullptr;
    lv_obj_t* chat_list_     = nullptr;
    lv_obj_t* search_input_  = nullptr;
    lv_obj_t* send_button_   = nullptr;
    lv_obj_t* logout_dialog_ = nullptr;
    lv_timer_t* timer_       = nullptr;

    std::string application_name_;
    std::string auth_draft_;
    std::string composer_draft_;
    std::uint64_t rendered_revision_         = 0;
    telegram::AuthState rendered_auth_state_ = telegram::AuthState::Unknown;
    bool wide_                               = false;
    bool media_changed_                      = false;
    bool pending_account_chat_open_          = false;
    bool chat_details_open_                  = false;
    bool rendered_chat_details_open_         = false;
    std::int32_t peer_scroll_y_               = 0;
    MainTab main_tab_                        = MainTab::Chats;
    AccountPage account_page_                = AccountPage::Settings;
    MainTab rendered_main_tab_               = MainTab::Chats;
    AccountPage rendered_account_page_       = AccountPage::Settings;
    std::map<std::pair<MainTab, AccountPage>, std::int32_t> account_scroll_positions_;
    std::int64_t selected_contact_id_         = 0;
    std::uint64_t rendered_cache_generation_  = 0;
    std::int32_t chat_scroll_y_              = 0;
    VoiceIdentity active_voice_identity_;
    telegram::media::VoiceRequestId active_voice_request_id_ = 0;
    telegram::media::VoicePlaybackState voice_state_         = telegram::media::VoicePlaybackState::idle;
    std::string media_status_;

    std::vector<std::unique_ptr<ChatContext>> chat_contexts_;
    std::vector<std::unique_ptr<VoiceContext>> voice_contexts_;
    std::vector<PhotoContext> photo_contexts_;
    std::vector<AvatarContext> avatar_contexts_;
    std::unordered_map<std::int32_t, AvatarEntry> avatar_cache_;
    std::vector<AvatarResult> pending_avatars_;
    std::uint64_t avatar_generation_ = 1;
    std::vector<std::unique_ptr<AccountUiAction>> account_actions_;
    std::vector<lv_obj_t*> account_inputs_;
    std::unordered_map<std::string, std::string> account_form_drafts_;
    std::function<void()> pending_account_confirmation_;
    std::deque<std::string> file_sources_;
    std::unordered_set<std::int32_t> requested_downloads_;
    std::unordered_map<std::int32_t, std::uint8_t> download_failures_;
    std::unordered_map<std::int32_t, std::uint32_t> download_retry_at_;
    std::unordered_set<std::int32_t> requested_images_;
    std::unordered_map<std::int32_t, std::uint8_t> image_failures_;
    std::unordered_map<std::int32_t, std::uint32_t> image_retry_at_;
    std::unordered_map<std::int32_t, std::unique_ptr<CachedImage>> image_cache_;
    std::deque<std::int32_t> image_cache_order_;
    std::vector<ImageResult> pending_images_;
    std::size_t image_cache_bytes_           = 0;
    std::int64_t rendered_chat_id_           = 0;
    std::int64_t rendered_oldest_message_id_ = 0;
    std::int64_t rendered_newest_message_id_ = 0;
    std::int32_t saved_scroll_y_             = 0;
    std::int32_t saved_scroll_bottom_        = 0;
    ScrollRestoreMode scroll_restore_mode_   = ScrollRestoreMode::Bottom;
};

#endif
