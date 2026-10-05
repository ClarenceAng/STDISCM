#pragma once

// Where the workers send each prime they find. With print = immediate the prime
// is printed on the spot, tagged with the finding thread's id and a timestamp.
// With print = end it is recorded instead, and the main thread prints every
// recorded prime after all workers have been joined.

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <mutex>
#include <string>
#include <vector>

#include "config.h"
#include "timing.h"

struct PrimeRecord {
    std::uint64_t value;
    unsigned threadId;
    SystemClock::time_point foundAt;
};

inline std::string formatPrimeLine(const PrimeRecord& prime) {
    return "[" + formatTimestamp(prime.foundAt, false) + "] [Thread " + std::to_string(prime.threadId) + "] " +
           std::to_string(prime.value) + '\n';
}

class PrimeOutput {
public:
    PrimeOutput(PrintMode mode, unsigned threads) : mode_(mode), slots_(threads) {}

    // Called by worker `threadId` (1-based) for each prime it finds. A worker
    // only ever touches its own slot, so counting and recording need no lock;
    // only writing to the console does.
    void report(unsigned threadId, std::uint64_t prime) {
        Slot& slot = slots_[threadId - 1];
        const PrimeRecord record{prime, threadId, SystemClock::now()};
        ++slot.count;
        if (mode_ == PrintMode::AtEnd) {
            slot.recorded.push_back(record);
            return;
        }
        // The line is built before taking the lock and written in one piece, so lines
        // from different threads interleave with each other but never tear apart.
        const std::string line = formatPrimeLine(record);
        std::lock_guard<std::mutex> lock(printMutex_);
        std::cout << line << std::flush;
    }

    std::uint64_t count(unsigned threadId) const { return slots_[threadId - 1].count; }

    std::uint64_t total() const {
        std::uint64_t sum = 0;
        for (const Slot& slot : slots_) sum += slot.count;
        return sum;
    }

    // print = end only: prints every recorded prime in ascending order. Call it
    // after all workers have been joined.
    void printRecorded() const {
        std::vector<PrimeRecord> all;
        all.reserve(total());
        for (const Slot& slot : slots_) all.insert(all.end(), slot.recorded.begin(), slot.recorded.end());
        std::sort(all.begin(), all.end(), [](const PrimeRecord& a, const PrimeRecord& b) { return a.value < b.value; });
        for (const PrimeRecord& prime : all) std::cout << formatPrimeLine(prime);
    }

private:
    // One cache line per worker, so workers updating their own slots at the
    // same time don't slow each other down.
    struct alignas(64) Slot {
        std::uint64_t count = 0;
        std::vector<PrimeRecord> recorded;
    };

    PrintMode mode_;
    std::vector<Slot> slots_;
    std::mutex printMutex_;
};
