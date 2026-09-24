#include "pages/counter/counter_model.hpp"

int CounterModel::value() const noexcept
{
    return value_;
}

void CounterModel::increment() noexcept
{
    if (value_ < maximum) ++value_;
}

void CounterModel::decrement() noexcept
{
    if (value_ > minimum) --value_;
}

void CounterModel::reset() noexcept
{
    value_ = minimum;
}
