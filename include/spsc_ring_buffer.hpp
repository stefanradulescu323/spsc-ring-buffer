#pragma once

#include <cstddef>
#include <array>

template<typename T, std::size_t Capacity>
class SpscRingBuffer {
	static_assert(Capacity >= 2, "Capacity must be at least 2");
	static_assert((Capacity & (Capacity - 1)) == 0, "Capacity must be a power of 2");

	static constexpr std::size_t MASK = Capacity - 1;

	std::atomic<std::size_t> head_{0};
	std::atomic<std::size_t> tail_{0};
	std::array<T, Capacity> buffer_;

public:
	bool try_push(const T& item) {
		const std::size_t head = head_.load(std::memory_order_relaxed); // relaxed because only producer writes to head
		const std::size_t new_head = (head + 1) % MASK;
		if (new_head == tail_.load(std::memory_order_acquire)) {
			return false; // full
		}
		buffer_[head] = item;
		head_.store(new_head, std::memory_order_release);
		return true;
	}

	bool try_pop(T& item) {
		const std::size_t tail = tail_.load(std::memory_order_relaxed);
		if (tail == head_.load(std::memory_order_acquire))
			return false; // empty
		item = buffer_[tail];
		tail.store((tail + 1) % MASK, std::memory_order_release);
		return true;
	}
};