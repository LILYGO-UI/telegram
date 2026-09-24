#include "pages/counter/counter_view_model.hpp"

#include <cassert>

int main()
{
    lv_init();

    {
        CounterViewModel view_model;
        assert(view_model.counter() == 0);
        assert(lv_subject_get_int(view_model.counter_subject()) == 0);

        view_model.decrement();
        assert(view_model.counter() == 0);
        view_model.increment();
        assert(view_model.counter() == 1);
        assert(lv_subject_get_int(view_model.counter_subject()) == 1);
        view_model.reset();
        assert(view_model.counter() == 0);
        assert(lv_subject_get_int(view_model.counter_subject()) == 0);
    }

    lv_deinit();
    return 0;
}
