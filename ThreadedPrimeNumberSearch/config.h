#pragma once

#include <cctype>
#include <charconv>
#include <cstdint>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>

// Sanity cap on y, so a typo in the config can't start a search that would run
// for days or, with print = end, run out of memory recording the primes. x has
// no cap: the program creates as many threads as the operating system allows.
constexpr std::uint64_t kMaxLimit = 1'000'000'000;

// When primes are printed.
enum class PrintMode {
    Immediate,   // "immediate": by the thread that found it, the moment it is found
    AtEnd,       // "end": by the main thread, after every worker has been joined
};

// How the search is split across the threads.
enum class Division {
    Straight,       // "straight": [1, y] is cut into one contiguous block per thread
    Divisibility,   // "divisibility": numbers are tested one at a time, each number's divisors are split across the threads
};

struct Config {
    unsigned threads = 0;       // x: number of worker threads to create
    std::uint64_t limit = 0;    // y: search for primes in [1, limit]
    PrintMode print = PrintMode::Immediate;
    Division division = Division::Straight;
};

namespace config_detail {

inline std::string trim(const std::string& text) {
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return "";
    const auto last = text.find_last_not_of(" \t\r\n");
    return text.substr(first, last - first + 1);
}

inline std::string lowercase(std::string text) {
    for (char& c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return text;
}

inline std::runtime_error lineError(const std::string& path, int lineNumber, const std::string& message) {
    return std::runtime_error(path + ", line " + std::to_string(lineNumber) + ": " + message);
}

// Parses digits only: no sign, spaces, separators or decimals.
inline std::uint64_t parseWholeNumber(const std::string& key, const std::string& value, std::uint64_t min,
                                      std::uint64_t max, const std::string& path, int lineNumber) {
    std::uint64_t result = 0;
    const char* end = value.data() + value.size();
    const auto [stop, error] = std::from_chars(value.data(), end, result);
    if (error == std::errc::result_out_of_range || (error == std::errc() && stop == end && result > max)) {
        throw lineError(path, lineNumber, key + " = " + value + " is too large (the maximum is " + std::to_string(max) + ")");
    }
    if (error != std::errc() || stop != end) {
        throw lineError(path, lineNumber, key + " must be a whole number written with digits only, not '" + value + "'");
    }
    if (result < min) {
        throw lineError(path, lineNumber, key + " must be at least " + std::to_string(min) + ", not " + value);
    }
    return result;
}

}  // namespace config_detail

// Reads a config file made of "key = value" lines. Blank lines are ignored and
// '#' starts a comment. Each of x, y, print and division must appear exactly
// once. Throws std::runtime_error describing the first problem found.
inline Config loadConfig(const std::string& path) {
    using namespace config_detail;

    std::ifstream file(path);
    if (!file) {
        throw std::runtime_error("cannot open config file '" + path +
                                 "'. Run the program from the folder that contains it, or pass its path as the first argument.");
    }

    Config config;
    // Line each setting was read from, or 0 if it hasn't been seen yet.
    int threadsLine = 0;
    int limitLine = 0;
    int printLine = 0;
    int divisionLine = 0;

    std::string line;
    for (int lineNumber = 1; std::getline(file, line); ++lineNumber) {
        if (lineNumber == 1 && line.rfind("\xEF\xBB\xBF", 0) == 0) {
            line.erase(0, 3);   // UTF-8 byte order mark, e.g. Notepad's "UTF-8 with BOM"
        }
        if (line.find('\0') != std::string::npos) {
            throw std::runtime_error(path + " looks like a UTF-16 file (Windows PowerShell's '>' writes those). "
                                            "Save it as UTF-8 or ANSI instead.");
        }

        line = trim(line.substr(0, line.find('#')));
        if (line.empty()) continue;

        const auto equals = line.find('=');
        if (equals == std::string::npos) throw lineError(path, lineNumber, "expected 'key = value', got '" + line + "'");

        const std::string key = trim(line.substr(0, equals));
        const std::string value = trim(line.substr(equals + 1));
        if (key.empty()) throw lineError(path, lineNumber, "missing the setting name before '='");

        int* seenOn = nullptr;
        if (key == "x") seenOn = &threadsLine;
        else if (key == "y") seenOn = &limitLine;
        else if (key == "print") seenOn = &printLine;
        else if (key == "division") seenOn = &divisionLine;
        else throw lineError(path, lineNumber, "unknown setting '" + key + "' (expected x, y, print or division)");

        if (*seenOn != 0) {
            throw lineError(path, lineNumber, key + " is already set on line " + std::to_string(*seenOn));
        }
        *seenOn = lineNumber;
        if (value.empty()) throw lineError(path, lineNumber, key + " has no value");

        if (key == "x") {
            config.threads = static_cast<unsigned>(
                parseWholeNumber(key, value, 1, std::numeric_limits<unsigned>::max(), path, lineNumber));
        } else if (key == "y") {
            config.limit = parseWholeNumber(key, value, 1, kMaxLimit, path, lineNumber);
        } else if (key == "print") {
            const std::string mode = lowercase(value);
            if (mode == "immediate") {
                config.print = PrintMode::Immediate;
            } else if (mode == "end") {
                config.print = PrintMode::AtEnd;
            } else {
                throw lineError(path, lineNumber, "print must be 'immediate' or 'end', not '" + value + "'");
            }
        } else {
            const std::string mode = lowercase(value);
            if (mode == "straight") {
                config.division = Division::Straight;
            } else if (mode == "divisibility") {
                config.division = Division::Divisibility;
            } else {
                throw lineError(path, lineNumber, "division must be 'straight' or 'divisibility', not '" + value + "'");
            }
        }
    }
    if (file.bad()) throw std::runtime_error("error while reading config file '" + path + "'");

    std::string missing;
    const auto requireSet = [&missing](int seenOn, const char* description) {
        if (seenOn != 0) return;
        if (!missing.empty()) missing += ", ";
        missing += description;
    };
    requireSet(threadsLine, "x (number of threads)");
    requireSet(limitLine, "y (search limit)");
    requireSet(printLine, "print (immediate or end)");
    requireSet(divisionLine, "division (straight or divisibility)");
    if (!missing.empty()) throw std::runtime_error(path + ": missing " + missing);

    return config;
}
