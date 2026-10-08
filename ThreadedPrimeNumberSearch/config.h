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

int numThreads = 0;
int maxThreads = 0;
uint64_t limit = 0;
bool printImmediately = true;
bool straightDivision = true;

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

string toLower(string s) {
    for (int i = 0; i < (int)s.size(); i++) {
        if (s[i] >= 'A' && s[i] <= 'Z') s[i] = (char)(s[i] + 32);
    }
    return s;
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

// reads a value that must be one of two words (upper or lower case); true means trueWord, false means falseWord
bool readChoice(string key, string value, string trueWord, string falseWord, string fileName, int lineNumber) {
    string word = toLower(value);
    if (word != trueWord && word != falseWord) {
        lineError(fileName, lineNumber, key + " must be '" + trueWord + "' or '" + falseWord + "', not '" + value + "'");
    }
    return word == trueWord;
}

void loadConfig(string fileName) {
    ifstream file(fileName);
    if (!file) {
        configError("cannot open config file " + fileName);
    }

    // line each setting was found on (0 = not found yet)
    map<string, int> lineOf = {{"x", 0}, {"max_threads", 0}, {"y", 0}, {"print", 0}, {"division", 0}};

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
            lineError(fileName, lineNumber, "unknown setting '" + key + "' (expected x, max_threads, y, print or division)");
        }
        if (lineOf[key] != 0) {
            lineError(fileName, lineNumber, key + " is already set on line " + to_string(lineOf[key]));
        }

        if (value == "") {
            lineError(fileName, lineNumber, key + " has no value");
        }

        lineOf[key] = lineNumber;
        if (key == "x") {
            numThreads = (int)readNumber(key, value, 1, INT_MAX, fileName, lineNumber);
        } else if (key == "max_threads") {
            maxThreads = (int)readNumber(key, value, 1, INT_MAX, fileName, lineNumber);
        } else if (key == "y") {
            limit = readNumber(key, value, 1, UINT64_MAX, fileName, lineNumber);
        } else if (key == "print") {
            printImmediately = readChoice(key, value, "immediate", "end", fileName, lineNumber);
        } else if (key == "division") {
            straightDivision = readChoice(key, value, "straight", "divisibility", fileName, lineNumber);
        }
    }

    if (file.bad()) {
        configError("error while reading config file '" + fileName + "'");
    }

    string missing = "";
    if (lineOf["x"] == 0) missing += ", x (number of threads)";
    if (lineOf["max_threads"] == 0) missing += ", max_threads (largest x allowed)";
    if (lineOf["y"] == 0) missing += ", y (search limit)";
    if (lineOf["print"] == 0) missing += ", print (immediate or end)";
    if (lineOf["division"] == 0) missing += ", division (straight or divisibility)";
    if (missing != "") {
        configError(fileName + ": missing " + missing.substr(2));  // substr(2) removes the first ", "
    }

    if (numThreads > maxThreads) {
        lineError(fileName, lineOf["x"], "x = " + to_string(numThreads) + " is more than max_threads = " +
                                             to_string(maxThreads) + " (line " + to_string(lineOf["max_threads"]) + ")");
    }
}
