#pragma once

// division = divisibility: the search is linear, numbers 2, 3, ..., limit are
// tested one at a time. For each number n, the candidate divisors
// 2 .. floor(sqrt(n)) are cut into one contiguous block per thread and all
// threads test their block at once. A std::barrier keeps the threads in
// lock-step: nobody starts n + 1 until every thread is done with n. The
// barrier's completion step, which runs on the last thread to arrive, decides
// whether n was prime, reports it under that thread's id, and moves the search
// on to n + 1.

#include <atomic>
#include <barrier>
#include <cstddef>
#include <cstdint>

#include "prime_output.h"
#include "primes.h"
#include "run_threads.h"

namespace divisibility_detail {

// Lets the barrier's completion step know which worker it is running on.
inline thread_local unsigned currentThreadId = 0;

// State shared by every worker. `current` is only changed in the barrier's
// completion step, while all workers are blocked at the barrier, so workers can
// read it without a lock.
struct SharedSearch {
    std::uint64_t limit = 0;
    unsigned threads = 0;
    std::uint64_t current = 2;   // 1 is not prime and has no divisors to test
    std::atomic<bool> divisorFound{false};
    PrimeOutput* output = nullptr;
};

// Barrier completion step: runs exactly once per number, after every thread has
// finished its share of the divisors for `current`.
struct FinishNumber {
    SharedSearch* search;

    void operator()() noexcept {
        if (!search->divisorFound.load(std::memory_order_relaxed)) {
            search->output->report(currentThreadId, search->current);
        }
        search->divisorFound.store(false, std::memory_order_relaxed);
        ++search->current;
    }
};

using NumberBarrier = std::barrier<FinishNumber>;

inline void testDivisors(unsigned threadId, SharedSearch& search, NumberBarrier& barrier) {
    currentThreadId = threadId;
    while (search.current <= search.limit) {
        const std::uint64_t n = search.current;
        const Range divisors = splitRange(2, isqrt(n), search.threads, threadId - 1);
        for (std::uint64_t d = divisors.first; d <= divisors.last; ++d) {
            // Stop early once any thread has proven n composite.
            if (search.divisorFound.load(std::memory_order_relaxed)) break;
            if (n % d == 0) {
                search.divisorFound.store(true, std::memory_order_relaxed);
                break;
            }
        }
        barrier.arrive_and_wait();
    }
}

}  // namespace divisibility_detail

inline void runDivisibilityTesting(std::uint64_t limit, unsigned threads, PrimeOutput& output) {
    using namespace divisibility_detail;

    SharedSearch search;
    search.limit = limit;
    search.threads = threads;
    search.output = &output;
    NumberBarrier barrier(static_cast<std::ptrdiff_t>(threads), FinishNumber{&search});
    runThreads(threads, [&search, &barrier](unsigned threadId) { testDivisors(threadId, search, barrier); });
}
