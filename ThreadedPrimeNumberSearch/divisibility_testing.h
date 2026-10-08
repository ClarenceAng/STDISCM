#pragma once

// division=divisibility: numbers are checked one at a time (2, 3, 4, ... y)
// for each number n the divisors 2 to sqrt(n) are split between the threads
// a barrier makes every thread finish n before anyone moves on to n+1
// the threads are only created once and reused for every number

#include <atomic>
#include <barrier>

#include "config.h"
#include "prime_output.h"
#include "primes.h"
#include "run_threads.h"

using namespace std;

uint64_t currentNumber = 2;        
bool searchDone = false;          
atomic<bool> divisorFound(false); 

thread_local int myThreadId = 0;  

void finishNumber() noexcept {
    if (!divisorFound) {
        reportPrime(myThreadId, currentNumber);
    }
    divisorFound = false;

    // stop before ++ because if y is the biggest uint64, currentNumber++ would wrap to 0
    if (currentNumber == limit) {
        searchDone = true;
    } else {
        currentNumber++;
    }
}

typedef barrier<void (*)() noexcept> NumberBarrier;
NumberBarrier* numberBarrier = nullptr;

void testDivisors(int threadId) {
    myThreadId = threadId;
    while (!searchDone) {
        uint64_t n = currentNumber;
        Range divisors = splitRange(2, intSqrt(n), numThreads, threadId - 1);

        for (uint64_t d = divisors.first; d <= divisors.last; d++) {
            if (divisorFound.load(memory_order_relaxed)) break;  // another thread found so stop early
            if (n % d == 0) {
                divisorFound.store(true, memory_order_relaxed);
                break;
            }
        }
        numberBarrier->arrive_and_wait();
    }
}

void runDivisibilityTesting() {
    searchDone = (currentNumber > limit);  
    NumberBarrier barrierForNumbers(numThreads, finishNumber);
    numberBarrier = &barrierForNumbers;
    runThreads(numThreads, testDivisors);
}
