#ifndef LILYGO_UI_TEMPLATE_PAGES_COUNTER_COUNTER_MODEL_HPP
#define LILYGO_UI_TEMPLATE_PAGES_COUNTER_COUNTER_MODEL_HPP

class CounterModel {
public:
    static constexpr int minimum = 0;
    static constexpr int maximum = 999;

    [[nodiscard]] int value() const noexcept;

    void increment() noexcept;
    void decrement() noexcept;
    void reset() noexcept;

private:
    int value_ = minimum;
};

#endif
