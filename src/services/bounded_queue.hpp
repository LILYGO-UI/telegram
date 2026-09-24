#ifndef LILYGO_UI_TELEGRAM_SERVICES_BOUNDED_QUEUE_HPP
#define LILYGO_UI_TELEGRAM_SERVICES_BOUNDED_QUEUE_HPP

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <mutex>
#include <stdexcept>
#include <utility>

namespace telegram {

template <typename T>
class BoundedQueue {
public:
    explicit BoundedQueue(std::size_t capacity) : capacity_(capacity)
    {
        if (capacity_ == 0) throw std::invalid_argument("BoundedQueue capacity must be greater than zero");
    }

    BoundedQueue(const BoundedQueue&)            = delete;
    BoundedQueue& operator=(const BoundedQueue&) = delete;
    BoundedQueue(BoundedQueue&&)                 = delete;
    BoundedQueue& operator=(BoundedQueue&&)      = delete;

    [[nodiscard]] std::size_t capacity() const noexcept
    {
        return capacity_;
    }

    [[nodiscard]] std::size_t size() const
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        return items_.size();
    }

    [[nodiscard]] bool empty() const
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        return items_.empty();
    }

    [[nodiscard]] bool closed() const
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        return closed_;
    }

    bool try_push(const T& value)
    {
        return try_push_impl(value);
    }

    bool try_push(T&& value)
    {
        return try_push_impl(std::move(value));
    }

    bool push(const T& value)
    {
        return push_impl(value);
    }

    bool push(T&& value)
    {
        return push_impl(std::move(value));
    }

    bool try_pop(T& value)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (items_.empty()) return false;
        value = std::move(items_.front());
        items_.pop_front();
        not_full_.notify_one();
        return true;
    }

    // Waits until an item is available or the closed queue has been drained.
    bool pop(T& value)
    {
        std::unique_lock<std::mutex> lock(mutex_);
        not_empty_.wait(lock, [this] { return closed_ || !items_.empty(); });
        if (items_.empty()) return false;
        value = std::move(items_.front());
        items_.pop_front();
        lock.unlock();
        not_full_.notify_one();
        return true;
    }

    void close()
    {
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            closed_ = true;
        }
        not_empty_.notify_all();
        not_full_.notify_all();
    }

    std::size_t clear()
    {
        std::size_t removed = 0;
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            removed = items_.size();
            items_.clear();
        }
        not_full_.notify_all();
        return removed;
    }

private:
    template <typename U>
    bool try_push_impl(U&& value)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (closed_ || items_.size() >= capacity_) return false;
        items_.emplace_back(std::forward<U>(value));
        not_empty_.notify_one();
        return true;
    }

    template <typename U>
    bool push_impl(U&& value)
    {
        std::unique_lock<std::mutex> lock(mutex_);
        not_full_.wait(lock, [this] { return closed_ || items_.size() < capacity_; });
        if (closed_) return false;
        items_.emplace_back(std::forward<U>(value));
        lock.unlock();
        not_empty_.notify_one();
        return true;
    }

    const std::size_t capacity_;
    mutable std::mutex mutex_;
    std::condition_variable not_empty_;
    std::condition_variable not_full_;
    std::deque<T> items_;
    bool closed_ = false;
};

}  // namespace telegram

#endif
