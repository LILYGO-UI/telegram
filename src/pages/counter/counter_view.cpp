#include "pages/counter/counter_view.hpp"

#include "components/components.hpp"

#include <cm0/typography.h>

CounterView::CounterView(CounterViewModel &view_model) noexcept : view_model_(view_model)
{
}

lv_obj_t *CounterView::create(lv_obj_t *parent, const char *application_name)
{
    surface_ = components::create_page(parent);
    lv_obj_add_event_cb(surface_, size_changed_event, LV_EVENT_SIZE_CHANGED, this);
    components::create_header(surface_, application_name);

    auto *body = lv_obj_create(surface_);
    lv_obj_set_width(body, LV_PCT(100));
    lv_obj_set_flex_grow(body, 1);
    lv_obj_set_style_border_width(body, 0, 0);
    lv_obj_set_style_radius(body, 0, 0);
    lv_obj_set_style_bg_opa(body, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(body, 24, 0);
    lv_obj_set_style_pad_bottom(body, 52, 0);
    lv_obj_clear_flag(body, LV_OBJ_FLAG_SCROLLABLE);

    panel_ = components::create_card(body);
    lv_obj_set_name_static(panel_, "counter_panel");
    lv_obj_set_height(panel_, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_row(panel_, 24, 0);
    lv_obj_set_flex_flow(panel_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(panel_, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_center(panel_);

    auto *counter_title = lv_label_create(panel_);
    lv_obj_set_name_static(counter_title, "counter_title");
    lv_label_set_text(counter_title, "Counter");
    lv_obj_set_style_text_font(counter_title, lilygo_ui_font_get(22), 0);
    lv_obj_set_style_text_color(counter_title, lv_color_hex(components::color_container_title), 0);

    auto *value_label = lv_label_create(panel_);
    lv_obj_set_name_static(value_label, "counter_value");
    lv_obj_set_style_text_font(value_label, lilygo_ui_font_get(48), 0);
    lv_obj_set_style_text_color(value_label, lv_color_hex(components::color_text), 0);
    lv_label_bind_text(value_label, view_model_.counter_subject(), "%d");

    controls_ = lv_obj_create(panel_);
    lv_obj_set_size(controls_, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_border_width(controls_, 0, 0);
    lv_obj_set_style_bg_opa(controls_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(controls_, 0, 0);
    lv_obj_set_style_pad_column(controls_, 12, 0);
    lv_obj_clear_flag(controls_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(controls_, LV_FLEX_FLOW_ROW);

    decrement_button_ = components::create_button(controls_, "-", decrement_event, this);
    lv_obj_set_name_static(decrement_button_, "counter_decrement");
    reset_button_ = components::create_button(controls_, "Reset", reset_event, this);
    lv_obj_set_name_static(reset_button_, "counter_reset");
    increment_button_ = components::create_button(controls_, "+", increment_event, this);
    lv_obj_set_name_static(increment_button_, "counter_increment");
    apply_layout();
    return surface_;
}

void CounterView::destroy() noexcept
{
    if (surface_) lv_obj_delete(surface_);
    surface_          = nullptr;
    panel_            = nullptr;
    controls_         = nullptr;
    decrement_button_ = nullptr;
    reset_button_     = nullptr;
    increment_button_ = nullptr;
}

void CounterView::decrement_event(lv_event_t *event)
{
    static_cast<CounterView *>(lv_event_get_user_data(event))->view_model_.decrement();
}

void CounterView::reset_event(lv_event_t *event)
{
    static_cast<CounterView *>(lv_event_get_user_data(event))->view_model_.reset();
}

void CounterView::increment_event(lv_event_t *event)
{
    static_cast<CounterView *>(lv_event_get_user_data(event))->view_model_.increment();
}

void CounterView::size_changed_event(lv_event_t *event)
{
    static_cast<CounterView *>(lv_event_get_user_data(event))->apply_layout();
}

void CounterView::apply_layout() noexcept
{
    if (!surface_ || !panel_ || !controls_ || !decrement_button_ || !reset_button_ || !increment_button_) return;

    const auto width   = lv_obj_get_content_width(surface_);
    const bool compact = width < 400;
    const bool wide    = width >= 800;
    lv_obj_set_width(panel_, compact ? LV_PCT(94) : (wide ? 560 : LV_PCT(92)));
    lv_obj_set_style_pad_all(panel_, wide ? 40 : (compact ? 16 : 28), 0);
    lv_obj_set_style_pad_column(controls_, compact ? 8 : 12, 0);
    lv_obj_set_width(decrement_button_, compact ? 56 : 64);
    lv_obj_set_width(reset_button_, compact ? 96 : 120);
    lv_obj_set_width(increment_button_, compact ? 56 : 64);
}
