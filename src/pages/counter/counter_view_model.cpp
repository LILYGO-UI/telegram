#include "pages/counter/counter_view_model.hpp"

CounterViewModel::CounterViewModel() : counter_(model_.value(), CounterModel::minimum, CounterModel::maximum)
{
}

int CounterViewModel::counter() const noexcept
{
    return model_.value();
}

lv_subject_t *CounterViewModel::counter_subject() noexcept
{
    return counter_.get();
}

void CounterViewModel::increment() noexcept
{
    model_.increment();
    publish();
}

void CounterViewModel::decrement() noexcept
{
    model_.decrement();
    publish();
}

void CounterViewModel::reset() noexcept
{
    model_.reset();
    publish();
}

void CounterViewModel::publish() noexcept
{
    counter_.set(model_.value());
}
