#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <exception>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

// written as one string so lines from different threads don't get mixed together
inline void logLine(const std::string& message) {
    std::cout << ("[consumer] " + message + "\n") << std::flush;
    if (!std::cout) {
        // the log can't report its own failure, so stderr is the only place left to say it
        std::cout.clear();
        std::cerr << ("[consumer] (stdout failed) " + message + "\n") << std::flush;
    }
}

// For cleaning up files or folders. Whatever they were for has already ended,
// but one that can't be removed is still taking up disk space, so it is reported.
inline void removeAndReport(const std::filesystem::path& path) noexcept {
    try {
        std::error_code error;
        std::filesystem::remove_all(path, error);
        if (error) logLine("could not remove " + path.string() + ": " + error.message());
    } catch (const std::exception& problem) {
        std::cerr << "[consumer] could not clean up a file: " << problem.what() << "\n" << std::flush;
    }
}

inline std::int64_t unixMillisNow() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

struct ActivityEvent {
    std::int64_t atMs;
    std::string kind;
    std::string producerId;
    std::string fileName;
    std::string detail;
};

// counters and the recent-events list shown in the GUI
class Activity {
public:
    std::atomic<std::uint64_t> accepted{0};
    std::atomic<std::uint64_t> saved{0};
    std::atomic<std::uint64_t> dropped{0};
    std::atomic<std::uint64_t> duplicates{0};
    std::atomic<std::uint64_t> failed{0};

    void record(std::string kind, std::string producerId, std::string fileName, std::string detail = "") {
        std::lock_guard<std::mutex> lock(mutex_);
        events_.push_front({unixMillisNow(), std::move(kind), std::move(producerId), std::move(fileName), std::move(detail)});
        if (events_.size() > maxEvents) events_.pop_back();
    }

    std::vector<ActivityEvent> recent() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return {events_.begin(), events_.end()};
    }

private:
    static constexpr std::size_t maxEvents = 40;
    mutable std::mutex mutex_;
    std::deque<ActivityEvent> events_;
};
