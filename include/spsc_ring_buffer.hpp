#pragma once

#include <cstddef>
#include <array>
#include <atomic>

template<typename T, std::size_t Capacity>
class SpscRingBuffer {
	static_assert(Capacity >= 2, "Capacity must be at least 2");
	static_assert((Capacity & (Capacity - 1)) == 0, "Capacity must be a power of 2");

	static constexpr std::size_t MASK = Capacity - 1;
	static constexpr std::size_t CACHE_LINE = 64;

	// Producer's line: head_ plus its private copy of tail_.
	alignas(CACHE_LINE) std::atomic<std::size_t> head_{0};
	std::size_t tail_cache_ = 0;

	// Consumer's line: tail_ plus its private copy of head_.
	alignas(CACHE_LINE) std::atomic<std::size_t> tail_{0};
	std::size_t head_cache_ = 0;

	alignas(CACHE_LINE) std::array<T, Capacity> buffer_;

public:
	bool try_push(const T& item) {
		const std::size_t head = head_.load(std::memory_order_relaxed);
		const std::size_t new_head = (head + 1) & MASK;
		if (new_head == tail_cache_) {                         // looks full
			tail_cache_ = tail_.load(std::memory_order_acquire); // refresh
			if (new_head == tail_cache_) return false;           // really full
		}
		buffer_[head] = item;
		head_.store(new_head, std::memory_order_release);
		return true;
	}

	bool try_pop(T& item) {
		const std::size_t tail = tail_.load(std::memory_order_relaxed);
		if (tail == head_cache_) {                             // looks empty
			head_cache_ = head_.load(std::memory_order_acquire); // refresh
			if (tail == head_cache_) return false;               // really empty
		}
		item = buffer_[tail];
		tail_.store((tail + 1) & MASK, std::memory_order_release);
		return true;
	}
};