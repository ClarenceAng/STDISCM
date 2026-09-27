// Variant 1: print immediately + straight division of the search range.
//
// [1, limit] is cut into one contiguous block per thread (for 1 - 1000 and 4
// threads: 1-250, 251-500, 501-750, 751-1000). Each thread tests every number
// in its block on its own and prints a prime, with its thread id and a
// timestamp, the moment it finds one.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "config.h"
#include "primes.h"
#include "timing.h"

namespace {

struct ThreadResult {
    std::uint64_t primesFound = 0;
    SteadyClock::duration finishedAfter{};   // measured from the start of the run
};

std::mutex outputMutex;

// The line is built before taking the lock and written in one piece, so lines
// from different threads interleave with each other but never tear apart.
void printPrime(unsigned threadId, std::uint64_t prime) {
    const std::string line = "[" + formatTimestamp(SystemClock::now(), false) + "] [Thread " +
                             std::to_string(threadId) + "] " + std::to_string(prime) + '\n';
    std::lock_guard<std::mutex> lock(outputMutex);
    std::cout << line << std::flush;
}

void searchRange(unsigned threadId, Range range, SteadyClock::time_point runStart, ThreadResult& result) {
    std::uint64_t found = 0;
    for (std::uint64_t n = range.first; n <= range.last; ++n) {
        if (isPrime(n)) {
            printPrime(threadId, n);
            ++found;
        }
    }
    result.primesFound = found;
    result.finishedAfter = SteadyClock::now() - runStart;
}

std::string describe(const Range& range) {
    if (range.empty()) return "(no numbers)";
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

    std::vector<Range> ranges(config.threads);
    for (unsigned t = 0; t < config.threads; ++t) {
        ranges[t] = splitRange(1, config.limit, config.threads, t);
    }

    std::cout << "Variant 1: print immediately | straight division of the search range\n"
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

    const auto elapsed = SteadyClock::now() - runStart;
    std::cout << "\nEnd time:   " << formatTimestamp(SystemClock::now()) << '\n';

    std::uint64_t totalPrimes = 0;
    for (const auto& result : results) totalPrimes += result.primesFound;

    std::cout << "Elapsed:    " << formatMilliseconds(elapsed) << '\n'
              << "Primes found: " << totalPrimes << "\n\n"
              << "Per thread (the join waits for the slowest one):\n";
    std::size_t rangeWidth = 0;
    for (const auto& range : ranges) rangeWidth = std::max(rangeWidth, describe(range).size());
    for (unsigned t = 0; t < config.threads; ++t) {
        std::cout << "  Thread " << std::left << std::setw(5) << t + 1
                  << std::setw(static_cast<int>(rangeWidth)) << describe(ranges[t])
                  << std::right << std::setw(9) << results[t].primesFound << " primes   finished after "
                  << formatMilliseconds(results[t].finishedAfter) << '\n';
    }
    return 0;
}
