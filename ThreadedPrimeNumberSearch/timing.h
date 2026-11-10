#pragma once

#include <chrono>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <string>

using namespace std;

chrono::steady_clock::time_point startTime;

string getTimestamp(chrono::system_clock::time_point when, bool showDate) {
    time_t seconds = chrono::system_clock::to_time_t(when);

    // a clock the system can't convert shouldn't stop the search, but it mustn't print garbage either
    tm local;
#ifdef _WIN32
    if (localtime_s(&local, &seconds) != 0) return "(time unavailable)";
#else
    if (localtime_r(&seconds, &local) == nullptr) return "(time unavailable)";
#endif

    uint64_t micros = chrono::duration_cast<chrono::microseconds>(when.time_since_epoch()).count() % 1000000;

    char timeText[64];
    const char* format = showDate ? "%Y-%m-%d %H:%M:%S" : "%H:%M:%S";
    if (strftime(timeText, sizeof(timeText), format, &local) == 0) return "(time unavailable)";

    char result[96];
    snprintf(result, sizeof(result), "%s.%06" PRIu64, timeText, micros);
    return result;
}

string msString(chrono::steady_clock::duration duration) {
    double ms = chrono::duration<double, milli>(duration).count();
    char result[64];
    snprintf(result, sizeof(result), "%.3f ms", ms);
    return result;
}
