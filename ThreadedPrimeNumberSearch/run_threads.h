#pragma once

// Creates, starts and joins the worker threads for both division schemes.

#include <atomic>
#include <exception>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

// Runs work(threadId) on `count` new threads, with ids 1 .. count, and joins
// them all.
//
// Each thread waits at a start gate until all `count` threads exist, so they
// begin together. There is no limit on `count` except what the operating
// system allows. If the system refuses to create a thread, the threads already
// created leave without doing any work and a std::runtime_error says how many
// could be created. Running with only some of the threads is not an option:
// divisibility testing would wait forever at its barrier for the missing ones.
template <typename Work>
void runThreads(unsigned count, Work work) {
    enum Gate : int { Closed, Open, Cancelled };
    std::atomic<int> gate{Closed};

    std::vector<std::thread> workers;
    workers.reserve(count);
    try {
        for (unsigned i = 0; i < count; ++i) {
            workers.emplace_back([&gate, &work, threadId = i + 1] {
                gate.wait(Closed);
                if (gate.load() == Open) work(threadId);
            });
        }
    } catch (const std::exception& error) {
        gate.store(Cancelled);
        gate.notify_all();
        for (auto& worker : workers) worker.join();
        throw std::runtime_error("the system could only create " + std::to_string(workers.size()) + " of the " +
                                 std::to_string(count) + " threads asked for (" + error.what() + ")");
    }

    gate.store(Open);
    gate.notify_all();
    for (auto& worker : workers) worker.join();
}
