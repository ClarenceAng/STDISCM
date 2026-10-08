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

    tm local;
#ifdef _WIN32
    localtime_s(&local, &seconds);
#else
    localtime_r(&seconds, &local);
#endif

    uint64_t micros = chrono::duration_cast<chrono::microseconds>(when.time_since_epoch()).count() % 1000000;

    char timeText[64];
    if (showDate) {
        strftime(timeText, sizeof(timeText), "%Y-%m-%d %H:%M:%S", &local);
    } else {
        strftime(timeText, sizeof(timeText), "%H:%M:%S", &local);
    }

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
