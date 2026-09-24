#ifndef LILYGO_UI_TEMPLATE_COMPONENTS_HPP
#define LILYGO_UI_TEMPLATE_COMPONENTS_HPP

#include <lvgl.h>

#include <cstdint>

namespace components {

inline constexpr std::uint32_t color_page_background         = 0xf2f2f7;
inline constexpr std::uint32_t color_container_background    = 0xffffff;
inline constexpr std::uint32_t color_container_title         = 0x1a1a1a;
inline constexpr std::uint32_t color_text                    = 0x000000;
inline constexpr std::uint32_t color_text_secondary          = 0x8e8e93;
inline constexpr std::uint32_t color_border                  = 0xc8c7cc;
inline constexpr std::uint32_t color_action                  = 0x0088ff;
inline constexpr std::uint32_t color_action_pressed          = 0x0062cc;
inline constexpr std::uint32_t color_selection               = 0xe9f0fa;
inline constexpr std::uint32_t color_outgoing_message        = 0xe1ffc7;
inline constexpr std::uint32_t color_conversation_background = color_page_background;
inline constexpr std::uint32_t color_search_background       = 0xe9e9eb;
inline constexpr std::uint32_t color_outgoing_meta           = 0x60884b;
inline constexpr std::uint32_t color_date_background         = 0xd3dece;
inline constexpr std::uint32_t color_muted_badge             = 0xb6b6bb;
inline constexpr std::uint32_t color_destructive             = 0xff3b30;
inline constexpr std::uint32_t color_error_background        = 0xfff1f0;
inline constexpr std::uint32_t color_avatar_red              = 0xff516a;
inline constexpr std::uint32_t color_avatar_orange           = 0xffa85c;
inline constexpr std::uint32_t color_avatar_violet           = 0x9975da;
inline constexpr std::uint32_t color_avatar_green            = 0x4ac76f;
inline constexpr std::uint32_t color_avatar_cyan             = 0x29b9d6;
inline constexpr std::uint32_t color_status_ready            = 0x2f9e44;
inline constexpr std::uint32_t color_status_warning          = 0xd97706;

lv_obj_t *create_page(lv_obj_t *parent);
lv_obj_t *create_header(lv_obj_t *parent, const char *title);
lv_obj_t *create_card(lv_obj_t *parent);
lv_obj_t *create_button(lv_obj_t *parent, const char *text, lv_event_cb_t callback, void *user_data);

}  // namespace components

#endif
