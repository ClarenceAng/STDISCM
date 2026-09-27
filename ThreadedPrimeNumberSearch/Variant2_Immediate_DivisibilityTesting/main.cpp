// Variant 2: print immediately + threads split the divisibility test.
//
// The search is linear: numbers 2, 3, ..., limit are tested one at a time.
// For each number n, the candidate divisors 2 .. floor(sqrt(n)) are cut into
// one contiguous block per thread and all threads test their block at once.
// A std::barrier keeps the threads in lock-step: nobody starts n + 1 until
// every thread is done with n. The barrier's completion step, which runs on the
// last thread to arrive, decides whether n was prime, prints it right away with
// that thread's id and a timestamp, and moves the search on to n + 1.

#include <atomic>
#include <barrier>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "config.h"
#include "primes.h"
#include "timing.h"

namespace {

// Lets the barrier's completion step know which worker it is running on.
thread_local unsigned currentThreadId = 0;

std::mutex outputMutex;

// The line is built before taking the lock and written in one piece, so lines
// from different threads interleave with each other but never tear apart.
void printPrime(unsigned threadId, std::uint64_t prime) {
    const std::string line = "[" + formatTimestamp(SystemClock::now(), false) + "] [Thread " +
                             std::to_string(threadId) + "] " + std::to_string(prime) + '\n';
    std::lock_guard<std::mutex> lock(outputMutex);
    std::cout << line << std::flush;
}

// State shared by every worker. `current` and `primesFound` are only changed in
// the barrier's completion step, while all workers are blocked at the barrier,
// so workers can read `current` without a lock.
struct SharedSearch {
    std::uint64_t limit = 0;
    unsigned threads = 0;
    std::uint64_t current = 2;   // 1 is not prime and has no divisors to test
    std::uint64_t primesFound = 0;
    std::atomic<bool> divisorFound{false};
};

// Barrier completion step: runs exactly once per number, after every thread has
// finished its share of the divisors for `current`.
struct FinishNumber {
    SharedSearch* search;

    void operator()() noexcept {
        if (!search->divisorFound.load(std::memory_order_relaxed)) {
            printPrime(currentThreadId, search->current);
            ++search->primesFound;
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
    std::ios::sync_with_stdio(false);   // cout is only ever used under outputMutex or from main alone

    const std::string configPath = argc > 1 ? argv[1] : "config.txt";
    Config config;
    try {
        config = loadConfig(configPath);
    } catch (const std::exception& error) {
        std::cerr << "Config error: " << error.what() << '\n';
        return 1;
    }

    const std::uint64_t exampleRoot = isqrt(config.limit);
    std::cout << "Variant 2: print immediately | threads split the divisibility test of each number\n"
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

    const auto elapsed = SteadyClock::now() - runStart;
    const std::uint64_t numbersTested = config.limit >= 2 ? config.limit - 1 : 0;
    std::cout << "\nEnd time:   " << formatTimestamp(SystemClock::now()) << '\n'
              << "Elapsed:    " << formatMilliseconds(elapsed) << '\n'
              << "Primes found: " << search.primesFound << '\n'
              << "Numbers tested: " << numbersTested << " (each one is a barrier round every thread waits on)\n";
    return 0;
}
