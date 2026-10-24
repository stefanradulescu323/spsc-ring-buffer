# Lock-free SPSC Ring Buffer

[![CI](https://github.com/stefanradulescu323/spsc-ring-buffer/actions/workflows/ci.yml/badge.svg)](https://github.com/stefanradulescu323/spsc-ring-buffer/actions/workflows/ci.yml)

A bounded, lock-free single-producer / single-consumer queue in C++20, plus a benchmark that measures each optimization step against a mutex-protected `std::queue`.

On an Intel Core i7-8750H (WSL2), the final version reaches **5–11× the throughput** and **12× lower round-trip latency** than the mutex baseline.

- Header-only, no dependencies: [`include/spsc_ring_buffer.hpp`](include/spsc_ring_buffer.hpp)
- `try_push` / `try_pop` are wait-free: no locks, only plain atomic loads and stores
- Tested single-threaded, with a two-thread stress test, and under ThreadSanitizer
- Benchmarks four variants: mutex baseline, naive SPSC, SPSC + cache-line padding, SPSC + padding + cached indices

## Usage

```cpp
#include "spsc_ring_buffer.hpp"

SpscRingBuffer<int, 1024> q;   // capacity must be a power of two; holds up to 1023 items

// producer thread
while (!q.try_push(42)) { /* full: spin, back off, or drop */ }

// consumer thread
int x;
if (q.try_pop(x)) { /* use x */ }
```

## Results

### Environment

| | |
|---|---|
| CPU | Intel Core i7-8750H: 6 cores / 12 threads, 2.2 GHz base, laptop |
| OS | Windows, WSL2 (Ubuntu) |
| Compiler | GCC 11.4, `-O3 -march=native` |
| Queue | `uint64_t` items, capacity 1024 (8 KB, fits in L1) |
| Pinning | producer/ping thread on CPU 0, consumer/echo thread on CPU 2 (different physical cores) |

Every number is the **median of 21 runs** after one discarded warmup run. The min–max range is shown because the spread is wide.

### Throughput

10 million items per run. Higher is better.

| Queue | Median (M ops/s) | Min – max | vs mutex |
|---|---:|---:|---:|
| `std::queue` + `std::mutex` | 8.5 | 7.6 – 9.0 | 1.0× |
| SPSC naive | 48.1 | 33.1 – 85.1 | 5.7× |
| SPSC + padding | 44.0 | 40.9 – 144.5 | 5.2× |
| SPSC + padding + cached indices | **90.9** | 57.4 – 417.0 | **10.7×** |

### Round-trip latency

1 million ping-pong round trips per run. Lower is better.

| Queue | Median (ns / round trip) | Min – max | vs mutex |
|---|---:|---:|---:|
| `std::queue` + `std::mutex` | 2136 | 2096 – 2177 | — |
| SPSC naive | 188 | 46 – 201 | 11.3× lower |
| SPSC + padding | 174 | 54 – 189 | 12.3× lower |
| SPSC + padding + cached indices | **169** | 54 – 188 | **12.7× lower** |

### Same physical core vs different cores

Medians with the threads on CPUs 0 + 1 (hyperthread siblings, according to `lscpu -e`) compared with CPUs 0 + 2:

| Queue | Throughput 0+2 | Throughput 0+1 | RTT 0+2 | RTT 0+1 |
|---|---:|---:|---:|---:|
| mutex | 8.5 | 9.1 | 2136 | 2186 |
| naive | 48.1 | 54.2 | 188 | 173 |
| padded | 44.0 | 45.2 | 174 | 182 |
| cached | 90.9 | 62.6 | 169 | 174 |

### What the numbers show

1. **Every SPSC variant beats the mutex queue**, by 5× or more in throughput and more than 10× in latency.
2. **Padding alone does not help.** It was slightly slower than the naive version in both CPU configurations.
3. **Padding + cached indices is the fastest for throughput.** It had both the best median and the best minimum in both configurations.
4. **For latency, the three SPSC variants are within about 10% of each other**, and their order changes between configurations. The differences are within the noise.

## How it works

### Ring buffer basics

The queue is an array of `Capacity` slots with two indices:

- `head_`: the next slot the producer will write
- `tail_`: the next slot the consumer will read

The queue is **empty** when `head == tail` and **full** when `head + 1 == tail`. One slot always stays unused: if all `Capacity` slots could fill, a full queue would also have `head == tail`, and it would look the same as an empty one. So the queue holds `Capacity - 1` items.

Indices wrap with `& MASK`, where `MASK = Capacity - 1`. That's why the capacity must be a power of two: for a power of two, `x & (Capacity - 1)` equals `x % Capacity`, and it compiles to a single AND instruction. A general modulo needs a division, which costs tens of cycles.

### Ownership: why no locks are needed

| | `head_` | `tail_` |
|---|---|---|
| producer | writes | reads |
| consumer | reads | writes |

Each index has exactly one writer. Neither thread ever needs an atomic read-modify-write.

### Memory ordering

```cpp
bool try_push(const T& item) {
    const std::size_t head = head_.load(std::memory_order_relaxed);  // (1)
    const std::size_t new_head = (head + 1) & MASK;
    if (new_head == tail_.load(std::memory_order_acquire))           // (2)
        return false;
    buffer_[head] = item;                                            // (3)
    head_.store(new_head, std::memory_order_release);                // (4)
    return true;
}
```

(This is the naive version; the cached-index version below keeps the same orderings.)

1. **Its own index is loaded `relaxed`.** Only the producer writes `head_`, and a thread always sees its own writes, so no ordering is needed.
2. **The other side's index is loaded `acquire`.** This pairs with the consumer's `release` store to `tail_`. It guarantees the consumer has finished *reading* a slot before the producer overwrites it in step 3.
3. **The producer writes the item** into the slot.
4. **The new index is published with `release`.** This guarantees the item written in step 3 becomes visible to the consumer before the new `head_` does.

`try_pop` is the mirror image.

### False sharing and cache-line padding

CPU caches move data between cores in 64-byte **cache lines**, not as individual variables. To write to a line, a core needs exclusive ownership of it, which invalidates every other core's copy. The next time another core reads that line, it misses its cache and the line has to be transferred, which costs tens of nanoseconds.

In the naive layout, `head_` and `tail_` sit in the same line. So every push by the producer invalidates the consumer's copy of `tail_`, even though the producer never writes `tail_`. Two independent variables sharing one line like this is called **false sharing**.

The padded version gives each index its own line:

```cpp
alignas(64) std::atomic<std::size_t> head_{0};
alignas(64) std::atomic<std::size_t> tail_{0};
alignas(64) std::array<T, Capacity> buffer_;
```

64 bytes is the cache-line size on x86 and most ARM chips.

### Cached indices

The producer only needs the real `tail_` when the queue *might* be full. So each side keeps a private copy of the other side's index, and only reloads the real atomic when the copy says the queue is full (producer) or empty (consumer):

```cpp
alignas(64) std::atomic<std::size_t> head_{0};
std::size_t tail_cache_ = 0;     // producer's private copy of tail_

alignas(64) std::atomic<std::size_t> tail_{0};
std::size_t head_cache_ = 0;     // consumer's private copy of head_

bool try_push(const T& item) {
    const std::size_t head = head_.load(std::memory_order_relaxed);
    const std::size_t new_head = (head + 1) & MASK;
    if (new_head == tail_cache_) {                           // looks full
        tail_cache_ = tail_.load(std::memory_order_acquire);  // refresh
        if (new_head == tail_cache_) return false;            // really full
    }
    buffer_[head] = item;
    head_.store(new_head, std::memory_order_release);
    return true;
}
```

- **The caches are plain `size_t`.** Only one thread ever touches each copy, so they can't race.
- **Each cache sits on its owner's cache line.** All the state a push touches is in one line, and the same for a pop.
- **A stale copy is always safe.** The consumer only ever advances `tail_`, so an old `tail_cache_` can only *underestimate* the free space. The worst case is an unnecessary refresh, never an overwrite of a slot that hasn't been read yet. The same argument applies to `head_cache_` on the consumer side.

Most operations now touch only the thread's own cache line. Cross-core traffic happens only when a cached copy runs out.

## Discussion

### Why padding alone made it slower

Padding only fixes *false* sharing. In the naive queue, both threads really do read the other side's index on every operation, so the sharing is real. Splitting the two indices into separate lines doesn't remove that traffic: each operation now touches two contended lines instead of one.

Padding pays off once cached indices make each line mostly private to one thread. The two optimizations belong together, and measuring them separately is what shows it.

### Latency vs throughput

In ping-pong, each queue holds at most one item, so cached indices can't batch. Their only saving is skipping the read of the other side's index on each push. That's why the throughput optimization gives a large gain in throughput but no clear gain in latency. Which optimization matters depends on which metric you care about.

### Measuring inside WSL2

WSL2 runs Linux in a virtual machine. Pinning a thread to "CPU 2" pins it to a *virtual* CPU, and the Windows hypervisor decides which physical core runs that virtual CPU at any moment. Two observations are consistent with this:

- **Sibling CPUs made no consistent difference.** CPUs 0 + 1, hyperthreads of one core according to `lscpu -e`, gave about the same results as CPUs 0 + 2. On bare metal, hyperthread siblings share their L1 and L2 caches, so you'd expect noticeably lower latency.
- **Occasional very low latency.** Runs around 45–60 ns per round trip appear in both configurations. That's what you'd expect if the hypervisor sometimes placed both virtual CPUs on the same physical core.

These results are therefore measurements of WSL2 on this laptop, not of the bare hardware.

## Benchmark methodology

`benchmark/benchmark.cpp`:

- **Thread pinning:** `pthread_setaffinity_np` fixes each thread to a chosen CPU, given on the command line.
- **Start handshake:** each thread pins itself before the clock starts, so thread creation isn't timed.
- **Throughput:** the producer pushes `0..N-1` and the consumer pops all N. The time is measured on the consumer from start to the last pop. The consumer sums every value, which stops the compiler from removing the loop, and the total is checked against `N(N-1)/2`.
- **Latency:** two queues, `ping` (main → echo) and `pong` (echo → main). The main thread sends a value and waits for it to come back, then repeats. Total time divided by the number of round trips gives the mean round-trip time, and every value is checked on return. A single clock on one thread is used, so no cross-core clock comparison is needed.
- **Statistics:** one warmup run is discarded , then the median, min and max of 21 runs are reported. The median ignores outlier runs.
- **Variants:** the naive and padded versions are frozen copies in `benchmark/` (`SpscNaive`, `SpscPadded`), so all four variants are measured by the same binary in the same run.

## Testing

`tests/test_basic.cpp`:

| Test | Checks |
|---|---|
| empty | popping an empty queue fails and leaves the output unchanged |
| fill until full | exactly `Capacity - 1` pushes succeed, then the items come out in FIFO order |
| wraparound | 100 rounds on a 4-slot queue: the indices lap the ring repeatedly |
| capacity 2 | the smallest legal queue, which flips between full and empty on every operation |
| concurrent | a producer thread pushes `0..999999` and the consumer checks it receives exactly that sequence; run with capacities 2, 4 and 1024, and on the mutex queue |

**ThreadSanitizer:** all tests pass under `-fsanitize=thread` with no reports. As a check that the test can catch real bugs, weakening the `release` store of `head_` to `relaxed` makes TSan report a data race between the slot write in `try_push` and the slot read in `try_pop`.

## Building

Requirements: Linux (or WSL2), GCC or Clang with C++20, CMake ≥ 3.20. The queue header itself is portable standard C++; only the benchmark uses Linux-specific APIs.

```bash
cmake -B build -S .
cmake --build build
./build/test_basic
./build/benchmark 0 2     # producer CPU, consumer CPU
```

ThreadSanitizer build:

```bash
cmake -B build-tsan -S . -DSPSC_TSAN=ON
cmake --build build-tsan
./build-tsan/test_basic
```

## Limitations

- **Usable capacity is `Capacity - 1`**, and the capacity is fixed at compile time.
- **`T` must be default-constructible and copy-assignable.** `std::array` constructs every slot up front, and `try_push` copies the item.
- **Popped items aren't destroyed.** A slot keeps its old value until it's overwritten, which matters for types that hold resources, such as `std::string` or `std::shared_ptr`.
- **No blocking API.** Callers spin, back off, or drop when `try_push` / `try_pop` fails.
- **All numbers come from WSL2 on one laptop.**
