#pragma once

#include <condition_variable>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <thread>
#include <vector>

using namespace std;

// the threads wait at this gate until all of them have been created
// 0 = wait, 1 = start, 2 = cancelled (couldn't create all the threads)
int gateState = 0;
mutex gateMutex;
condition_variable gateChanged;

void (*threadWork)(int) = nullptr;  // the function every thread runs

void threadMain(int threadId) {
    unique_lock<mutex> lock(gateMutex);
    while (gateState == 0) {
        gateChanged.wait(lock);
    }
    bool start = (gateState == 1);
    lock.unlock();

    if (start) threadWork(threadId);
}

// makes `count` threads that each run work(threadId) with ids 1 to count, then joins them
// nobody starts until every thread exists, so they all start together
// if not all threads can be created, the ones already made quit and the program exits with an error
// (running with fewer threads wouldn't work, divisibility testing would wait at the barrier forever)
void runThreads(int count, void (*work)(int)) {
    threadWork = work;
    gateState = 0;

    vector<thread> threads;
    threads.reserve(count);
    try {
        for (int i = 0; i < count; i++) {
            threads.push_back(thread(threadMain, i + 1));
        }
    } catch (exception& e) {
        gateMutex.lock();
        gateState = 2;
        gateMutex.unlock();
        gateChanged.notify_all();

        for (int i = 0; i < (int)threads.size(); i++) {
            threads[i].join();
        }
        cout << flush;
        cerr << "\nError: the system could only create " << threads.size() << " of the " << count
             << " threads asked for (" << e.what() << ")" << endl;
        exit(1);
    }

    gateMutex.lock();
    gateState = 1;
    gateMutex.unlock();
    gateChanged.notify_all();

    for (int i = 0; i < count; i++) {
        threads[i].join();
    }
}
