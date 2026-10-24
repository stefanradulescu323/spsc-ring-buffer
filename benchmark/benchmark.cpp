#include "mutex_queue.hpp"
#include "spsc_ring_buffer.hpp"
#include "spsc_naive.hpp"
#include "spsc_padded.hpp"


#include <pthread.h>
#include <sched.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <thread>
#include <vector>

void pin_to_cpu(int cpu) {
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    if (pthread_setaffinity_np(pthread_self(), sizeof(set), &set) != 0) {
        std::fprintf(stderr, "failed to pin to cpu %d\n", cpu);
        std::exit(1);
    }
}

template <typename Q>
double run_once(std::uint64_t n, int producer_cpu, int consumer_cpu) {
    auto q = std::make_unique<Q>();
    std::atomic<bool> ready{false}, go{false};

    std::thread producer([&] {
        pin_to_cpu(producer_cpu);
        ready.store(true, std::memory_order_release);
        while (!go.load(std::memory_order_acquire)) {}
        for (std::uint64_t i = 0; i < n; ++i)
            while (!q->try_push(i)) {}
    });

    pin_to_cpu(consumer_cpu);
    std::uint64_t sum = 0, x;
    while (!ready.load(std::memory_order_acquire)) {} // producer is pinned and waiting

    const auto start = std::chrono::steady_clock::now();
    go.store(true, std::memory_order_release);
    for (std::uint64_t i = 0; i < n; ++i) {
        while (!q->try_pop(x)) {}
        sum += x;
    }
    const auto end = std::chrono::steady_clock::now();
    producer.join();

    if (sum != n * (n - 1) / 2) {
        std::fprintf(stderr, "wrong sum: lost or duplicated items\n");
        std::exit(1);
    }
    const double secs = std::chrono::duration<double>(end - start).count();
    return n / secs;
}

template <typename Q>
void bench(const char* name, std::uint64_t n, int runs, int pcpu, int ccpu) {
    run_once<Q>(n, pcpu, ccpu); // warmup: page faults, CPU frequency ramp-up
    std::vector<double> results;
    for (int r = 0; r < runs; ++r) results.push_back(run_once<Q>(n, pcpu, ccpu));
    std::sort(results.begin(), results.end());
    std::printf("%-12s median %8.1f M ops/s   (min %.1f, max %.1f)\n", name,
                results[runs / 2] / 1e6, results.front() / 1e6, results.back() / 1e6);
}

template <typename Q>
double rtt_once(std::uint64_t n, int main_cpu, int echo_cpu) {
    auto ping = std::make_unique<Q>(); // main -> echo
    auto pong = std::make_unique<Q>(); // echo -> main
    std::atomic<bool> ready{false};

    std::thread echo([&] {
        pin_to_cpu(echo_cpu);
        ready.store(true, std::memory_order_release);
        std::uint64_t x;
        for (std::uint64_t i = 0; i < n; ++i) {
            while (!ping->try_pop(x)) {}
            while (!pong->try_push(x)) {}
        }
    });

    pin_to_cpu(main_cpu);
    while (!ready.load(std::memory_order_acquire)) {}

    std::uint64_t x;
    const auto start = std::chrono::steady_clock::now();
    for (std::uint64_t i = 0; i < n; ++i) {
        while (!ping->try_push(i)) {}
        while (!pong->try_pop(x)) {}
        if (x != i) {
            std::fprintf(stderr, "ping-pong got %llu, expected %llu\n",
                         (unsigned long long)x, (unsigned long long)i);
            std::exit(1);
        }
    }
    const auto end = std::chrono::steady_clock::now();
    echo.join();

    return std::chrono::duration<double, std::nano>(end - start).count() / n;
}

template <typename Q>
void bench_rtt(const char* name, std::uint64_t n, int runs, int cpu_a, int cpu_b) {
    rtt_once<Q>(n, cpu_a, cpu_b); // warmup
    std::vector<double> results;
    for (int r = 0; r < runs; ++r) 
        results.push_back(rtt_once<Q>(n, cpu_a, cpu_b));
    std::sort(results.begin(), results.end());
    std::printf("%-12s median %8.1f ns/RTT   (min %.1f, max %.1f)\n", name,
                results[runs / 2], results.front(), results.back());
}

int main(int argc, char** argv) {
    const int pcpu = argc > 1 ? std::atoi(argv[1]) : 0;
    const int ccpu = argc > 2 ? std::atoi(argv[2]) : 1;
    constexpr std::uint64_t N = 10'000'000;
    constexpr int RUNS = 21;

    std::printf("producer on cpu %d, consumer on cpu %d, %llu items, %d runs\n",
                pcpu, ccpu, (unsigned long long)N, RUNS);
    bench<MutexQueue<std::uint64_t, 1024>>("mutex", N, RUNS, pcpu, ccpu);
    bench<SpscNaive<std::uint64_t, 1024>>("spsc naive", N, RUNS, pcpu, ccpu);
    bench<SpscPadded<std::uint64_t, 1024>>("spsc padded", N, RUNS, pcpu, ccpu);
    bench<SpscRingBuffer<std::uint64_t, 1024>>("spsc cached", N, RUNS, pcpu, ccpu);

    constexpr std::uint64_t RTT_N = 1'000'000;
    std::printf("\nround-trip latency, %llu round trips, %d runs\n",
                (unsigned long long)RTT_N, RUNS);
    bench_rtt<MutexQueue<std::uint64_t, 1024>>("mutex", RTT_N, RUNS, pcpu, ccpu);
    bench_rtt<SpscNaive<std::uint64_t, 1024>>("spsc naive", RTT_N, RUNS, pcpu, ccpu);
    bench_rtt<SpscPadded<std::uint64_t, 1024>>("spsc padded", RTT_N, RUNS, pcpu, ccpu);
    bench_rtt<SpscRingBuffer<std::uint64_t, 1024>>("spsc cached", RTT_N, RUNS, pcpu, ccpu);
}