#pragma once

#include <charconv>
#include <cstdint>
#include <fstream>
#include <stdexcept>
#include <string>

// Sanity cap so a typo in the config can't try to spawn millions of threads.
constexpr std::uint64_t kMaxThreads = 1024;

struct Config {
    unsigned threads = 0;       // x: number of worker threads to create
    std::uint64_t limit = 0;    // y: search for primes in [1, limit]
};

namespace config_detail {

inline std::string trim(const std::string& text) {
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return "";
    const auto last = text.find_last_not_of(" \t\r\n");
    return text.substr(first, last - first + 1);
}

inline std::runtime_error lineError(const std::string& path, int lineNumber, const std::string& message) {
    return std::runtime_error(path + ", line " + std::to_string(lineNumber) + ": " + message);
}

inline std::uint64_t parseUnsigned(const std::string& value, const std::string& path, int lineNumber) {
    std::uint64_t result = 0;
    const char* end = value.data() + value.size();
    const auto [stop, error] = std::from_chars(value.data(), end, result);
    if (error != std::errc() || stop != end) {
        throw lineError(path, lineNumber, "'" + value + "' is not a non-negative whole number");
    }
    return result;
}

}  // namespace config_detail

// Reads a config file made of "key = value" lines. Blank lines are ignored and
// '#' starts a comment. Throws std::runtime_error describing the first problem.
inline Config loadConfig(const std::string& path) {
    using namespace config_detail;

    std::ifstream file(path);
    if (!file) throw std::runtime_error("cannot open config file '" + path + "'");

    Config config;
    bool hasThreads = false;
    bool hasLimit = false;

    std::string line;
    for (int lineNumber = 1; std::getline(file, line); ++lineNumber) {
        line = trim(line.substr(0, line.find('#')));
        if (line.empty()) continue;

        const auto equals = line.find('=');
        if (equals == std::string::npos) throw lineError(path, lineNumber, "expected 'key = value'");

        const std::string key = trim(line.substr(0, equals));
        const std::string value = trim(line.substr(equals + 1));

        if (key == "x") {
            const std::uint64_t threads = parseUnsigned(value, path, lineNumber);
            if (threads < 1 || threads > kMaxThreads) {
                throw lineError(path, lineNumber, "x (number of threads) must be between 1 and " + std::to_string(kMaxThreads));
            }
            config.threads = static_cast<unsigned>(threads);
            hasThreads = true;
        } else if (key == "y") {
            config.limit = parseUnsigned(value, path, lineNumber);
            if (config.limit < 1) throw lineError(path, lineNumber, "y (search limit) must be at least 1");
            hasLimit = true;
        } else {
            throw lineError(path, lineNumber, "unknown key '" + key + "' (expected 'x' or 'y')");
        }
    }

    if (!hasThreads) throw std::runtime_error(path + ": missing 'x' (number of threads)");
    if (!hasLimit) throw std::runtime_error(path + ": missing 'y' (search limit)");
    return config;
}
