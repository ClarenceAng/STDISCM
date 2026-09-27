// Variant 4: wait until all threads are done, then print + threads split the
// divisibility test.
//
// The search is linear: numbers 2, 3, ..., limit are tested one at a time.
// For each number n, the candidate divisors 2 .. floor(sqrt(n)) are cut into
// one contiguous block per thread and all threads test their block at once.
// A std::barrier keeps the threads in lock-step: nobody starts n + 1 until
// every thread is done with n. The barrier's completion step, which runs on the
// last thread to arrive, decides whether n was prime, records it with that
// thread's id and the time, and moves the search on to n + 1. Nothing is
// printed until every thread has been joined.

#include <atomic>
#include <barrier>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "config.h"
#include "primes.h"
#include "timing.h"

namespace {

// Lets the barrier's completion step know which worker it is running on.
thread_local unsigned currentThreadId = 0;

struct PrimeRecord {
    std::uint64_t value;
    unsigned threadId;
    SystemClock::time_point foundAt;
};

// State shared by every worker. `current` and `primes` are only changed in the
// barrier's completion step, while all workers are blocked at the barrier, so
// workers can read `current` without a lock.
struct SharedSearch {
    std::uint64_t limit = 0;
    unsigned threads = 0;
    std::uint64_t current = 2;   // 1 is not prime and has no divisors to test
    std::vector<PrimeRecord> primes;
    std::atomic<bool> divisorFound{false};
};

// Barrier completion step: runs exactly once per number, after every thread has
// finished its share of the divisors for `current`.
struct FinishNumber {
    SharedSearch* search;

    void operator()() noexcept {
        if (!search->divisorFound.load(std::memory_order_relaxed)) {
            search->primes.push_back({search->current, currentThreadId, SystemClock::now()});
        }
        search->divisorFound.store(false, std::memory_order_relaxed);
        ++search->current;
    }
};

using NumberBarrier = std::barrier<FinishNumber>;

void testDivisors(unsigned threadId, SharedSearch& search, NumberBarrier& barrier) {
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

std::string describe(const Range& range) {
    if (range.empty()) return "(no divisors)";
    return std::to_string(range.first) + " - " + std::to_string(range.last);
}

}  // namespace

int main(int argc, char* argv[]) {
    std::ios::sync_with_stdio(false);   // only the main thread ever prints

    const std::string configPath = argc > 1 ? argv[1] : "config.txt";
    Config config;
    try {
        config = loadConfig(configPath);
    } catch (const std::exception& error) {
        std::cerr << "Config error: " << error.what() << '\n';
        return 1;
    }

    const std::uint64_t exampleRoot = isqrt(config.limit);
    std::cout << "Variant 4: print after all threads finish | threads split the divisibility test of each number\n"
              << "Threads: " << config.threads << " | Search range: 1 - " << config.limit << "\n\n"
              << "Numbers are tested one after another. Example split for n = " << config.limit
              << " (divisors " << describe({2, exampleRoot}) << "):\n";
    for (unsigned t = 0; t < config.threads; ++t) {
        std::cout << "  Thread " << t + 1 << " tests "
                  << describe(splitRange(2, exampleRoot, config.threads, t)) << '\n';
    }

    SharedSearch search;
    search.limit = config.limit;
    search.threads = config.threads;
    NumberBarrier barrier(static_cast<std::ptrdiff_t>(config.threads), FinishNumber{&search});

    const auto runStart = SteadyClock::now();
    std::cout << "\nStart time: " << formatTimestamp(SystemClock::now()) << "\n\n" << std::flush;

    std::vector<std::thread> workers;
    workers.reserve(config.threads);
    for (unsigned t = 0; t < config.threads; ++t) {
        workers.emplace_back(testDivisors, t + 1, std::ref(search), std::ref(barrier));
    }
    for (auto& worker : workers) {
        worker.join();
    }

    const auto searchTime = SteadyClock::now() - runStart;
    std::cout << "All threads joined after " << formatMilliseconds(searchTime)
              << ". Primes (timestamps show when each was found):\n\n";

    for (const auto& prime : search.primes) {
        std::cout << '[' << formatTimestamp(prime.foundAt, false) << "] [Thread " << prime.threadId << "] "
                  << prime.value << '\n';
    }

    const auto elapsed = SteadyClock::now() - runStart;
    const std::uint64_t numbersTested = config.limit >= 2 ? config.limit - 1 : 0;
    std::cout << "\nEnd time:   " << formatTimestamp(SystemClock::now()) << '\n'
              << "Elapsed:    " << formatMilliseconds(elapsed) << "  (search " << formatMilliseconds(searchTime)
              << " + printing " << formatMilliseconds(elapsed - searchTime) << ")\n"
              << "Primes found: " << search.primes.size() << '\n'
              << "Numbers tested: " << numbersTested << " (each one is a barrier round every thread waits on)\n";
    return 0;
}
