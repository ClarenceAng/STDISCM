#pragma once

// division = straight: [1, limit] is cut into one contiguous block per thread
// (for 1 - 1000 and 4 threads: 1-250, 251-500, 501-750, 751-1000). Each thread
// tests every number in its block on its own. The threads share nothing but the
// PrimeOutput they report to, and once started never wait for each other until
// the join.

#include <cstdint>
#include <vector>

#include "prime_output.h"
#include "primes.h"
#include "run_threads.h"
#include "timing.h"

inline std::vector<Range> straightDivisionBlocks(std::uint64_t limit, unsigned threads) {
    std::vector<Range> blocks(threads);
    for (unsigned t = 0; t < threads; ++t) {
        blocks[t] = splitRange(1, limit, threads, t);
    }
    return blocks;
}

namespace straight_detail {

inline void searchBlock(unsigned threadId, Range block, PrimeOutput& output, SteadyClock::time_point runStart,
                        SteadyClock::duration& finishedAfter) {
    for (std::uint64_t n = block.first; n <= block.last; ++n) {
        if (isPrime(n)) output.report(threadId, n);
    }
    finishedAfter = SteadyClock::now() - runStart;
}

}  // namespace straight_detail

// Runs one thread per block and returns, for each thread, how long after
// runStart it finished.
inline std::vector<SteadyClock::duration> runStraightDivision(const std::vector<Range>& blocks, PrimeOutput& output,
                                                              SteadyClock::time_point runStart) {
    std::vector<SteadyClock::duration> finishedAfter(blocks.size());
    runThreads(static_cast<unsigned>(blocks.size()), [&](unsigned threadId) {
        straight_detail::searchBlock(threadId, blocks[threadId - 1], output, runStart, finishedAfter[threadId - 1]);
    });
    return finishedAfter;
}
