#ifndef LILYGO_UI_TEMPLATE_PAGES_COUNTER_COUNTER_VIEW_MODEL_HPP
#define LILYGO_UI_TEMPLATE_PAGES_COUNTER_COUNTER_VIEW_MODEL_HPP

#include "components/lv_subject.hpp"
#include "pages/counter/counter_model.hpp"

class CounterViewModel {
public:
    CounterViewModel();

    [[nodiscard]] int counter() const noexcept;
    [[nodiscard]] lv_subject_t *counter_subject() noexcept;

    void increment() noexcept;
    void decrement() noexcept;
    void reset() noexcept;

private:
    void publish() noexcept;

    CounterModel model_;
    IntSubject counter_;
};

#endif
