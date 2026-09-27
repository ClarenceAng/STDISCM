#pragma once

#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <string>

using SystemClock = std::chrono::system_clock;   // wall-clock time, for printed timestamps
using SteadyClock = std::chrono::steady_clock;   // monotonic, for measuring durations

// Formats a wall-clock time as "2026-09-26 13:45:07.123456", or just
// "13:45:07.123456" when includeDate is false. Safe to call from any thread.
inline std::string formatTimestamp(SystemClock::time_point time, bool includeDate = true) {
    const std::time_t seconds = SystemClock::to_time_t(time);
    std::tm local{};
#if defined(_WIN32)
    localtime_s(&local, &seconds);
#else
    localtime_r(&seconds, &local);
#endif
    const auto sinceEpoch = std::chrono::duration_cast<std::chrono::microseconds>(time.time_since_epoch());
    const auto micros = sinceEpoch.count() % 1'000'000;

    std::ostringstream out;
    out << std::put_time(&local, includeDate ? "%Y-%m-%d %H:%M:%S" : "%H:%M:%S")
        << '.' << std::setfill('0') << std::setw(6) << micros;
    return out.str();
}

inline std::string formatMilliseconds(SteadyClock::duration duration) {
    std::ostringstream out;
    out << std::fixed << std::setprecision(3)
        << std::chrono::duration<double, std::milli>(duration).count() << " ms";
    return out.str();
}
