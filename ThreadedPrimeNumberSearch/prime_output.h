#pragma once

#include <algorithm>
#include <iostream>
#include <mutex>
#include <string>
#include <vector>

#include "config.h"
#include "timing.h"

using namespace std;

struct PrimeInfo {
    uint64_t number;
    int threadId;
    chrono::system_clock::time_point timeFound;
};

// each thread has its own ThreadData so saving a prime doesn't need a lock
// alignas(64) gives each one its own cache line so the threads don't slow each other down
struct alignas(64) ThreadData {
    uint64_t primeCount = 0;
    vector<PrimeInfo> primes;  // only used when print=end
};

vector<ThreadData> threadData;  // threadData[i] is thread i+1's
mutex printMutex;

string primeLine(PrimeInfo prime) {
    return "[" + getTimestamp(prime.timeFound, false) + "] [Thread " + to_string(prime.threadId) + "] " +
           to_string(prime.number) + "\n";
}

// called by a thread every time it finds a prime
void reportPrime(int threadId, uint64_t number) {
    PrimeInfo prime;
    prime.number = number;
    prime.threadId = threadId;
    prime.timeFound = chrono::system_clock::now();

    ThreadData& data = threadData[threadId - 1];
    data.primeCount++;

    if (!printImmediately) {
        data.primes.push_back(prime);
        return;
    }

    // build the whole line first, then print it while holding the lock
    // lines can come out in a different order but never get cut in half
    string line = primeLine(prime);
    printMutex.lock();
    cout << line << flush;
    printMutex.unlock();
}

uint64_t totalPrimes() {
    uint64_t total = 0;
    for (int i = 0; i < (int)threadData.size(); i++) {
        total += threadData[i].primeCount;
    }
    return total;
}

bool smallerNumber(const PrimeInfo& a, const PrimeInfo& b) {
    return a.number < b.number;
}

// print=end: prints the saved primes in order, call this after the threads are joined
void printAllPrimes() {
    vector<PrimeInfo> all;
    all.reserve(totalPrimes());
    for (int i = 0; i < (int)threadData.size(); i++) {
        all.insert(all.end(), threadData[i].primes.begin(), threadData[i].primes.end());
    }
    sort(all.begin(), all.end(), smallerNumber);
    for (int i = 0; i < (int)all.size(); i++) {
        cout << primeLine(all[i]);
    }
}
