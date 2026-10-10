#pragma once

#include <climits>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>

using namespace std;

const uint64_t longestClearTime = 15;

int maxInstances = 0;
long long numTanks = 0;
long long numHealers = 0;
long long numDps = 0;
int minClearTime = 0;
int maxClearTime = 0;

void configError(string message) {
    cerr << "Config error: " << message << endl;
    exit(1);
}

void lineError(string fileName, int lineNumber, string message) {
    configError(fileName + ", line " + to_string(lineNumber) + ": " + message);
}

bool isSpace(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

string trim(string s) {
    int start = 0;
    int end = (int)s.size() - 1;
    while (start <= end && isSpace(s[start])) start++;
    while (end >= start && isSpace(s[end])) end--;
    return s.substr(start, end - start + 1);
}

// takes the UTF-8 byte order mark (first line only), the '#' comment and the spaces around a line off
string cleanLine(string line, string fileName, int lineNumber) {
    if (lineNumber == 1 && line.substr(0, 3) == "\xEF\xBB\xBF") {
        line = line.substr(3);
    }
    if (line.find('\0') != string::npos) {
        configError(fileName + " looks like a UTF-16 file (Windows PowerShell's '>' writes those). "
                               "Save it as UTF-8 or ANSI instead.");
    }
    return trim(line.substr(0, line.find('#')));
}

// reads a whole number like "1000" and checks that it's between minValue and maxValue
uint64_t readNumber(string key, string value, uint64_t minValue, uint64_t maxValue, string fileName, int lineNumber) {
    // digits only (no -, +, commas, decimals...)
    for (int i = 0; i < (int)value.size(); i++) {
        if (value[i] < '0' || value[i] > '9') {
            lineError(fileName, lineNumber, key + " must be a whole number written with digits only, not '" + value + "'");
        }
    }

    uint64_t number = 0;
    bool tooBig = false;
    try {
        number = stoull(value);
    } catch (out_of_range&) {
        tooBig = true;
    }

    if (tooBig || number > maxValue) {
        lineError(fileName, lineNumber, key + " = " + value + " is too large (the maximum is " + to_string(maxValue) + ")");
    }
    if (number < minValue) {
        lineError(fileName, lineNumber, key + " must be at least " + to_string(minValue) + ", not " + value);
    }
    return number;
}

void loadConfig(string fileName) {
    ifstream file(fileName);
    if (!file) {
        configError("cannot open config file " + fileName);
    }

    // line each setting was found on (0 = not found yet)
    map<string, int> lineOf = {{"n", 0}, {"t", 0}, {"h", 0}, {"d", 0}, {"t1", 0}, {"t2", 0}};

    string line;
    int lineNumber = 0;
    while (getline(file, line)) {
        lineNumber++;
        line = cleanLine(line, fileName, lineNumber);
        if (line == "") continue;

        size_t equals = line.find('=');
        if (equals == string::npos) {
            lineError(fileName, lineNumber, "expected 'key = value', got '" + line + "'");
        }
        string key = trim(line.substr(0, equals));
        string value = trim(line.substr(equals + 1));

        if (key == "") {
            lineError(fileName, lineNumber, "missing the setting name before '='");
        }
        if (lineOf.count(key) == 0) {
            lineError(fileName, lineNumber, "unknown setting '" + key + "' (expected n, t, h, d, t1 or t2)");
        }
        if (lineOf[key] != 0) {
            lineError(fileName, lineNumber, key + " is already set on line " + to_string(lineOf[key]));
        }

        if (value == "") {
            lineError(fileName, lineNumber, key + " has no value");
        }

        lineOf[key] = lineNumber;
        if (key == "n") {
            maxInstances = (int)readNumber(key, value, 1, INT_MAX, fileName, lineNumber);
        } else if (key == "t") {
            numTanks = (long long)readNumber(key, value, 0, INT_MAX, fileName, lineNumber);
        } else if (key == "h") {
            numHealers = (long long)readNumber(key, value, 0, INT_MAX, fileName, lineNumber);
        } else if (key == "d") {
            numDps = (long long)readNumber(key, value, 0, INT_MAX, fileName, lineNumber);
        } else if (key == "t1") {
            minClearTime = (int)readNumber(key, value, 0, longestClearTime, fileName, lineNumber);
        } else if (key == "t2") {
            maxClearTime = (int)readNumber(key, value, 0, longestClearTime, fileName, lineNumber);
        }
    }

    if (file.bad()) {
        configError("error while reading config file '" + fileName + "'");
    }

    string missing = "";
    if (lineOf["n"] == 0) missing += ", n (maximum concurrent instances)";
    if (lineOf["t"] == 0) missing += ", t (tank players in queue)";
    if (lineOf["h"] == 0) missing += ", h (healer players in queue)";
    if (lineOf["d"] == 0) missing += ", d (DPS players in queue)";
    if (lineOf["t1"] == 0) missing += ", t1 (minimum clear time)";
    if (lineOf["t2"] == 0) missing += ", t2 (maximum clear time)";
    if (missing != "") {
        configError(fileName + ": missing " + missing.substr(2));  // substr(2) removes the first ", "
    }

    if (minClearTime > maxClearTime) {
        lineError(fileName, lineOf["t1"], "t1 = " + to_string(minClearTime) + " is more than t2 = " +
                                              to_string(maxClearTime) + " (line " + to_string(lineOf["t2"]) + ")");
    }
}
