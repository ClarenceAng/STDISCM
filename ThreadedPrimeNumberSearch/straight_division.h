#pragma once

// division=straight: 1 to y is split into one block per thread
// ex. 1-1000 with 4 threads = 1-250, 251-500, 501-750, 751-1000
// every thread checks its own block and they don't wait for each other until the join

#include <chrono>
#include <vector>

#include "config.h"
#include "prime_output.h"
#include "primes.h"
#include "run_threads.h"
#include "timing.h"

using namespace std;

vector<Range> blocks;                                // blocks[i] is thread i+1's block
vector<chrono::steady_clock::duration> finishTimes;  // how long after the start each thread finished

void makeBlocks() {
    blocks.resize(numThreads);
    for (int i = 0; i < numThreads; i++) {
        blocks[i] = splitRange(1, limit, numThreads, i);
    }
}

void searchBlock(int threadId) {
    Range block = blocks[threadId - 1];
    if (block.first <= block.last) {
        uint64_t n = block.first;
        while (true) {
            if (isPrime(n)) reportPrime(threadId, n);
            if (n == block.last) break;
            n++;
        }
    }
    finishTimes[threadId - 1] = chrono::steady_clock::now() - startTime;
}

void runStraightDivision() {
    finishTimes.resize(numThreads);
    runThreads(numThreads, searchBlock);
}
