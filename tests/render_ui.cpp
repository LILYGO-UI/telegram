#include "app.hpp"
#include "domain/telegram_account_model.hpp"

#include <cm0/typography.h>

#include <algorithm>
#include <array>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <vector>

namespace {

constexpr int kPartialRows                = 80;
constexpr std::uint32_t kPageBackground   = 0xf2f2f7;
constexpr std::uint32_t kHeaderBackground = kPageBackground;

std::vector<lv_color32_t> framebuffer;
int display_width = 0;

void write_avatar_fixture(const std::filesystem::path& path)
{
    // Red/green/blue/yellow columns in an 8x4 PNG distinguish cover cropping from stretching.
    static constexpr unsigned char png[] = {
        0x89,0x50,0x4e,0x47,0x0d,0x0a,0x1a,0x0a,0x00,0x00,0x00,0x0d,
        0x49,0x48,0x44,0x52,0x00,0x00,0x00,0x08,0x00,0x00,0x00,0x04,
        0x08,0x02,0x00,0x00,0x00,0x3c,0xaf,0xe9,0xa7,0x00,0x00,0x00,
        0x01,0x73,0x52,0x47,0x42,0x00,0xae,0xce,0x1c,0xe9,0x00,0x00,
        0x00,0x44,0x65,0x58,0x49,0x66,0x4d,0x4d,0x00,0x2a,0x00,0x00,
        0x00,0x08,0x00,0x01,0x87,0x69,0x00,0x04,0x00,0x00,0x00,0x01,
        0x00,0x00,0x00,0x1a,0x00,0x00,0x00,0x00,0x00,0x03,0xa0,0x01,
        0x00,0x03,0x00,0x00,0x00,0x01,0x00,0x01,0x00,0x00,0xa0,0x02,
        0x00,0x04,0x00,0x00,0x00,0x01,0x00,0x00,0x00,0x08,0xa0,0x03,
        0x00,0x04,0x00,0x00,0x00,0x01,0x00,0x00,0x00,0x04,0x00,0x00,
        0x00,0x00,0x50,0x74,0x96,0x53,0x00,0x00,0x00,0x1b,0x49,0x44,
        0x41,0x54,0x08,0x1d,0x63,0xfc,0xcf,0x00,0x02,0x8c,0x48,0xd4,
        0xff,0xff,0x8c,0x40,0x11,0x26,0xb0,0x38,0x16,0x82,0x74,0x09,
        0x00,0xb6,0x50,0x05,0x06,0x89,0xf1,0x14,0x78,0x00,0x00,0x00,
        0x00,0x49,0x45,0x4e,0x44,0xae,0x42,0x60,0x82,
    };
    std::ofstream output(path, std::ios::binary);
    output.write(reinterpret_cast<const char*>(png), sizeof(png));
    assert(output.good());
}

void flush_display(lv_display_t* display, const lv_area_t* area, std::uint8_t* pixels)
{
    const int width    = area->x2 - area->x1 + 1;
    const auto* source = reinterpret_cast<const lv_color32_t*>(pixels);
    for (int y = area->y1; y <= area->y2; ++y) {
        const auto source_offset = static_cast<std::size_t>(y - area->y1) * static_cast<std::size_t>(width);
        const auto destination_offset =
            static_cast<std::size_t>(y) * static_cast<std::size_t>(display_width) + static_cast<std::size_t>(area->x1);
        std::copy_n(source + source_offset, width, framebuffer.begin() + destination_offset);
    }
    lv_display_flush_ready(display);
}

bool write_ppm(const char* path, int width, int height)
{
    std::ofstream file(path, std::ios::binary);
    if (!file) return false;
    file << "P6\n" << width << ' ' << height << "\n255\n";
    for (const auto& pixel : framebuffer) {
        file.put(static_cast<char>(pixel.red));
        file.put(static_cast<char>(pixel.green));
        file.put(static_cast<char>(pixel.blue));
    }
    return file.good();
}

bool colors_differ(const lv_color32_t& left, const lv_color32_t& right)
{
    return left.red != right.red || left.green != right.green || left.blue != right.blue || left.alpha != right.alpha;
}

struct PixelRegion {
    int x1;
    int y1;
    int x2;
    int y2;
};

bool visible_object_region(lv_obj_t* object, lv_obj_t* viewport, int height, PixelRegion* region)
{
    if (!object || !viewport || !region || display_width <= 0 || height <= 0) return false;

    lv_area_t object_area{};
    lv_area_t viewport_area{};
    lv_obj_get_coords(object, &object_area);
    lv_obj_get_coords(viewport, &viewport_area);

    region->x1 = std::max({0, static_cast<int>(object_area.x1), static_cast<int>(viewport_area.x1)});
    region->y1 = std::max({0, static_cast<int>(object_area.y1), static_cast<int>(viewport_area.y1)});
    region->x2 = std::min({display_width - 1, static_cast<int>(object_area.x2), static_cast<int>(viewport_area.x2)});
    region->y2 = std::min({height - 1, static_cast<int>(object_area.y2), static_cast<int>(viewport_area.y2)});
    return region->x1 <= region->x2 && region->y1 <= region->y2;
}

bool region_has_contrast(const PixelRegion& region)
{
    const auto& reference = framebuffer[static_cast<std::size_t>(region.y1) * display_width + region.x1];
    for (int y = region.y1; y <= region.y2; ++y) {
        for (int x = region.x1; x <= region.x2; ++x) {
            if (colors_differ(framebuffer[static_cast<std::size_t>(y) * display_width + x], reference)) return true;
        }
    }
    return false;
}

void advance_ui(std::uint32_t milliseconds = 300)
{
    for (std::uint32_t elapsed = 0; elapsed < milliseconds; elapsed += 50) {
        lv_tick_inc(50);
        lv_timer_handler();
    }
    lv_obj_update_layout(lv_screen_active());
}

lv_obj_t* required(lv_obj_t* root, const char* name)
{
    auto* object = lv_obj_find_by_name(root, name);
    if (!object) std::fprintf(stderr, "Missing UI object: %s\n", name);
    assert(object);
    return object;
}

std::size_t count_named(lv_obj_t* root, const char* name)
{
    const auto* object_name = lv_obj_get_name(root);
    std::size_t count       = object_name && std::strcmp(object_name, name) == 0 ? 1 : 0;
    for (std::uint32_t index = 0; index < lv_obj_get_child_count(root); ++index) {
        count += count_named(lv_obj_get_child(root, index), name);
    }
    return count;
}

bool contains_label(lv_obj_t* root, const char* text)
{
    if (lv_obj_check_type(root, &lv_label_class)) {
        // LVGL temporarily replaces source characters with dots while rendering narrow labels.
        if (lv_label_get_long_mode(root) == LV_LABEL_LONG_DOT) lv_label_set_text(root, nullptr);
        if (std::strcmp(lv_label_get_text(root), text) == 0) return true;
    }
    for (std::uint32_t index = 0; index < lv_obj_get_child_count(root); ++index) {
        if (contains_label(lv_obj_get_child(root, index), text)) return true;
    }
    return false;
}

void click(lv_obj_t* root, const char* name)
{
    auto* object = required(root, name);
    lv_obj_scroll_to_view_recursive(object, LV_ANIM_OFF);
    advance_ui(50);
    lv_obj_send_event(required(root, name), LV_EVENT_CLICKED, nullptr);
    advance_ui();
}

void set_switch(lv_obj_t* root, const char* name, bool enabled)
{
    auto* object = required(root, name);
    assert(!lv_obj_has_state(object, LV_STATE_DISABLED));
    if (enabled) lv_obj_add_state(object, LV_STATE_CHECKED);
    else lv_obj_remove_state(object, LV_STATE_CHECKED);
    lv_obj_send_event(object, LV_EVENT_VALUE_CHANGED, nullptr);
    advance_ui();
    assert(lv_obj_has_state(required(root, name), LV_STATE_CHECKED) == enabled);
}

lv_obj_t* named_row(lv_obj_t* root, const char* object_name, const char* title)
{
    const auto* name = lv_obj_get_name(root);
    if (name && std::strcmp(name, object_name) == 0 && contains_label(root, title)) return root;
    for (std::uint32_t index = 0; index < lv_obj_get_child_count(root); ++index) {
        if (auto* row = named_row(lv_obj_get_child(root, index), object_name, title)) return row;
    }
    return nullptr;
}

void show_chats(lv_obj_t* root)
{
    if (!lv_obj_find_by_name(root, "telegram_chat_list")) click(root, "telegram_back");
    assert(lv_obj_is_visible(required(root, "telegram_chat_list")));
}

void open_chat(lv_obj_t* root, const char* title)
{
    show_chats(root);
    auto* list = required(root, "telegram_chat_list");
    for (std::uint32_t index = 0; index < lv_obj_get_child_count(list); ++index) {
        auto* row        = lv_obj_get_child(list, index);
        const auto* name = lv_obj_get_name(row);
        if (name && std::strcmp(name, "telegram_chat_row") == 0 && contains_label(row, title)) {
            lv_obj_send_event(row, LV_EVENT_CLICKED, nullptr);
            advance_ui();
            return;
        }
    }
    std::fprintf(stderr, "Missing chat: %s\n", title);
    assert(false);
}

void assert_contained(lv_obj_t* object, lv_obj_t* parent)
{
    assert(object && parent);
    lv_area_t area{}, parent_area{};
    lv_obj_get_coords(object, &area);
    lv_obj_get_coords(parent, &parent_area);
    const bool contained = area.x1 >= parent_area.x1 && area.x2 <= parent_area.x2 && area.y1 >= parent_area.y1 &&
                           area.y2 <= parent_area.y2 && area.x1 <= area.x2 && area.y1 <= area.y2;
    if (!contained) {
        const char* object_name = lv_obj_get_name(object);
        if (!object_name && lv_obj_check_type(object, &lv_label_class)) object_name = lv_label_get_text(object);
        const char* parent_name = lv_obj_get_name(parent);
        std::fprintf(stderr, "%s [%d,%d,%d,%d] outside %s [%d,%d,%d,%d]\n", object_name ? object_name : "<unnamed>",
                     static_cast<int>(area.x1), static_cast<int>(area.y1), static_cast<int>(area.x2),
                     static_cast<int>(area.y2), parent_name ? parent_name : "<unnamed>",
                     static_cast<int>(parent_area.x1), static_cast<int>(parent_area.y1),
                     static_cast<int>(parent_area.x2), static_cast<int>(parent_area.y2));
    }
    assert(contained);
}

void assert_separate(lv_obj_t* first, lv_obj_t* second)
{
    lv_area_t a{}, b{};
    lv_obj_get_coords(first, &a);
    lv_obj_get_coords(second, &b);
    assert(a.x2 < b.x1 || b.x2 < a.x1 || a.y2 < b.y1 || b.y2 < a.y1);
}

const lv_color32_t& pixel_at(int x, int y)
{
    assert(x >= 0 && x < display_width && y >= 0);
    return framebuffer.at(static_cast<std::size_t>(y) * display_width + x);
}

bool close_rgb(const lv_color32_t& left, const lv_color32_t& right, int tolerance = 8)
{
    return std::abs(static_cast<int>(left.red) - right.red) <= tolerance &&
           std::abs(static_cast<int>(left.green) - right.green) <= tolerance &&
           std::abs(static_cast<int>(left.blue) - right.blue) <= tolerance;
}

void assert_avatar_pixels(lv_obj_t* avatar, int expected_size, bool photo, const char* initial = nullptr)
{
    assert(lv_obj_get_width(avatar) == expected_size);
    assert(lv_obj_get_height(avatar) == expected_size);
    assert_contained(avatar, lv_obj_get_parent(avatar));
    assert_contained(avatar, lv_screen_active());
    assert(lv_obj_get_style_radius(avatar, LV_PART_MAIN) >= expected_size / 2);
    lv_area_t bounds{};
    lv_obj_get_coords(avatar, &bounds);
    const auto background = pixel_at(bounds.x1 - 2, bounds.y1 + 1);
    for (const auto& point : std::array<std::array<int, 2>, 4>{{
             {{bounds.x1 + 1, bounds.y1 + 1}}, {{bounds.x2 - 1, bounds.y1 + 1}},
             {{bounds.x1 + 1, bounds.y2 - 1}}, {{bounds.x2 - 1, bounds.y2 - 1}},
         }}) {
        assert(close_rgb(pixel_at(point[0], point[1]), background));
    }
    if (photo) {
        auto* image = required(avatar, "telegram_avatar_image");
        assert(lv_obj_is_visible(image));
        assert_contained(image, avatar);
        assert(lv_obj_get_style_clip_corner(avatar, LV_PART_MAIN));
        const auto& green = pixel_at(bounds.x1 + expected_size / 5, bounds.y1 + expected_size / 2);
        const auto& blue = pixel_at(bounds.x1 + expected_size * 7 / 10, bounds.y1 + expected_size / 2);
        if (!(green.green > 200 && green.red < 50 && green.blue < 50 &&
              blue.blue > 200 && blue.red < 50 && blue.green < 50)) {
            std::fprintf(stderr, "Avatar %dpx crop pixels: left=%d,%d,%d right=%d,%d,%d\n", expected_size,
                         green.red, green.green, green.blue, blue.red, blue.green, blue.blue);
        }
        assert(green.green > 200 && green.red < 50 && green.blue < 50);
        assert(blue.blue > 200 && blue.red < 50 && blue.green < 50);
    } else {
        assert(!lv_obj_find_by_name(avatar, "telegram_avatar_image"));
        auto* label = required(avatar, "telegram_avatar_initial");
        assert(lv_obj_is_visible(label));
        assert_contained(label, avatar);
        assert(initial && std::strcmp(lv_label_get_text(label), initial) == 0);
        assert(region_has_contrast({bounds.x1 + expected_size / 4, bounds.y1 + expected_size / 4,
                                   bounds.x2 - expected_size / 4, bounds.y2 - expected_size / 4}));
    }
}

void assert_chat_avatars(lv_obj_t* root, lv_display_t* display)
{
    for (const auto* title : {"LILYGO Team", "Alice", "Saved Messages"}) {
        auto* row = named_row(root, "telegram_chat_row", title);
        assert(row);
        lv_obj_scroll_to_view_recursive(row, LV_ANIM_OFF);
        advance_ui();
        lv_refr_now(display);
        row = named_row(root, "telegram_chat_row", title);
        assert_avatar_pixels(required(row, "telegram_avatar"), 60, std::strcmp(title, "Saved Messages") != 0, "S");
    }
}

void assert_contact_avatars(lv_obj_t* root, lv_display_t* display)
{
    for (const auto* title : {"Alice", "Bob Chen"}) {
        auto* row = named_row(root, "telegram_contact_row", title);
        assert(row);
        lv_obj_scroll_to_view_recursive(row, LV_ANIM_OFF);
        advance_ui();
        lv_refr_now(display);
        row = named_row(root, "telegram_contact_row", title);
        assert_avatar_pixels(required(row, "telegram_avatar"), 52, std::strcmp(title, "Alice") == 0, "B");
    }
}

void assert_conversation_avatars(lv_obj_t* root, lv_display_t* display)
{
    auto* header = required(root, display_width >= 960 ? "telegram_conversation_header" : "telegram_header");
    lv_refr_now(display);
    assert_avatar_pixels(required(header, "telegram_avatar"), 42, true);
    const auto sender_size = display_width >= 960 ? 40 : 36;
    for (const auto* text : {"Good morning! Are we ready for the next test?", "A photo from the studio."}) {
        auto* row = named_row(root, "telegram_message_row", text);
        assert(row);
        auto* avatar = required(row, "telegram_sender_avatar");
        lv_obj_scroll_to_view_recursive(avatar, LV_ANIM_OFF);
        advance_ui();
        lv_refr_now(display);
        row = named_row(root, "telegram_message_row", text);
        avatar = required(row, "telegram_sender_avatar");
        assert_avatar_pixels(avatar, sender_size, std::strcmp(text, "A photo from the studio.") != 0, "D");
        assert_separate(avatar, required(row, "telegram_message_bubble"));
        assert_contained(required(row, "telegram_message_bubble"), row);
    }
    assert(count_named(required(root, "telegram_message_list"), "telegram_sender_avatar") == 2);
}

void assert_labels_contained(lv_obj_t* root)
{
    if (lv_obj_check_type(root, &lv_label_class)) assert_contained(root, lv_obj_get_parent(root));
    for (std::uint32_t index = 0; index < lv_obj_get_child_count(root); ++index) {
        assert_labels_contained(lv_obj_get_child(root, index));
    }
}

void assert_conversation_layout(lv_obj_t* root)
{
    auto* conversation = required(root, "telegram_conversation");
    auto* composer     = required(root, "telegram_composer");
    auto* send         = required(root, "telegram_send");
    auto* messages     = required(root, "telegram_message_list");
    auto* bar          = lv_obj_get_parent(composer);
    assert(lv_obj_is_visible(composer) && lv_obj_is_visible(send));
    assert(lv_obj_get_parent(send) == bar);
    assert_contained(conversation, required(root, "app_surface"));
    assert_contained(bar, conversation);
    assert_contained(composer, bar);
    assert_contained(send, bar);
    assert_contained(messages, conversation);
    assert_separate(composer, send);
    assert_separate(messages, bar);
    auto* keyboard = required(root, "telegram_keyboard");
    if (!lv_obj_has_flag(keyboard, LV_OBJ_FLAG_HIDDEN)) {
        assert_contained(keyboard, required(root, "app_surface"));
        assert_separate(bar, keyboard);
    }
}

void assert_history_available(lv_obj_t* root)
{
    auto* messages = required(root, "telegram_message_list");
    auto* older    = required(root, "telegram_load_older");
    lv_obj_scroll_to_view_recursive(older, LV_ANIM_OFF);
    advance_ui(50);
    assert(lv_obj_is_visible(older));
    assert_contained(older, messages);
    const auto count = count_named(messages, "telegram_message_bubble");
    click(root, "telegram_load_older");
    assert(count_named(required(root, "telegram_message_list"), "telegram_message_bubble") == count);
}

void capture(lv_display_t* display, const std::string& path, int width, int height)
{
    lv_tick_inc(20);
    lv_refr_now(display);
    const bool has_contrast = std::any_of(framebuffer.cbegin() + 1, framebuffer.cend(),
                                          [](const auto& pixel) { return colors_differ(pixel, framebuffer[0]); });
    assert(has_contrast);
    assert(write_ppm(path.c_str(), width, height));
}

void assert_peer_details_layout(lv_obj_t* root, bool conversation)
{
    auto* details = required(root, "telegram_peer_details");
    assert(lv_obj_is_visible(details));
    assert_contained(details, required(root, "app_surface"));
    if (conversation) {
        assert_contained(details, required(root, "telegram_conversation"));
        assert(!lv_obj_find_by_name(root, "telegram_message_list"));
        assert(!lv_obj_find_by_name(root, "telegram_composer"));
    }
    assert_labels_contained(details);
    assert_separate(required(details, "telegram_peer_name"), required(details, "telegram_peer_status"));
    assert(lv_obj_has_flag(required(root, "telegram_keyboard"), LV_OBJ_FLAG_HIDDEN));
}

void assert_alice_details(lv_obj_t* root)
{
    auto* details = required(root, "telegram_peer_details");
    assert(contains_label(required(details, "telegram_peer_name"), "Alice"));
    assert(contains_label(required(details, "telegram_peer_status"), "online"));
    assert(contains_label(required(details, "telegram_peer_username"), "@alice_studio"));
    assert(contains_label(required(details, "telegram_peer_phone"), "+15550100101"));
    assert(contains_label(required(details, "telegram_peer_description"), "Designing interfaces and sharing ideas."));
    assert(!lv_obj_find_by_name(details, "telegram_peer_members"));
}

void exercise_peer_details(lv_obj_t* root, lv_display_t* display, const std::string& output, int width, int height)
{
    for (const bool group : {true, false}) {
        const auto* title = group ? "LILYGO Team" : "Alice";
        const auto* draft = group ? "Team details draft" : "Alice details draft";
        open_chat(root, title);
        lv_textarea_set_text(required(root, "telegram_composer"), draft);
        auto* messages = required(root, "telegram_message_list");
        const auto message_count = count_named(messages, "telegram_message_bubble");
        lv_obj_scroll_to_y(messages, 0, LV_ANIM_OFF);
        advance_ui(50);
        const auto scroll_y = lv_obj_get_scroll_y(messages);

        click(root, "telegram_chat_info");
        assert_peer_details_layout(root, true);
        auto* details = required(root, "telegram_peer_details");
        assert(contains_label(required(details, "telegram_peer_name"), title));
        if (group) {
            assert(contains_label(required(details, "telegram_peer_status"), "24 members"));
            assert(contains_label(required(details, "telegram_peer_username"), "https://t.me/lilygo_team"));
            assert(contains_label(required(details, "telegram_peer_description"),
                                  "LILYGO hardware, firmware and interface development."));
            assert(contains_label(required(details, "telegram_peer_members"), "24"));
            assert(contains_label(required(details, "telegram_peer_type"), "Public Group"));
            assert(!lv_obj_find_by_name(details, "telegram_peer_phone"));
        } else {
            assert_alice_details(root);
        }
        capture(display, output + (group ? ".group-details.ppm" : ".private-details.ppm"), width, height);
        assert_avatar_pixels(required(details, "telegram_avatar"), 96, true);
        click(root, "telegram_peer_reload");
        assert_peer_details_layout(root, true);
        assert(contains_label(required(root, "telegram_peer_name"), title));
        if (!group) assert_alice_details(root);
        click(root, group ? "telegram_peer_back" : "telegram_peer_message");
        assert(!lv_obj_find_by_name(root, "telegram_peer_details"));
        assert_conversation_layout(root);
        assert(std::string(lv_textarea_get_text(required(root, "telegram_composer"))) == draft);
        messages = required(root, "telegram_message_list");
        assert(count_named(messages, "telegram_message_bubble") == message_count);
        assert(std::abs(lv_obj_get_scroll_y(messages) - scroll_y) <= 1);
        lv_textarea_set_text(required(root, "telegram_composer"), "");
    }
    open_chat(root, "LILYGO Team");
}

void exercise_account_pages(lv_obj_t* root, lv_display_t* display, const std::string& output,
                             int width, int height, std::vector<lv_color32_t>& draw_buffer)
{
    show_chats(root);
    click(root, "telegram_tab_contacts");
    auto* contacts = required(root, "telegram_contact_list");
    assert(count_named(contacts, "telegram_contact_row") == 3);
    assert_labels_contained(contacts);
    assert_contained(required(root, "telegram_contact_search"), required(root, "telegram_contacts"));
    assert_contact_avatars(root, display);
    auto* sort = required(root, "telegram_contacts_sort");
    lv_dropdown_set_selected(sort, 1);
    lv_obj_send_event(sort, LV_EVENT_VALUE_CHANGED, nullptr);
    advance_ui();
    assert(lv_dropdown_get_selected(required(root, "telegram_contacts_sort")) == 1);
    assert(contains_label(lv_obj_get_child(required(root, "telegram_contact_list"), 0), "Alice"));
    capture(display, output + ".contacts.ppm", width, height);
    lv_textarea_set_text(required(root, "telegram_contact_search"), "bOb");
    advance_ui();
    assert(count_named(required(root, "telegram_contact_list"), "telegram_contact_row") == 1);
    assert(contains_label(required(root, "telegram_contact_list"), "Bob Chen"));
    lv_textarea_set_text(required(root, "telegram_contact_search"), "missing contact");
    advance_ui();
    assert(count_named(required(root, "telegram_contact_list"), "telegram_contact_row") == 0);
    click(root, "telegram_contact_search_clear");

    auto* alice_details_row = named_row(root, "telegram_contact_row", "Alice");
    assert(alice_details_row);
    click(alice_details_row, "telegram_contact_info");
    assert_peer_details_layout(root, false);
    assert_alice_details(root);
    capture(display, output + ".contact-details.ppm", width, height);
    assert_avatar_pixels(required(required(root, "telegram_peer_details"), "telegram_avatar"), 96, true);
    click(root, "telegram_contact_message");
    assert(std::string(lv_textarea_get_text(required(root, "telegram_composer"))) == "Alice draft");
    show_chats(root);
    click(root, "telegram_tab_contacts");

    auto* bob = named_row(root, "telegram_contact_row", "Bob Chen");
    assert(bob);
    lv_obj_send_event(bob, LV_EVENT_CLICKED, nullptr);
    auto* alice = named_row(root, "telegram_contact_row", "Alice");
    assert(alice);
    lv_obj_send_event(alice, LV_EVENT_CLICKED, nullptr);
    advance_ui();
    assert(contains_label(required(root, "telegram_message_list"), "No messages"));
    lv_textarea_set_text(required(root, "telegram_composer"), "First message to Bob");
    click(root, "telegram_send");
    assert(contains_label(required(root, "telegram_message_list"), "First message to Bob"));
    show_chats(root);
    assert(named_row(root, "telegram_chat_row", "Bob Chen"));

    click(root, "telegram_tab_contacts");
    click(root, "telegram_contact_add");
    lv_textarea_set_text(required(root, "telegram_contact_first_name"), "Sam");
    lv_textarea_set_text(required(root, "telegram_contact_last_name"), "Test");
    lv_textarea_set_text(required(root, "telegram_contact_phone"), "+15550100999");
    click(root, "telegram_contact_save");
    assert(contains_label(root, "Contact added"));
    click(root, "telegram_back");
    auto* sam = named_row(root, "telegram_contact_row", "Sam Test");
    assert(sam);
    click(sam, "telegram_contact_info");
    assert(contains_label(required(root, "telegram_peer_phone"), "+15550100999"));
    click(root, "telegram_contact_remove");
    assert_contained(required(root, "telegram_account_dialog"), required(root, "app_surface"));
    click(root, "telegram_account_cancel");
    assert(required(root, "telegram_peer_name"));
    click(root, "telegram_contact_remove");
    click(root, "telegram_account_confirm");
    assert(contains_label(root, "Contact removed"));
    click(root, "telegram_back");
    assert(!named_row(root, "telegram_contact_row", "Sam Test"));
    assert(count_named(required(root, "telegram_contact_list"), "telegram_contact_row") == 3);

    click(root, "telegram_tab_settings");
    click(root, "telegram_settings_profile");
    assert(contains_label(required(root, "telegram_profile_name"), "Alex Morgan"));
    capture(display, output + ".profile.ppm", width, height);
    click(root, "telegram_profile_name");
    auto* first = required(root, "telegram_profile_first_name");
    lv_textarea_set_text(first, "Taylor");
    lv_textarea_set_text(required(root, "telegram_profile_last_name"), "River");
    // Type a new draft while the submitted profile update is still queued.
    lv_obj_send_event(required(root, "telegram_profile_save"), LV_EVENT_CLICKED, nullptr);
    lv_textarea_set_text(first, "Taylor pending draft");
    lv_obj_send_event(first, LV_EVENT_FOCUSED, nullptr);
    lv_textarea_set_cursor_pos(first, 4);
    lv_keyboard_set_mode(required(root, "telegram_keyboard"), LV_KEYBOARD_MODE_SPECIAL);
    advance_ui();
    first = required(root, "telegram_profile_first_name");
    assert(std::string(lv_textarea_get_text(first)) == "Taylor pending draft");
    assert(lv_textarea_get_cursor_pos(first) == 4);
    assert(lv_keyboard_get_textarea(required(root, "telegram_keyboard")) == first);
    for (const bool rotated : {true, false}) {
        display_width = rotated ? height : width;
        std::fill(framebuffer.begin(), framebuffer.end(), lv_color32_t{});
        lv_display_set_resolution(display, rotated ? height : width, rotated ? width : height);
        lv_display_set_buffers(display, draw_buffer.data(), nullptr,
                               static_cast<std::uint32_t>(draw_buffer.size() * sizeof(lv_color32_t)),
                               LV_DISPLAY_RENDER_MODE_PARTIAL);
        advance_ui();
        first = required(root, "telegram_profile_first_name");
        auto* keyboard = required(root, "telegram_keyboard");
        assert(std::string(lv_textarea_get_text(first)) == "Taylor pending draft");
        assert(std::string(lv_textarea_get_text(required(root, "telegram_profile_last_name"))) == "River");
        assert(lv_textarea_get_cursor_pos(first) == 4);
        assert(lv_keyboard_get_textarea(keyboard) == first);
        assert(lv_keyboard_get_mode(keyboard) == LV_KEYBOARD_MODE_SPECIAL);
        assert(!lv_obj_has_flag(keyboard, LV_OBJ_FLAG_HIDDEN));
        assert_contained(keyboard, required(root, "app_surface"));
    }
    lv_textarea_set_text(required(root, "telegram_profile_first_name"), "Taylor");
    lv_obj_send_event(required(root, "telegram_keyboard"), LV_EVENT_CANCEL, nullptr);
    click(root, "telegram_profile_save");
    click(root, "telegram_back");
    assert(contains_label(required(root, "telegram_profile_name"), "Taylor River"));
    click(root, "telegram_profile_username");
    lv_textarea_set_text(required(root, "telegram_profile_username_input"), "taken_username");
    click(root, "telegram_profile_save");
    assert(contains_label(required(root, "telegram_account_error"), "This username is already taken"));
    assert(std::string(lv_textarea_get_text(required(root, "telegram_profile_username_input"))) == "taken_username");
    lv_textarea_set_text(required(root, "telegram_profile_username_input"), "taylor_river");
    click(root, "telegram_profile_save");
    click(root, "telegram_back");
    assert(contains_label(required(root, "telegram_profile_username"), "@taylor_river"));
    click(root, "telegram_profile_bio");
    lv_textarea_set_text(required(root, "telegram_profile_bio_input"), "At the workshop.");
    click(root, "telegram_profile_save");
    click(root, "telegram_back");
    assert(contains_label(required(root, "telegram_profile_bio"), "At the workshop."));
    click(root, "telegram_back");

    click(root, "telegram_settings_notifications");
    click(root, "telegram_notifications_private");
    assert(lv_obj_has_state(required(root, "telegram_notification_enabled"), LV_STATE_CHECKED));
    set_switch(root, "telegram_notification_enabled", false);
    assert(lv_obj_has_state(required(root, "telegram_notification_preview"), LV_STATE_CHECKED));
    set_switch(root, "telegram_notification_preview", false);
    set_switch(root, "telegram_notification_sound", false);
    capture(display, output + ".notifications.ppm", width, height);
    click(root, "telegram_back");
    click(root, "telegram_notifications_groups");
    assert(lv_obj_has_state(required(root, "telegram_notification_enabled"), LV_STATE_CHECKED));
    set_switch(root, "telegram_notification_preview", false);
    click(root, "telegram_back");
    click(root, "telegram_notifications_channels");
    assert(lv_obj_has_state(required(root, "telegram_notification_preview"), LV_STATE_CHECKED));
    click(root, "telegram_back");
    click(root, "telegram_notifications_private");
    for (const auto* name : {"telegram_notification_enabled", "telegram_notification_preview", "telegram_notification_sound"}) {
        assert(!lv_obj_has_state(required(root, name), LV_STATE_CHECKED));
    }
    click(root, "telegram_back");
    click(root, "telegram_back");

    click(root, "telegram_settings_privacy");
    click(root, "telegram_privacy_phone");
    assert(lv_obj_has_state(required(root, "telegram_privacy_contacts"), LV_STATE_CHECKED));
    click(root, "telegram_privacy_nobody");
    assert(lv_obj_has_state(required(root, "telegram_privacy_nobody"), LV_STATE_CHECKED));
    click(root, "telegram_back");
    click(root, "telegram_privacy_last_seen");
    click(root, "telegram_privacy_everybody");
    assert(lv_obj_has_state(required(root, "telegram_privacy_everybody"), LV_STATE_CHECKED));
    click(root, "telegram_back");
    click(root, "telegram_privacy_photo");
    click(root, "telegram_privacy_contacts");
    assert(lv_obj_has_state(required(root, "telegram_privacy_contacts"), LV_STATE_CHECKED));
    click(root, "telegram_back");
    click(root, "telegram_privacy_phone");
    assert(lv_obj_has_state(required(root, "telegram_privacy_nobody"), LV_STATE_CHECKED));
    click(root, "telegram_back");
    click(root, "telegram_privacy_blocked");
    assert(count_named(root, "telegram_blocked_unblock") == 2);
    click(root, "telegram_blocked_unblock");
    click(root, "telegram_account_cancel");
    assert(count_named(root, "telegram_blocked_unblock") == 2);
    click(root, "telegram_blocked_unblock");
    click(root, "telegram_account_confirm");
    assert(count_named(root, "telegram_blocked_unblock") == 1);
    click(root, "telegram_blocked_unblock");
    click(root, "telegram_account_confirm");
    assert(count_named(root, "telegram_blocked_unblock") == 0);
    assert(contains_label(root, "No blocked users"));
    click(root, "telegram_back");
    click(root, "telegram_back");

    click(root, "telegram_settings_devices");
    auto* current = required(root, "telegram_session_current");
    assert(!lv_obj_has_flag(current, LV_OBJ_FLAG_CLICKABLE));
    lv_obj_send_event(current, LV_EVENT_CLICKED, nullptr);
    advance_ui();
    assert(!lv_obj_find_by_name(root, "telegram_account_dialog"));
    assert(count_named(root, "telegram_session_terminate") == 1);
    assert_labels_contained(required(root, "telegram_settings"));
    capture(display, output + ".devices.ppm", width, height);
    click(root, "telegram_session_terminate");
    click(root, "telegram_account_cancel");
    assert(count_named(root, "telegram_session_terminate") == 1);
    click(root, "telegram_session_terminate");
    click(root, "telegram_account_confirm");
    assert(count_named(root, "telegram_session_terminate") == 0);
    assert(required(root, "telegram_session_current"));
    click(root, "telegram_back");

    click(root, "telegram_settings_storage");
    assert(contains_label(required(root, "telegram_storage_media"), "24.0 MB"));
    capture(display, output + ".storage.ppm", width, height);
    set_switch(root, "telegram_storage_auto_photos", false);
    click(root, "telegram_storage_clear");
    click(root, "telegram_account_cancel");
    assert(contains_label(required(root, "telegram_storage_media"), "24.0 MB"));
    click(root, "telegram_storage_clear");
    click(root, "telegram_account_confirm");
    assert(contains_label(required(root, "telegram_storage_media"), "0 B"));
    assert(contains_label(required(root, "telegram_storage_files"), "0"));
    assert(contains_label(required(root, "telegram_storage_database"), "4.0 MB"));
    click(root, "telegram_back");

    click(root, "telegram_settings_appearance");
    auto* font_size = required(root, "telegram_appearance_font_size");
    lv_dropdown_set_selected(font_size, 2);
    lv_obj_send_event(font_size, LV_EVENT_VALUE_CHANGED, nullptr);
    advance_ui();
    assert(lv_dropdown_get_selected(required(root, "telegram_appearance_font_size")) == 2);
    auto* preview_text = lv_obj_get_child(required(root, "telegram_appearance_preview"), 0);
    assert(lv_obj_get_style_text_font(preview_text, LV_PART_MAIN) == lilygo_ui_font_get(26));
    click(root, "telegram_back");
    click(root, "telegram_settings_about");
    assert(required(root, "telegram_about_version"));
    assert(contains_label(required(root, "telegram_about_client"), "TDLib"));
    click(root, "telegram_back");
    click(root, "telegram_settings_saved");
    assert(required(root, "telegram_message_list"));
    show_chats(root);
}

}  // namespace

