#include "mutex_queue.hpp"
#include "spsc_ring_buffer.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <thread>

// assert() is compiled out with -DNDEBUG (Release builds), so use our own.
#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__,      \
                         __LINE__, #cond);                                   \
            std::abort();                                                    \
        }                                                                    \
    } while (0)

// ---------- single-threaded: SPSC semantics ----------

void test_empty() {
    SpscRingBuffer<int, 8> q;
    int x = -1;
    CHECK(!q.try_pop(x));
    CHECK(x == -1); // output untouched on failure
}

void test_fill_until_full() {
    constexpr std::size_t N = 8;
    SpscRingBuffer<int, N> q;
    // One slot always stays empty to tell "full" from "empty": usable = N - 1.
    for (int i = 0; i < int(N - 1); ++i) CHECK(q.try_push(i));
    CHECK(!q.try_push(99));

    int x;
    for (int i = 0; i < int(N - 1); ++i) {
        CHECK(q.try_pop(x));
        CHECK(x == i);
    }
    CHECK(!q.try_pop(x));
}

void test_wraparound() {
    SpscRingBuffer<int, 4> q;
    int x, next_in = 0, next_out = 0;
    // Push 3, pop 3, repeatedly: head/tail lap the ring many times.
    for (int round = 0; round < 100; ++round) {
        for (int k = 0; k < 3; ++k) CHECK(q.try_push(next_in++));
        CHECK(!q.try_push(-1));
        for (int k = 0; k < 3; ++k) {
            CHECK(q.try_pop(x));
            CHECK(x == next_out++);
        }
        CHECK(!q.try_pop(x));
    }
}

void test_capacity_two() {
    SpscRingBuffer<int, 2> q; // smallest legal size: holds exactly 1 item
    int x;
    for (int i = 0; i < 10; ++i) {
        CHECK(q.try_push(i));
        CHECK(!q.try_push(i));
        CHECK(q.try_pop(x));
        CHECK(x == i);
        CHECK(!q.try_pop(x));
    }
}

// ---------- two threads: works for any queue with try_push/try_pop ----------

template <typename Q>
void test_concurrent(const char* name, std::uint64_t n) {
    auto q = std::make_unique<Q>(); // heap: a large std::array would blow the stack

    std::thread producer([&] {
        for (std::uint64_t i = 0; i < n; ++i)
            while (!q->try_push(i)) {} // spin while full
    });

    // Consumer runs on this thread and checks strict FIFO order.
    std::uint64_t expected = 0, x;
    while (expected < n) {
        if (q->try_pop(x)) {
            CHECK(x == expected);
            ++expected;
        }
    }
    producer.join();
    CHECK(!q->try_pop(x));

    std::cout << "  " << name << ": " << n << " items, in order\n";
}

int main() {
    test_empty();
    test_fill_until_full();
    test_wraparound();
    test_capacity_two();
    std::cout << "single-threaded tests passed\n";

    constexpr std::uint64_t N = 1'000'000;
    // Tiny capacity = constant full/empty transitions, the racy edge cases.
    test_concurrent<SpscRingBuffer<std::uint64_t, 2>>("spsc cap 2", N);
    test_concurrent<SpscRingBuffer<std::uint64_t, 4>>("spsc cap 4", N);
    test_concurrent<SpscRingBuffer<std::uint64_t, 1024>>("spsc cap 1024", N);
    test_concurrent<MutexQueue<std::uint64_t, N>>("mutex queue", N);

    std::cout << "all tests passed\n";
    return 0;
}