// Threaded prime number search: finds every prime from 1 to y using x threads.
//
// config.txt chooses when primes are printed and how the work is split:
//   print    = immediate      each thread prints a prime, with its id and a timestamp, the moment it finds it
//            = end            primes are recorded and printed by the main thread after every thread is joined
//   division = straight       [1, y] is cut into x contiguous blocks, one per thread (straight_division.h)
//            = divisibility   numbers are tested one at a time; each number's divisors are split across
//                             the x threads (divisibility_testing.h)

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <new>
#include <string>
#include <vector>

#include "config.h"
#include "divisibility_testing.h"
#include "prime_output.h"
#include "primes.h"
#include "straight_division.h"
#include "timing.h"

namespace {

const char* describe(PrintMode mode) {
    return mode == PrintMode::Immediate ? "immediately, by the thread that finds each prime"
                                        : "after all threads finish, by the main thread";
}

const char* describe(Division division) {
    return division == Division::Straight ? "straight division of the search range"
                                          : "threads split the divisibility test of each number";
}

std::string describe(const Range& range, const char* whenEmpty) {
    if (range.empty()) return whenEmpty;
    return std::to_string(range.first) + " - " + std::to_string(range.last);
}

// Calls printLine(t) for each thread index t. Thousands of lines would only be
// noise, so a long list shows just the first and last few threads.
template <typename PrintLine>
void listThreads(unsigned count, PrintLine printLine) {
    constexpr unsigned kListAllUpTo = 64;
    constexpr unsigned kShownAtEachEnd = 10;
    if (count <= kListAllUpTo) {
        for (unsigned t = 0; t < count; ++t) printLine(t);
        return;
    }
    for (unsigned t = 0; t < kShownAtEachEnd; ++t) printLine(t);
    std::cout << "  ... " << count - 2 * kShownAtEachEnd << " more threads ...\n";
    for (unsigned t = count - kShownAtEachEnd; t < count; ++t) printLine(t);
}

void runSearch(const Config& config) {
    std::cout << "Threaded prime number search\n"
              << "Threads:  " << config.threads << " | Search range: 1 - " << config.limit << '\n'
              << "Print:    " << describe(config.print) << '\n'
              << "Division: " << describe(config.division) << "\n\n";

    std::vector<Range> blocks;
    if (config.division == Division::Straight) {
        blocks = straightDivisionBlocks(config.limit, config.threads);
        listThreads(config.threads, [&](unsigned t) {
            std::cout << "  Thread " << t + 1 << " searches " << describe(blocks[t], "(no numbers)") << '\n';
        });
    } else {
        const std::uint64_t exampleRoot = isqrt(config.limit);
        std::cout << "Numbers are tested one after another. Example split for n = " << config.limit
                  << " (divisors " << describe({2, exampleRoot}, "(no divisors)") << "):\n";
        listThreads(config.threads, [&](unsigned t) {
            std::cout << "  Thread " << t + 1 << " tests "
                      << describe(splitRange(2, exampleRoot, config.threads, t), "(no divisors)") << '\n';
        });
    }

    PrimeOutput output(config.print, config.threads);
    const auto runStart = SteadyClock::now();
    std::cout << "\nStart time: " << formatTimestamp(SystemClock::now()) << "\n\n" << std::flush;

    std::vector<SteadyClock::duration> finishedAfter;
    if (config.division == Division::Straight) {
        finishedAfter = runStraightDivision(blocks, output, runStart);
    } else {
        runDivisibilityTesting(config.limit, config.threads, output);
    }
    const auto searchTime = SteadyClock::now() - runStart;

    if (config.print == PrintMode::AtEnd) {
        std::cout << "All threads joined after " << formatMilliseconds(searchTime)
                  << ". Primes (timestamps show when each was found):\n\n";
        output.printRecorded();
    }

    const auto elapsed = SteadyClock::now() - runStart;
    std::cout << "\nEnd time:   " << formatTimestamp(SystemClock::now()) << '\n'
              << "Elapsed:    " << formatMilliseconds(elapsed);
    if (config.print == PrintMode::AtEnd) {
        std::cout << "  (search " << formatMilliseconds(searchTime) << " + printing "
                  << formatMilliseconds(elapsed - searchTime) << ")";
    }
    std::cout << "\nPrimes found: " << output.total() << '\n';

    if (config.division == Division::Straight) {
        std::cout << "\nPer thread (the join waits for the slowest one):\n";
        std::size_t blockWidth = 0;
        for (const auto& block : blocks) blockWidth = std::max(blockWidth, describe(block, "(no numbers)").size());
        listThreads(config.threads, [&](unsigned t) {
            std::cout << "  Thread " << std::left << std::setw(5) << t + 1
                      << std::setw(static_cast<int>(blockWidth)) << describe(blocks[t], "(no numbers)")
                      << std::right << std::setw(9) << output.count(t + 1) << " primes   finished after "
                      << formatMilliseconds(finishedAfter[t]) << '\n';
        });
        const auto slowest = static_cast<std::size_t>(
            std::max_element(finishedAfter.begin(), finishedAfter.end()) - finishedAfter.begin());
        std::cout << "Slowest:  Thread " << slowest + 1 << ", finished after "
                  << formatMilliseconds(finishedAfter[slowest]) << '\n';
    } else {
        const std::uint64_t numbersTested = config.limit >= 2 ? config.limit - 1 : 0;
        std::cout << "Numbers tested: " << numbersTested << " (each one is a barrier round every thread waits on)\n";
    }
}

}  // namespace

int main(int argc, char* argv[]) {
    std::ios::sync_with_stdio(false);   // cout is only ever used under PrimeOutput's lock or from main alone

    if (argc > 2) {
        std::cerr << "Usage: prime_search [config file]   (the config file defaults to config.txt)\n";
        return 1;
    }
    const std::string configPath = argc > 1 ? argv[1] : "config.txt";
    Config config;
    try {
        config = loadConfig(configPath);
    } catch (const std::exception& error) {
        std::cerr << "Config error: " << error.what() << '\n';
        return 1;
    }

    try {
        runSearch(config);
    } catch (const std::bad_alloc&) {
        std::cout << std::flush;
        std::cerr << "\nError: not enough memory to run " << config.threads << " threads up to y = " << config.limit
                  << '\n';
        return 1;
    } catch (const std::exception& error) {
        std::cout << std::flush;
        std::cerr << "\nError: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
