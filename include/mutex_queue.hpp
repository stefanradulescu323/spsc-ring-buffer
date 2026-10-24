#pragma once
#include <cstddef>
#include <mutex>
#include <queue>

template <typename T, std::size_t Capacity>
class MutexQueue {
    std::queue<T> queue_;
    std::mutex mutex_;

public:
    bool try_push(const T& item) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (queue_.size() == Capacity) return false; // full
        queue_.push(item);
        return true;
    }

    bool try_pop(T& item) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (queue_.empty()) return false;
        item = queue_.front();
        queue_.pop();
        return true;
    }
};