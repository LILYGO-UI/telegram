#include "pages/counter/counter_model.hpp"

#include <cassert>

namespace {

void test_model()
{
    CounterModel model;
    assert(model.value() == 0);
    model.decrement();
    assert(model.value() == 0);
    model.increment();
    assert(model.value() == 1);
    model.reset();
    assert(model.value() == 0);
    for (int i = 0; i < 1100; ++i) model.increment();
    assert(model.value() == 999);
}

}  // namespace

int main()
{
    test_model();
    return 0;
}