int main(int argc, char** argv)
{
    assert(argc == 4);
    const int width  = std::atoi(argv[1]);
    const int height = std::atoi(argv[2]);
    assert(width >= 320 && height >= 240);
    const auto timestamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto test_data = std::filesystem::temp_directory_path() /
        ("telegram-render-" + std::to_string(timestamp) + "-" + std::to_string(std::random_device{}()));
    assert(std::filesystem::create_directory(test_data));
    const auto avatar_fixture = test_data / "avatar-fixture.png";
    write_avatar_fixture(avatar_fixture);
    assert(setenv("TELEGRAM_DEMO", "1", 1) == 0);
    assert(setenv("TELEGRAM_DEMO_IMAGE", avatar_fixture.c_str(), 1) == 0);
    assert(setenv("XDG_DATA_HOME", test_data.c_str(), 1) == 0);

    display_width = width;
    framebuffer.resize(static_cast<std::size_t>(width) * static_cast<std::size_t>(height));
    std::vector<lv_color32_t> draw_buffer(static_cast<std::size_t>(std::max(width, height)) * kPartialRows);

    lv_init();
    auto* display = lv_display_create(width, height);
    assert(display);
    lv_display_set_buffers(display, draw_buffer.data(), nullptr,
                           static_cast<std::uint32_t>(draw_buffer.size() * sizeof(lv_color32_t)),
                           LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(display, flush_display);
    lv_display_set_default(display);

    cm0_app_context_t context{};
    context.root           = lv_screen_active();
    const auto* descriptor = cm0_app_get_descriptor();
    descriptor->open(&context);
    advance_ui();

    auto* surface = required(context.root, "app_surface");
    auto* header  = required(context.root, "telegram_header");
    auto* title   = required(context.root, "telegram_header_title");
    auto* sidebar = required(context.root, "telegram_chat_sidebar");
    auto* list    = required(context.root, "telegram_chat_list");
    assert(lv_color_eq(lv_obj_get_style_bg_color(surface, LV_PART_MAIN), lv_color_hex(kPageBackground)));
    assert(lv_color_eq(lv_obj_get_style_bg_color(header, LV_PART_MAIN), lv_color_hex(kHeaderBackground)));
    assert(std::string(lv_label_get_text(title)) == "Chats");
    assert(count_named(list, "telegram_chat_row") == 3);
    assert_labels_contained(list);
    assert_contained(sidebar, surface);
    assert_contained(required(context.root, "telegram_chat_search"), sidebar);
    assert_contained(required(context.root, "telegram_tab_chats"), surface);
    assert_contained(required(context.root, "telegram_tab_settings"), surface);
    assert_separate(required(context.root, "telegram_tab_chats"), required(context.root, "telegram_tab_settings"));
    assert_chat_avatars(context.root, display);
    capture(display, std::string(argv[3]) + ".chats.ppm", width, height);

    lv_textarea_set_text(required(context.root, "telegram_chat_search"), "aLiCe");
    advance_ui();
    list = required(context.root, "telegram_chat_list");
    assert(count_named(list, "telegram_chat_row") == 1);
    assert(contains_label(list, "Alice"));
    lv_textarea_set_text(required(context.root, "telegram_chat_search"), "No matching local chat");
    advance_ui();
    assert(count_named(required(context.root, "telegram_chat_list"), "telegram_chat_row") == 0);
    assert(lv_obj_is_visible(required(context.root, "telegram_search_clear")));
    click(context.root, "telegram_search_clear");
    assert(std::string(lv_textarea_get_text(required(context.root, "telegram_chat_search"))).empty());
    assert(count_named(required(context.root, "telegram_chat_list"), "telegram_chat_row") == 3);

    auto* search = required(context.root, "telegram_chat_search");
    lv_obj_send_event(search, LV_EVENT_FOCUSED, nullptr);
    lv_keyboard_set_mode(required(context.root, "telegram_keyboard"), LV_KEYBOARD_MODE_NUMBER);
    lv_textarea_set_text(search, "100");
    lv_textarea_set_cursor_pos(search, 1);
    advance_ui();
    search         = required(context.root, "telegram_chat_search");
    auto* keyboard = required(context.root, "telegram_keyboard");
    assert(!lv_obj_has_flag(keyboard, LV_OBJ_FLAG_HIDDEN));
    assert(lv_keyboard_get_textarea(keyboard) == search);
    assert(lv_keyboard_get_mode(keyboard) == LV_KEYBOARD_MODE_NUMBER);
    assert(std::string(lv_textarea_get_text(search)) == "100");
    assert(lv_textarea_get_cursor_pos(search) == 1);
    click(context.root, "telegram_search_clear");
    search = required(context.root, "telegram_chat_search");
    assert(lv_keyboard_get_textarea(keyboard) == search);
    assert(lv_keyboard_get_mode(keyboard) == LV_KEYBOARD_MODE_NUMBER);
    assert(std::string(lv_textarea_get_text(search)).empty());
    assert(count_named(required(context.root, "telegram_chat_list"), "telegram_chat_row") == 3);
    lv_obj_send_event(keyboard, LV_EVENT_CANCEL, nullptr);
    advance_ui();

    click(context.root, "telegram_tab_settings");
    assert(lv_obj_is_visible(required(context.root, "telegram_settings")));
    capture(display, std::string(argv[3]) + ".settings.ppm", width, height);
    click(context.root, "telegram_logout");
    auto* dialog = required(context.root, "telegram_logout_dialog");
    assert(lv_obj_is_visible(dialog));
    assert_contained(dialog, surface);
    assert_contained(required(context.root, "telegram_logout_confirm"), dialog);
    assert_contained(required(context.root, "telegram_logout_cancel"), dialog);
    assert_separate(required(context.root, "telegram_logout_confirm"),
                    required(context.root, "telegram_logout_cancel"));
    click(context.root, "telegram_logout_cancel");
    assert(!lv_obj_find_by_name(context.root, "telegram_logout_dialog"));
    assert(lv_obj_is_visible(required(context.root, "telegram_settings")));
    click(context.root, "telegram_tab_chats");
    assert(count_named(required(context.root, "telegram_chat_list"), "telegram_chat_row") == 3);

    open_chat(context.root, "LILYGO Team");
    auto* messages = required(context.root, "telegram_message_list");
    assert(count_named(messages, "telegram_message_bubble") == 3);
    assert(count_named(messages, "telegram_message_row") == 3);
    assert(count_named(messages, "telegram_date_separator") >= 1);
    assert(contains_label(messages, "LILYGO Team"));
    assert(contains_label(messages, "Design Notes"));
    assert(required(context.root, "telegram_photo"));
    if (width < 960) assert(!lv_obj_find_by_name(context.root, "telegram_chat_sidebar"));
    assert_conversation_layout(context.root);
    assert_conversation_avatars(context.root, display);
    assert_history_available(context.root);

    exercise_peer_details(context.root, display, argv[3], width, height);

    auto* composer = required(context.root, "telegram_composer");
    assert(std::string(lv_textarea_get_text(composer)).empty());
    assert(lv_obj_has_state(required(context.root, "telegram_send"), LV_STATE_DISABLED));
    lv_textarea_set_text(composer, "Team draft");
    open_chat(context.root, "Alice");
    messages = required(context.root, "telegram_message_list");
    assert(count_named(messages, "telegram_message_bubble") == 1);
    assert(!contains_label(messages, "Alice"));
    assert(count_named(messages, "telegram_sender_avatar") == 1);
    composer = required(context.root, "telegram_composer");
    assert(std::string(lv_textarea_get_text(composer)).empty());
    lv_textarea_set_text(composer, "Alice draft");
    open_chat(context.root, "LILYGO Team");
    composer = required(context.root, "telegram_composer");
    assert(std::string(lv_textarea_get_text(composer)) == "Team draft");

    lv_textarea_set_text(composer, "Rendered from the simulator");
    assert(!lv_obj_has_state(required(context.root, "telegram_send"), LV_STATE_DISABLED));
    lv_obj_send_event(required(context.root, "telegram_send"), LV_EVENT_CLICKED, nullptr);
    assert(std::string(lv_textarea_get_text(composer)).empty());
    assert(lv_obj_has_state(required(context.root, "telegram_send"), LV_STATE_DISABLED));

    // Start the next draft before the queued delivery event rebuilds the conversation.
    lv_textarea_set_text(composer, "Draft while delivery updates");
    lv_obj_send_event(composer, LV_EVENT_FOCUSED, nullptr);
    keyboard = required(context.root, "telegram_keyboard");
    lv_keyboard_set_mode(keyboard, LV_KEYBOARD_MODE_SPECIAL);
    lv_textarea_set_cursor_pos(composer, 5);
    advance_ui();
    messages = required(context.root, "telegram_message_list");
    assert(count_named(messages, "telegram_message_bubble") == 4);
    assert(contains_label(messages, "Rendered from the simulator"));
    composer = required(context.root, "telegram_composer");
    assert(std::string(lv_textarea_get_text(composer)) == "Draft while delivery updates");
    assert(!lv_obj_has_flag(keyboard, LV_OBJ_FLAG_HIDDEN));
    assert(lv_keyboard_get_textarea(keyboard) == composer);
    assert(lv_keyboard_get_mode(keyboard) == LV_KEYBOARD_MODE_SPECIAL);
    assert(lv_textarea_get_cursor_pos(composer) == 5);
    lv_obj_send_event(keyboard, LV_EVENT_CANCEL, nullptr);
    lv_textarea_set_text(composer, "");
    advance_ui();
    assert(std::string(lv_textarea_get_text(required(context.root, "telegram_composer"))).empty());
    assert(lv_obj_has_state(required(context.root, "telegram_send"), LV_STATE_DISABLED));

    open_chat(context.root, "Alice");
    assert(std::string(lv_textarea_get_text(required(context.root, "telegram_composer"))) == "Alice draft");
    open_chat(context.root, "LILYGO Team");
    composer = required(context.root, "telegram_composer");
    assert(std::string(lv_textarea_get_text(composer)).empty());
    lv_textarea_set_text(composer, "Draft survives rotation");
    lv_obj_send_event(composer, LV_EVENT_FOCUSED, nullptr);
    advance_ui();
    keyboard = required(context.root, "telegram_keyboard");
    lv_keyboard_set_mode(keyboard, LV_KEYBOARD_MODE_SPECIAL);
    lv_textarea_set_cursor_pos(composer, 6);
    assert(lv_keyboard_get_textarea(keyboard) == composer);
    assert_conversation_layout(context.root);

    // Reconfigure the partial buffer before rendering each new display resolution.
    for (const bool rotated : {true, false}) {
        const int current_width  = rotated ? height : width;
        const int current_height = rotated ? width : height;
        display_width            = current_width;
        std::fill(framebuffer.begin(), framebuffer.end(), lv_color32_t{});
        lv_display_set_resolution(display, current_width, current_height);
        lv_display_set_buffers(display, draw_buffer.data(), nullptr,
                               static_cast<std::uint32_t>(draw_buffer.size() * sizeof(lv_color32_t)),
                               LV_DISPLAY_RENDER_MODE_PARTIAL);
        advance_ui();
        composer = required(context.root, "telegram_composer");
        assert(std::string(lv_textarea_get_text(composer)) == "Draft survives rotation");
        keyboard = required(context.root, "telegram_keyboard");
        assert(!lv_obj_has_flag(keyboard, LV_OBJ_FLAG_HIDDEN));
        assert(lv_keyboard_get_textarea(keyboard) == composer);
        assert(lv_keyboard_get_mode(keyboard) == LV_KEYBOARD_MODE_SPECIAL);
        assert(lv_textarea_get_cursor_pos(composer) == 6);
        assert_conversation_layout(context.root);
        lv_obj_send_event(keyboard, LV_EVENT_CANCEL, nullptr);
        advance_ui();
        assert_history_available(context.root);
        assert_conversation_avatars(context.root, display);
        composer = required(context.root, "telegram_composer");
        lv_obj_send_event(composer, LV_EVENT_FOCUSED, nullptr);
        advance_ui();
        lv_keyboard_set_mode(keyboard, LV_KEYBOARD_MODE_SPECIAL);
        lv_textarea_set_cursor_pos(composer, 6);
    }
    lv_obj_send_event(required(context.root, "telegram_keyboard"), LV_EVENT_CANCEL, nullptr);
    lv_textarea_set_text(required(context.root, "telegram_composer"), "");
    advance_ui();
    assert_conversation_layout(context.root);

    auto* photo = required(context.root, "telegram_photo");
    messages    = required(context.root, "telegram_message_list");
    lv_obj_scroll_to_view_recursive(photo, LV_ANIM_OFF);
    advance_ui(50);
    capture(display, argv[3], width, height);
    PixelRegion visible_photo{};
    assert(visible_object_region(photo, messages, height, &visible_photo));
    assert(region_has_contrast(visible_photo));
    lv_obj_scroll_to_y(messages, LV_COORD_MAX, LV_ANIM_OFF);
    advance_ui(50);
    capture(display, std::string(argv[3]) + ".latest.ppm", width, height);

    exercise_account_pages(context.root, display, argv[3], width, height, draw_buffer);
    const auto preferences = telegram::load_ui_preferences((test_data / "lilygo-ui-telegram/ui-preferences.json").string());
    assert(preferences.message_font_size == 26);
    assert(!preferences.auto_download_photos);
    assert(preferences.contacts_sort_online);

    // A compact handset viewport must preserve avatar hit areas and keep sender images outside bubbles.
    display_width = 360;
    framebuffer.resize(360U * 640U);
    lv_display_set_resolution(display, 360, 640);
    lv_display_set_buffers(display, draw_buffer.data(), nullptr,
                           static_cast<std::uint32_t>(draw_buffer.size() * sizeof(lv_color32_t)),
                           LV_DISPLAY_RENDER_MODE_PARTIAL);
    advance_ui();
    show_chats(context.root);
    assert_chat_avatars(context.root, display);
    click(context.root, "telegram_tab_contacts");
    assert_contact_avatars(context.root, display);
    click(context.root, "telegram_tab_chats");
    open_chat(context.root, "LILYGO Team");
    assert_conversation_avatars(context.root, display);
    assert_conversation_layout(context.root);
    capture(display, std::string(argv[3]) + ".avatars-compact.ppm", 360, 640);
    exercise_peer_details(context.root, display, std::string(argv[3]) + ".compact", 360, 640);
    display_width = width;
    framebuffer.resize(static_cast<std::size_t>(width) * height);
    lv_display_set_resolution(display, width, height);
    lv_display_set_buffers(display, draw_buffer.data(), nullptr,
                           static_cast<std::uint32_t>(draw_buffer.size() * sizeof(lv_color32_t)),
                           LV_DISPLAY_RENDER_MODE_PARTIAL);
    advance_ui();

    show_chats(context.root);
    click(context.root, "telegram_tab_settings");
    click(context.root, "telegram_logout");
    click(context.root, "telegram_logout_confirm");
    assert(lv_obj_is_visible(required(context.root, "telegram_auth_panel")));
    assert(std::string(lv_label_get_text(required(context.root, "telegram_auth_title"))) == "Sign in to Telegram");
    assert(lv_obj_find_by_name(context.root, "telegram_auth_input"));
    assert(lv_obj_find_by_name(context.root, "telegram_auth_submit"));
    assert(!lv_obj_find_by_name(context.root, "telegram_chat_list"));
    assert(!lv_obj_find_by_name(context.root, "telegram_composer"));
    assert(!lv_obj_find_by_name(context.root, "telegram_settings"));
    assert(!lv_obj_find_by_name(context.root, "telegram_logout_dialog"));
    assert(lv_obj_has_flag(required(context.root, "telegram_keyboard"), LV_OBJ_FLAG_HIDDEN));

    descriptor->close();
    descriptor->open(&context);
    advance_ui();
    click(context.root, "telegram_tab_contacts");
    assert(lv_dropdown_get_selected(required(context.root, "telegram_contacts_sort")) == 1);
    click(context.root, "telegram_tab_settings");
    click(context.root, "telegram_settings_appearance");
    assert(lv_dropdown_get_selected(required(context.root, "telegram_appearance_font_size")) == 2);
    click(context.root, "telegram_back");
    click(context.root, "telegram_settings_storage");
    assert(!lv_obj_has_state(required(context.root, "telegram_storage_auto_photos"), LV_STATE_CHECKED));
    descriptor->close();

    const auto broken_avatar = test_data / "broken-avatar.png";
    write_avatar_fixture(broken_avatar);
    std::filesystem::resize_file(broken_avatar, 33);
    assert(setenv("TELEGRAM_DEMO_IMAGE", broken_avatar.c_str(), 1) == 0);
    descriptor->open(&context);
    advance_ui(600);
    auto* broken_row = named_row(context.root, "telegram_chat_row", "LILYGO Team");
    assert(broken_row);
    lv_obj_scroll_to_view_recursive(broken_row, LV_ANIM_OFF);
    advance_ui();
    lv_refr_now(display);
    broken_row = named_row(context.root, "telegram_chat_row", "LILYGO Team");
    assert_avatar_pixels(required(broken_row, "telegram_avatar"), 60, false, "L");
    open_chat(context.root, "LILYGO Team");
    advance_ui(600);
    lv_refr_now(display);
    auto* broken_header = required(context.root, width >= 960 ? "telegram_conversation_header" : "telegram_header");
    assert_avatar_pixels(required(broken_header, "telegram_avatar"), 42, false, "L");
    assert_conversation_layout(context.root);
    descriptor->close();
    lv_display_delete(display);
    lilygo_ui_fonts_deinit();
    lv_deinit();
    std::filesystem::remove_all(test_data);
    return 0;
}
