// Variant 3: wait until all threads are done, then print + straight division
// of the search range.
//
// [1, limit] is cut into one contiguous block per thread (for 1 - 1000 and 4
// threads: 1-250, 251-500, 501-750, 751-1000). Each thread tests every number
// in its block and records each prime it finds, with its thread id and the
// time it was found, in a list of its own. Nothing is printed until every
// thread has been joined; then the main thread prints all the lists.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "config.h"
#include "primes.h"
#include "timing.h"

namespace {

struct PrimeRecord {
    std::uint64_t value;
    unsigned threadId;
    SystemClock::time_point foundAt;
};

struct ThreadResult {
    std::vector<PrimeRecord> primes;
    SteadyClock::duration finishedAfter{};   // measured from the start of the run
};

void searchRange(unsigned threadId, Range range, SteadyClock::time_point runStart, ThreadResult& result) {
    // Collect into a local list so threads share nothing while they search.
    std::vector<PrimeRecord> found;
    for (std::uint64_t n = range.first; n <= range.last; ++n) {
        if (isPrime(n)) {
            found.push_back({n, threadId, SystemClock::now()});
        }
    }
    result.primes = std::move(found);
    result.finishedAfter = SteadyClock::now() - runStart;
}

std::string describe(const Range& range) {
    if (range.empty()) return "(no numbers)";
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

    std::vector<Range> ranges(config.threads);
    for (unsigned t = 0; t < config.threads; ++t) {
        ranges[t] = splitRange(1, config.limit, config.threads, t);
    }

    std::cout << "Variant 3: print after all threads finish | straight division of the search range\n"
              << "Threads: " << config.threads << " | Search range: 1 - " << config.limit << "\n\n";
    for (unsigned t = 0; t < config.threads; ++t) {
        std::cout << "  Thread " << t + 1 << " searches " << describe(ranges[t]) << '\n';
    }

    const auto runStart = SteadyClock::now();
    std::cout << "\nStart time: " << formatTimestamp(SystemClock::now()) << "\n\n" << std::flush;

    std::vector<ThreadResult> results(config.threads);
    std::vector<std::thread> workers;
    workers.reserve(config.threads);
    for (unsigned t = 0; t < config.threads; ++t) {
        workers.emplace_back(searchRange, t + 1, ranges[t], runStart, std::ref(results[t]));
    }
    for (auto& worker : workers) {
        worker.join();
    }

    const auto searchTime = SteadyClock::now() - runStart;
    std::cout << "All threads joined after " << formatMilliseconds(searchTime)
              << ". Primes (timestamps show when each was found):\n\n";

    // Blocks are in ascending order, so printing thread by thread lists the primes in order.
    std::uint64_t totalPrimes = 0;
    for (const auto& result : results) {
        for (const auto& prime : result.primes) {
            std::cout << '[' << formatTimestamp(prime.foundAt, false) << "] [Thread " << prime.threadId << "] "
                      << prime.value << '\n';
        }
        totalPrimes += result.primes.size();
    }

    const auto elapsed = SteadyClock::now() - runStart;
    std::cout << "\nEnd time:   " << formatTimestamp(SystemClock::now()) << '\n'
              << "Elapsed:    " << formatMilliseconds(elapsed) << "  (search " << formatMilliseconds(searchTime)
              << " + printing " << formatMilliseconds(elapsed - searchTime) << ")\n"
              << "Primes found: " << totalPrimes << "\n\n"
              << "Per thread (the join waits for the slowest one):\n";
    std::size_t rangeWidth = 0;
    for (const auto& range : ranges) rangeWidth = std::max(rangeWidth, describe(range).size());
    for (unsigned t = 0; t < config.threads; ++t) {
        std::cout << "  Thread " << std::left << std::setw(5) << t + 1
                  << std::setw(static_cast<int>(rangeWidth)) << describe(ranges[t])
                  << std::right << std::setw(9) << results[t].primes.size() << " primes   finished after "
                  << formatMilliseconds(results[t].finishedAfter) << '\n';
    }
    return 0;
}
