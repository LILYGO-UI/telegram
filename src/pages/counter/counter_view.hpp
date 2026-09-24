#ifndef LILYGO_UI_TEMPLATE_PAGES_COUNTER_COUNTER_VIEW_HPP
#define LILYGO_UI_TEMPLATE_PAGES_COUNTER_COUNTER_VIEW_HPP

#include "pages/counter/counter_view_model.hpp"

#include <lvgl.h>

class CounterView {
public:
    explicit CounterView(CounterViewModel &view_model) noexcept;

    lv_obj_t *create(lv_obj_t *parent, const char *application_name);
    void destroy() noexcept;

private:
    static void decrement_event(lv_event_t *event);
    static void reset_event(lv_event_t *event);
    static void increment_event(lv_event_t *event);
    static void size_changed_event(lv_event_t *event);

    void apply_layout() noexcept;

    CounterViewModel &view_model_;
    lv_obj_t *surface_          = nullptr;
    lv_obj_t *panel_            = nullptr;
    lv_obj_t *controls_         = nullptr;
    lv_obj_t *decrement_button_ = nullptr;
    lv_obj_t *reset_button_     = nullptr;
    lv_obj_t *increment_button_ = nullptr;
};

#endif
