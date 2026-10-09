#include <iomanip>
#include <iostream>
#include <new>
#include <string>

#include "config.h"
#include "divisibility_testing.h"
#include "prime_output.h"
#include "primes.h"
#include "straight_division.h"
#include "timing.h"

using namespace std;

string rangeToString(Range range, string ifEmpty) {
    if (range.first > range.last) return ifEmpty;
    return to_string(range.first) + " - " + to_string(range.last);
}

void runSearch() {
    cout << "Threaded prime number search\n";
    cout << "Threads:  " << numThreads << " | Search range: 1 - " << limit << "\n";
    if (printImmediately) {
        cout << "Print:    immediately, by the thread that finds each prime\n";
    } else {
        cout << "Print:    after all threads finish, by the main thread\n";
    }
    if (straightDivision) {
        cout << "Division: straight division of the search range\n\n";
    } else {
        cout << "Division: threads split the divisibility test of each number\n\n";
    }

    if (straightDivision) {
        makeBlocks();
        for (int i = 0; i < numThreads; i++) {
            if (numThreads > 64 && i == 10) {
                cout << "  ... " << numThreads - 20 << " more threads ...\n";
                i = numThreads - 10;
            }
            cout << "  Thread " << i + 1 << " searches " << rangeToString(blocks[i], "(no numbers)") << "\n";
        }
    } else {
        uint64_t root = intSqrt(limit);
        Range allDivisors;
        allDivisors.first = 2;
        allDivisors.last = root;
        cout << "Numbers are tested one after another. Example split for n = " << limit << " (divisors "
             << rangeToString(allDivisors, "(no divisors)") << "):\n";
        for (int i = 0; i < numThreads; i++) {
            if (numThreads > 64 && i == 10) {
                cout << "  ... " << numThreads - 20 << " more threads ...\n";
                i = numThreads - 10;
            }
            cout << "  Thread " << i + 1 << " tests " << rangeToString(splitRange(2, root, numThreads, i), "(no divisors)")
                 << "\n";
        }
    }

    threadData.resize(numThreads);
    startTime = chrono::steady_clock::now();
    cout << "\nStart time: " << getTimestamp(chrono::system_clock::now(), true) << "\n\n" << flush;

    if (straightDivision) {
        runStraightDivision();
    } else {
        runDivisibilityTesting();
    }
    chrono::steady_clock::duration searchTime = chrono::steady_clock::now() - startTime;

    if (!printImmediately) {
        cout << "All threads joined after " << msString(searchTime) << ". Primes (timestamps show when each was found):\n\n";
        printAllPrimes();
    }

    chrono::steady_clock::duration totalTime = chrono::steady_clock::now() - startTime;
    cout << "\nEnd time:   " << getTimestamp(chrono::system_clock::now(), true) << "\n";
    cout << "Elapsed:    " << msString(totalTime);
    if (!printImmediately) {
        cout << "  (search " << msString(searchTime) << " + printing " << msString(totalTime - searchTime) << ")";
    }
    cout << "\nPrimes found: " << totalPrimes() << "\n";

    if (straightDivision) {
        cout << "\nPer thread (the join waits for the slowest one):\n";

        int width = 0;
        for (int i = 0; i < numThreads; i++) {
            int length = (int)rangeToString(blocks[i], "(no numbers)").size();
            if (length > width) width = length;
        }

        for (int i = 0; i < numThreads; i++) {
            if (numThreads > 64 && i == 10) {
                cout << "  ... " << numThreads - 20 << " more threads ...\n";
                i = numThreads - 10;
            }
            cout << "  Thread " << left << setw(5) << i + 1 << setw(width) << rangeToString(blocks[i], "(no numbers)")
                 << right << setw(9) << threadData[i].primeCount << " primes   finished after "
                 << msString(finishTimes[i]) << "\n";
        }

        int slowest = 0;
        for (int i = 1; i < numThreads; i++) {
            if (finishTimes[i] > finishTimes[slowest]) slowest = i;
        }
        cout << "Slowest:  Thread " << slowest + 1 << ", finished after " << msString(finishTimes[slowest]) << "\n";
    } else {
        cout << "Numbers tested: " << limit - 1 << "\n";
    }
}

int main(int argc, char* argv[]) {
    ios::sync_with_stdio(false);

    if (argc > 2) {
        cerr << "Usage: prime_search [config file]   (the config file defaults to config.txt)\n";
        return 1;
    }
    string configFile = "config.txt";
    if (argc == 2) configFile = argv[1];

    loadConfig(configFile);

    try {
        runSearch();
    } catch (bad_alloc&) {
        cout << flush;
        cerr << "\nError: not enough memory to run " << numThreads << " threads up to y = " << limit << endl;
        return 1;
    }
    return 0;
}
