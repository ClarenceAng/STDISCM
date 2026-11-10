// The command line for the media upload service. It starts the consumer, then
// keeps listening for video or folder paths typed at its prompt. Each path
// starts one producer container that uploads it and finishes, with at most p
// running at once. docker-compose.yml is rewritten as producers start, so the
// containers follow config.txt and what was typed.
//
// usage: upload_cli <config file> <compose file> [video or folder] ...

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>
#else
#include <signal.h>
#endif

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#include "../common/error_text.h"
#include "../common/video_files.h"

using namespace std;
namespace fs = std::filesystem;

const uint64_t grpcPort = 50051;
const string containerFolder = "/input";  // where a producer's folder appears inside its container
const int consumerStartSeconds = 60;       // how long the consumer may take to start listening
const string consumerReadyText = "[consumer] ready:";
const string abandonedText = "abandoned:";  // the consumer logs this for each accepted video it never finished

struct Setting {
    string key;
    string description;
    uint64_t minValue;
    uint64_t maxValue;
    bool required;
    uint64_t value;  // the default, for optional settings
    int line;        // line it was found on (0 = not found yet)
};

vector<Setting> settings = {
    {"p", "producer containers", 1, 64, true, 0, 0},
    {"c", "consumer threads", 1, 64, true, 0, 0},
    {"q", "max queue length", 1, 100000, true, 0, 0},
    {"http_port", "browser GUI port", 1, 65535, false, 8080, 0},
    {"compress_height", "compressed video height", 144, 2160, false, 480, 0},
    {"scan_seconds", "seconds between folder scans", 1, 3600, false, 2, 0},
    {"max_upload_mb", "largest upload in MB", 1, 102400, false, 2048, 0},
};

Setting* findSetting(const string& key) {
    for (Setting& setting : settings) {
        if (setting.key == key) return &setting;
    }
    return nullptr;
}

uint64_t valueOf(const string& key) {
    return findSetting(key)->value;
}

[[noreturn]] void lineError(const string& fileName, int lineNumber, const string& message) {
    throw runtime_error(fileName + ", line " + to_string(lineNumber) + ": " + message);
}

string trim(const string& text) {
    const string spaces = " \t\r\n";
    size_t start = text.find_first_not_of(spaces);
    if (start == string::npos) return "";
    return text.substr(start, text.find_last_not_of(spaces) - start + 1);
}

// takes the UTF-8 byte order mark (first line only), the '#' comment and the spaces around a line off
string cleanLine(string line, const string& fileName, int lineNumber) {
    if (lineNumber == 1 && line.substr(0, 3) == "\xEF\xBB\xBF") {
        line = line.substr(3);
    }
    if (line.find('\0') != string::npos) {
        throw runtime_error(fileName + " looks like a UTF-16 file (Windows PowerShell's '>' writes those). "
                                       "Save it as UTF-8 or ANSI instead.");
    }
    return trim(line.substr(0, line.find('#')));
}

uint64_t readNumber(const Setting& setting, const string& value, const string& fileName, int lineNumber) {
    for (char digit : value) {
        if (digit < '0' || digit > '9') {
            lineError(fileName, lineNumber, setting.key + " must be a whole number written with digits only, not '" + value + "'");
        }
    }

    uint64_t number = 0;
    try {
        number = stoull(value);
    } catch (const out_of_range&) {
        number = UINT64_MAX;
    }

    if (number > setting.maxValue) {
        lineError(fileName, lineNumber, setting.key + " = " + value + " is too large (the maximum is " + to_string(setting.maxValue) + ")");
    }
    if (number < setting.minValue) {
        lineError(fileName, lineNumber, setting.key + " must be at least " + to_string(setting.minValue) + ", not " + value);
    }
    return number;
}

void loadConfig(const string& fileName) {
    ifstream file(fileName);
    if (!file) {
        throw runtime_error("cannot open config file " + fileName);
    }

    string line;
    int lineNumber = 0;
    while (getline(file, line)) {
        lineNumber++;
        line = cleanLine(line, fileName, lineNumber);
        if (line.empty()) continue;

        size_t equals = line.find('=');
        if (equals == string::npos) {
            lineError(fileName, lineNumber, "expected 'key = value', got '" + line + "'");
        }
        string key = trim(line.substr(0, equals));
        string value = trim(line.substr(equals + 1));

        if (key.empty()) {
            lineError(fileName, lineNumber, "missing the setting name before '='");
        }

        Setting* setting = findSetting(key);
        if (setting == nullptr) {
            lineError(fileName, lineNumber, "unknown setting '" + key + "'");
        }
        if (setting->line != 0) {
            lineError(fileName, lineNumber, key + " is already set on line " + to_string(setting->line));
        }
        if (value.empty()) {
            lineError(fileName, lineNumber, key + " has no value");
        }

        setting->value = readNumber(*setting, value, fileName, lineNumber);
        setting->line = lineNumber;
    }

    if (file.bad()) {
        throw runtime_error("error while reading config file " + fileName);
    }

    string missing;
    for (const Setting& setting : settings) {
        if (setting.required && setting.line == 0) {
            missing += ", " + setting.key + " (" + setting.description + ")";
        }
    }
    if (!missing.empty()) {
        throw runtime_error(fileName + ": missing " + missing.substr(2));
    }
}

const int firstPathArgument = 3;
const string promptText = "upload> ";

// what one producer was given
struct Input {
    fs::path path;  // full path on this machine
    bool isFile;    // a single video rather than a folder
};

atomic<bool> stopping{false};
atomic<bool> consumerDown{false};  // it stopped by itself, so new paths are refused
bool containersMayExist = false;   // Docker works and start-up got far enough to have created something
string composeFile;
string rootFolder;

mutex consoleMutex;
bool promptShowing = false;

// Lock order is composeMutex, then stateMutex. consoleMutex and spawnMutex are
// only held for one short step and nothing else is locked inside them.
mutex spawnMutex;
mutex composeMutex;  // docker-compose.yml and the docker compose commands that read it

mutex stateMutex;
condition_variable pathWaiting;
deque<Input> waitingPaths;        // typed while every producer was busy, oldest first
map<int, Input> activeProducers;  // producer number -> what it is uploading
set<int> freeProducers;

#ifdef _WIN32
BOOL WINAPI onConsoleSignal(DWORD signal) {
    if (signal == CTRL_C_EVENT || signal == CTRL_BREAK_EVENT) {
        stopping = true;
        // the prompt may be blocked waiting for a line; ERROR_NOT_FOUND only means it wasn't
        if (!CancelIoEx(GetStdHandle(STD_INPUT_HANDLE), nullptr) && GetLastError() != ERROR_NOT_FOUND) {
            cerr << "\nPress Enter to finish stopping.\n" << flush;
        }
        return TRUE;
    }
    return FALSE;
}
#else
void onStopSignal(int) {
    stopping = true;
}
#endif

void setUpConsole() {
#ifdef _WIN32
    if (!SetConsoleOutputCP(CP_UTF8)) {
        cerr << "Warning: could not switch the console to UTF-8, some file names may look wrong (Windows error " << GetLastError() << ")\n";
    }
    if (!SetConsoleCtrlHandler(onConsoleSignal, TRUE)) {
        cerr << "Warning: Ctrl+C won't stop the containers, type exit instead (Windows error " << GetLastError() << ")\n";
    }
#else
    // no SA_RESTART, so a blocked read returns and the prompt loop can end
    struct sigaction action {};
    action.sa_handler = onStopSignal;
    if (sigaction(SIGINT, &action, nullptr) != 0 || sigaction(SIGTERM, &action, nullptr) != 0) {
        cerr << "Warning: Ctrl+C won't stop the containers, type exit instead (" << lastErrorText() << ")\n";
    }
    // a closed terminal should show up as failed writes, not end the program mid-cleanup
    if (signal(SIGPIPE, SIG_IGN) == SIG_ERR) {
        cerr << "Warning: if this terminal is closed, the containers may be left running (" << lastErrorText() << ")\n";
    }
#endif
}

void showPrompt() {
    lock_guard<mutex> lock(consoleMutex);
    if (!promptShowing && !stopping) {
        cout << promptText << flush;
        promptShowing = true;
    }
}

// Producers report while the prompt is up, so each message is written over it
// and the prompt is drawn again underneath.
void printLine(const string& text) {
    lock_guard<mutex> lock(consoleMutex);
    cout << '\r' << text << '\n';
    promptShowing = !stopping;
    if (promptShowing) cout << promptText;
    cout << flush;
}

// one line from the terminal as UTF-8; false means the input has ended
bool readLine(string& line) {
    line.clear();
#ifdef _WIN32
    // the console's narrow input can't hold every file name, its wide input can
    HANDLE input = GetStdHandle(STD_INPUT_HANDLE);
    DWORD mode = 0;
    if (GetConsoleMode(input, &mode)) {
        wstring wide;
        wchar_t buffer[1024];
        do {
            DWORD got = 0;
            if (!ReadConsoleW(input, buffer, 1024, &got, nullptr)) {
                // Ctrl+C cancels this read on purpose
                const DWORD error = GetLastError();
                if (error != ERROR_OPERATION_ABORTED && !stopping) {
                    printLine("Could not read from the console (Windows error " + to_string(error) + ")");
                }
                return false;
            }
            if (got == 0) return false;
            wide.append(buffer, got);
        } while (wide.back() != L'\n');

        if (wide[0] == 0x1a) return false;  // Ctrl+Z
        const int size = WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
        if (size <= 0) {
            printLine("Could not understand that line, please type it again (Windows error " + to_string(GetLastError()) + ")");
            return true;
        }
        line.resize(static_cast<size_t>(size));
        if (WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), &line[0], size, nullptr, nullptr) != size) {
            printLine("Could not understand that line, please type it again (Windows error " + to_string(GetLastError()) + ")");
            line.clear();
        }
        return true;
    }
#endif
    if (getline(cin, line)) return true;
    if (!stopping) {
        printLine(cin.eof() ? "The input has ended, so no more paths can be typed." : "Could not read from the input.");
    }
    return false;
}

FILE* openPipe(const string& command) {
    // On Windows a child inherits any pipe being set up at the same moment, which
    // would keep that pipe open after its own process ends. One at a time avoids it.
    lock_guard<mutex> lock(spawnMutex);
#ifdef _WIN32
    return _popen(command.c_str(), "r");
#else
    return popen(command.c_str(), "r");
#endif
}

int closePipe(FILE* pipe) {
#ifdef _WIN32
    return _pclose(pipe);
#else
    return pclose(pipe);
#endif
}

// runs a command, collects what it prints and returns its exit status
int runCaptured(const string& command, string& output) {
    output.clear();
    errno = 0;
    FILE* pipe = openPipe(command);
    if (pipe == nullptr) {
        output = "could not run the command: " + lastErrorText();
        return -1;
    }

    char buffer[4096];
    while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
        output += buffer;
    }
    const bool readFailed = ferror(pipe) != 0;
    const int status = closePipe(pipe);
    if (readFailed) {
        output += "\n(not all of this command's output could be read)";
        return status != 0 ? status : -1;
    }
    return status;
}

string quotedForShell(const string& text) {
#ifdef _WIN32
    return "\"" + text + "\"";
#else
    string quoted = "'";
    for (char c : text) {
        if (c == '\'') quoted += "'\\''";
        else quoted += c;
    }
    return quoted + "'";
#endif
}

string composeCommand() {
    return "docker compose -f " + quotedForShell(composeFile);
}

// the paths given when the program was started
vector<fs::path> pathArguments(int argc, char* argv[]) {
    vector<fs::path> paths;
#ifdef _WIN32
    // Windows' narrow argv can't hold every file name, the wide command line can
    (void)argc;
    (void)argv;
    int count = 0;
    wchar_t** arguments = CommandLineToArgvW(GetCommandLineW(), &count);
    if (arguments == nullptr) {
        throw runtime_error("could not read the command line");
    }
    for (int i = firstPathArgument; i < count; i++) {
        paths.emplace_back(arguments[i]);
    }
    LocalFree(arguments);
#else
    for (int i = firstPathArgument; i < argc; i++) {
        paths.emplace_back(argv[i]);
    }
#endif
    return paths;
}

Input checkedInput(const fs::path& typed) {
    const string shown = typed.u8string();

    if (shown.find('"') != string::npos) {
        throw runtime_error("this path has a stray quote in it: " + shown + "\n" +
                            "On Windows a quoted path can't end in a backslash, so remove the \\ before the closing quote.");
    }

    error_code error;
    const fs::file_status status = fs::status(typed, error);
    if (status.type() == fs::file_type::not_found) {
        throw runtime_error("this path does not exist: " + shown);
    }
    if (error) {
        throw runtime_error("cannot access this path (" + error.message() + "): " + shown);
    }
    const bool isFile = fs::is_regular_file(status);
    if (!isFile && !fs::is_directory(status)) {
        throw runtime_error("this is not a folder or a video file: " + shown);
    }
    if (isFile && !isVideoFile(typed)) {
        throw runtime_error("this is not a folder or a supported video file: " + shown);
    }

    const fs::path full = fs::canonical(typed, error);
    if (error) {
        throw runtime_error("cannot read this path (" + error.message() + "): " + shown);
    }
    return {full, isFile};
}

bool samePath(const fs::path& a, const fs::path& b) {
    error_code error;
    const bool same = fs::equivalent(a, b, error);
    // one of them has gone away since it was typed, so the text is all that's left to compare
    return error ? a == b : same;
}

// puts a path in line for the next free producer
void submit(const fs::path& typed) {
    if (consumerDown) {
        throw runtime_error("the consumer is not running, so nothing can be uploaded. Type exit and start again.");
    }

    const Input input = checkedInput(typed);
    const string shown = input.path.u8string();
    string message;
    {
        lock_guard<mutex> lock(stateMutex);

        for (const auto& active : activeProducers) {
            if (samePath(active.second.path, input.path)) {
                throw runtime_error("producer-" + to_string(active.first) + " is already uploading " + shown);
            }
        }
        for (const Input& waiting : waitingPaths) {
            if (samePath(waiting.path, input.path)) {
                throw runtime_error("this is already waiting for a producer: " + shown);
            }
        }

        if (waitingPaths.size() >= freeProducers.size()) {
            message = "all " + to_string(valueOf("p")) + " producers are busy, this will start when one is free (" +
                      to_string(waitingPaths.size() - freeProducers.size() + 1) + " waiting): " + shown;
        }
        waitingPaths.push_back(input);
    }
    pathWaiting.notify_one();

    if (!message.empty()) printLine(message);
}

void submitAndReport(const fs::path& typed) {
    try {
        submit(typed);
    } catch (const exception& error) {
        printLine(string("Error: ") + error.what());
    }
}

// the conversion is inside the try because text that isn't valid UTF-8 can't become a path on Windows
void submitTypedLine(const string& text) {
    try {
        submit(fs::u8path(text));
    } catch (const exception& error) {
        printLine(string("Error: ") + error.what());
    }
}

// Single quotes keep backslashes, spaces and '#' literal in YAML. Compose would
// also treat a '$' as the start of a variable unless it is doubled.
string composeString(const string& text) {
    string quoted = "'";
    for (char c : text) {
        quoted += c;
        if (c == '\'' || c == '$') quoted += c;
    }
    return quoted + "'";
}

string composeText(const map<int, Input>& producers) {
    ostringstream yaml;

    yaml << "# Generated by tools/upload_cli from config.txt (p=" << valueOf("p") << ", c=" << valueOf("c")
         << ", q=" << valueOf("q") << ").\n"
         << "# Don't edit this file, it is rewritten as producers start.\n"
         << "name: media-upload\n\n"
         << "services:\n"
         << "  consumer:\n"
         << "    build:\n"
         << "      context: " << composeString(rootFolder) << "\n"
         << "      dockerfile: consumer/backend/Dockerfile\n"
         << "    image: media-upload-consumer\n"
         << "    environment:\n"
         << "      CONSUMER_THREADS: \"" << valueOf("c") << "\"\n"
         << "      QUEUE_CAPACITY: \"" << valueOf("q") << "\"\n"
         << "      GRPC_PORT: \"" << grpcPort << "\"\n"
         << "      HTTP_PORT: \"" << valueOf("http_port") << "\"\n"
         << "      COMPRESS_HEIGHT: \"" << valueOf("compress_height") << "\"\n"
         << "      MAX_UPLOAD_MB: \"" << valueOf("max_upload_mb") << "\"\n"
         << "    ports:\n"
         << "      - \"" << valueOf("http_port") << ":" << valueOf("http_port") << "\"\n"
         << "    volumes:\n"
         << "      - ./storage:/data\n";

    for (const auto& producer : producers) {
        const string name = "producer-" + to_string(producer.first);
        const Input& input = producer.second;

        // a single video is mounted by itself so the files next to it stay out of the container
        const string target = input.isFile ? containerFolder + "/" + input.path.filename().u8string() : containerFolder;

        yaml << "\n  " << name << ":\n"
             << "    image: media-upload-producer\n"
             << "    pull_policy: never\n"
             << "    environment:\n"
             << "      PRODUCER_ID: \"" << name << "\"\n"
             << "      CONSUMER_ADDRESS: \"consumer:" << grpcPort << "\"\n"
             << "      VIDEO_DIR: \"" << containerFolder << "\"\n"
             << "      SCAN_SECONDS: \"" << valueOf("scan_seconds") << "\"\n"
             << "    volumes:\n"
             << "      - type: bind\n"
             << "        source: " << composeString(input.path.u8string()) << "\n"
             << "        target: " << composeString(target) << "\n"
             << "        read_only: true\n";
    }
    return yaml.str();
}

// Written beside the real file and renamed over it, so a full disk can't leave
// a half-written file that stops compose from being able to take the containers down.
void writeCompose(const map<int, Input>& producers) {
    const string partial = composeFile + ".part";
    errno = 0;
    ofstream output(partial, ios::binary);
    if (!output) {
        throw runtime_error("cannot write " + partial + ": " + lastErrorText());
    }
    output << composeText(producers);
    output.close();
    if (!output) {
        const string reason = lastErrorText();
        error_code removeError;
        fs::remove(partial, removeError);
        throw runtime_error("error while writing " + partial + ": " + reason +
                            (removeError ? ", and it could not be removed: " + removeError.message() : ""));
    }

    error_code renameError;
    fs::rename(partial, composeFile, renameError);
    if (renameError) {
        throw runtime_error("cannot replace " + composeFile + ": " + renameError.message());
    }
}

// the container's id, or nothing if docker can't say (docker's own message goes to the terminal)
string containerOf(const string& service) {
    string output;
    if (runCaptured(composeCommand() + " ps -aq " + service, output) != 0) return "";
    return trim(output);
}

// starts one producer container and stays with it until it has finished
void runProducer(int number, const Input& input) {
    const string service = "producer-" + to_string(number);
    string containerId;
    {
        lock_guard<mutex> lock(composeMutex);
        if (stopping) return;

        map<int, Input> producers;
        {
            lock_guard<mutex> state(stateMutex);
            producers = activeProducers;
        }
        writeCompose(producers);

        // always a fresh container, so its log only has this upload in it
        string output;
        if (runCaptured(composeCommand() + " up -d --force-recreate " + service + " 2>&1", output) != 0) {
            printLine(service + " could not start: " + trim(output));
            return;
        }

        containerId = containerOf(service);
        if (containerId.empty()) {
            // Without its id nothing can follow it, and its slot would be handed out while it was still running.
            printLine(service + " started but docker could not say which container it is, so it is being stopped. Type the path again.");
            if (runCaptured(composeCommand() + " rm -fs " + service + " 2>&1", output) != 0) {
                printLine(service + " could not be stopped either: " + trim(output));
            }
            return;
        }
    }
    printLine(service + " started for " + input.path.u8string());

    errno = 0;
    FILE* logs = openPipe("docker logs -f " + containerId + " 2>&1");
    if (logs == nullptr) {
        printLine(service + ": could not follow its log, so its results won't appear here: " + lastErrorText());
    } else {
        char buffer[4096];
        while (fgets(buffer, sizeof(buffer), logs) != nullptr) {
            printLine(trim(buffer));
        }
        const bool readFailed = ferror(logs) != 0;
        const int logStatus = closePipe(logs);
        if ((readFailed || logStatus != 0) && !stopping) {
            printLine(service + ": its log ended early, so some of its results may be missing here");
        }
    }

    if (stopping) return;

    // waits rather than just looking, in case the log ended while the container was still running
    string exitCode;
    if (runCaptured("docker wait " + containerId + " 2>&1", exitCode) != 0) {
        printLine(service + ": could not get its result: " + trim(exitCode));
    } else if (trim(exitCode) != "0") {
        printLine(service + " ended with exit code " + trim(exitCode) + ", not everything it was given was uploaded. See its messages above.");
    }
}

// There is one of these per allowed producer, so a path waits exactly until one is free.
void producerSlot() {
    // an exception leaving a thread would end the whole program without taking the containers down
    try {
        while (true) {
            Input input;
            int number = 0;
            {
                unique_lock<mutex> lock(stateMutex);
                pathWaiting.wait(lock, [] { return stopping || !waitingPaths.empty(); });
                if (stopping) return;

                input = waitingPaths.front();
                waitingPaths.pop_front();
                number = *freeProducers.begin();
                freeProducers.erase(freeProducers.begin());
                activeProducers[number] = input;
            }

            try {
                runProducer(number, input);
            } catch (const exception& error) {
                printLine(string("Error: ") + error.what());
            }

            {
                lock_guard<mutex> lock(stateMutex);
                activeProducers.erase(number);
                freeProducers.insert(number);
            }
        }
    } catch (const exception& error) {
        printLine(string("A producer slot has stopped, one less can run at once: ") + error.what());
    }
}

// finds the common problems up front, so they get a clear message instead of a failed build
void checkDocker() {
    string output;
    if (runCaptured("docker info --format \"{{.ServerVersion}}\" 2>&1", output) != 0) {
        throw runtime_error("Docker is not available. On Windows, Docker Desktop must be running.\n" + trim(output));
    }
    if (runCaptured("docker compose version 2>&1", output) != 0) {
        throw runtime_error("Docker Compose v2 is needed, but 'docker compose' failed.\n" + trim(output));
    }
}

void runDocker(const string& command, const string& problem) {
    errno = 0;
    const int result = system(command.c_str());
    if (stopping) {
        throw runtime_error("stopped while starting the service");
    }
    if (result == -1) {
        throw runtime_error(problem + ": the command could not be run: " + lastErrorText());
    }
    if (result != 0) {
        throw runtime_error(problem + ". Docker's message is above.");
    }
}

// up -d only says the container started. This waits for the program inside it
// to be listening, and shows why if it stops instead. Returns the container's id.
string waitUntilConsumerReady() {
    const auto giveUpAt = chrono::steady_clock::now() + chrono::seconds(consumerStartSeconds);
    while (true) {
        if (stopping) {
            throw runtime_error("stopped while waiting for the consumer to start");
        }

        const string id = containerOf("consumer");
        string logs;
        const bool haveLogs = runCaptured(composeCommand() + " logs --no-log-prefix --no-color consumer 2>&1", logs) == 0;
        if (haveLogs && !id.empty() && logs.find(consumerReadyText) != string::npos) {
            return id;
        }

        string state;
        if (!id.empty() && runCaptured("docker inspect -f \"{{.State.Status}}\" " + id + " 2>&1", state) == 0) {
            state = trim(state);
            if (state == "exited" || state == "dead") {
                throw runtime_error("the consumer stopped while starting:\n" + trim(logs));
            }
        }

        if (chrono::steady_clock::now() >= giveUpAt) {
            throw runtime_error("the consumer was not ready after " + to_string(consumerStartSeconds) + " seconds:\n" + trim(logs));
        }
        this_thread::sleep_for(chrono::milliseconds(500));
    }
}

// The consumer runs in the background, so nothing else would notice it stopping.
// Detached because docker wait can only end when the container does.
void watchConsumer(const string& containerId) {
    try {
        string output;
        const int status = runCaptured("docker wait " + containerId + " 2>&1", output);
        if (stopping) return;

        if (status != 0) {
            printLine("Warning: the consumer can no longer be watched, so if it stops nothing will say so here: " + trim(output));
            return;
        }

        consumerDown = true;
        string logs;
        {
            lock_guard<mutex> lock(composeMutex);
            if (runCaptured(composeCommand() + " logs --no-log-prefix --no-color --tail 20 consumer 2>&1", logs) != 0) {
                logs = "(its log could not be read: " + trim(logs) + ")";
            }
        }
        printLine("The consumer stopped unexpectedly (exit code " + trim(output) + "). Its last messages:\n" + trim(logs) +
                  "\nType exit and start again.");
    } catch (const exception& error) {
        printLine(string("Warning: stopped watching the consumer: ") + error.what());
    }
}

void startService() {
    checkDocker();
    containersMayExist = true;

    fs::create_directories(fs::path(rootFolder) / "storage");
    writeCompose({});

    cout << "Starting the service, the first build takes several minutes.\n" << flush;

    runDocker("docker build -t media-upload-producer -f " + quotedForShell(rootFolder + "/producer/backend/Dockerfile") + " " +
                  quotedForShell(rootFolder),
              "could not build the producer image");
    runDocker(composeCommand() + " up -d --build --remove-orphans consumer", "could not start the consumer");

    thread(watchConsumer, waitUntilConsumerReady()).detach();
}

// takes the spaces and one pair of surrounding quotes off a typed line
string cleanedLine(const string& line) {
    string text = trim(line);
    if (text.size() >= 2 && (text.front() == '"' || text.front() == '\'') && text.back() == text.front()) {
        text = trim(text.substr(1, text.size() - 2));
    }
    return text;
}

bool isExitCommand(string text) {
    transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(tolower(c)); });
    return text == "exit" || text == "quit";
}

void stopService() {
    stopping = true;
    pathWaiting.notify_all();

    size_t abandoned = 0;
    {
        lock_guard<mutex> lock(stateMutex);
        abandoned = waitingPaths.size();
    }
    printLine("Stopping the service...");
    if (abandoned > 0) {
        printLine(to_string(abandoned) + " path(s) still waiting were not uploaded");
    }

    lock_guard<mutex> lock(composeMutex);
    string output;

    // The consumer lists what it couldn't finish as it stops, and that log goes when the container is removed.
    if (runCaptured(composeCommand() + " stop --timeout 15 2>&1", output) != 0) {
        printLine("Could not stop the containers: " + trim(output));
    } else if (runCaptured(composeCommand() + " logs --no-log-prefix --no-color consumer 2>&1", output) != 0) {
        printLine("Could not read the consumer's log, so any videos it didn't finish can't be listed: " + trim(output));
    } else {
        istringstream lines(output);
        string line;
        while (getline(lines, line)) {
            if (line.find(abandonedText) != string::npos) printLine(trim(line));
        }
    }

    if (runCaptured(composeCommand() + " down --remove-orphans --timeout 15 2>&1", output) != 0) {
        printLine("Could not remove the containers: " + trim(output));
    }
}

int main(int argc, char* argv[]) {
    if (argc < firstPathArgument) {
        cerr << "usage: upload_cli <config file> <compose file> [video or folder] ...\n";
        return 1;
    }

    setUpConsole();
    vector<thread> slots;
    int exitCode = 0;

    try {
        loadConfig(argv[1]);
        const fs::path composePath = fs::absolute(argv[2]);
        composeFile = composePath.string();
        rootFolder = composePath.parent_path().string();

        const uint64_t maxProducers = valueOf("p");
        for (uint64_t i = 1; i <= maxProducers; i++) {
            freeProducers.insert(static_cast<int>(i));
        }

        startService();

        for (uint64_t i = 0; i < maxProducers; i++) {
            try {
                slots.emplace_back(producerSlot);
            } catch (const system_error& error) {
                throw runtime_error("could not start producer slot " + to_string(i + 1) + " of " + to_string(maxProducers) + ": " + error.what());
            }
        }

        cout << "\nThe service is running. GUI: http://localhost:" << valueOf("http_port") << "\n"
             << "Type a video or folder path and press Enter, one per line. Up to " << maxProducers
             << " producer(s) run at once.\n"
             << "Type exit to stop.\n\n"
             << flush;

        for (const fs::path& typed : pathArguments(argc, argv)) {
            submitAndReport(typed);
        }

        showPrompt();
        string line;
        while (!stopping && readLine(line)) {
            {
                lock_guard<mutex> lock(consoleMutex);
                promptShowing = false;
            }

            const string text = cleanedLine(line);
            if (isExitCommand(text)) break;
            if (!text.empty()) submitTypedLine(text);
            showPrompt();
        }

        stopService();
    } catch (const exception& error) {
        stopping = true;
        pathWaiting.notify_all();
        cerr << "Error: " << error.what() << "\n";
        exitCode = 1;

        // anything already started would otherwise keep running with nothing left to stop it
        if (containersMayExist) {
            try {
                stopService();
            } catch (const exception& stopError) {
                cerr << "Could not stop the containers, run 'docker compose -f docker-compose.yml down' by hand: " << stopError.what() << "\n";
            }
        }
    }

    for (thread& slot : slots) {
        slot.join();
    }

    // cout doesn't throw when a write fails, so check it here
    cout.flush();
    if (!cout) {
        cerr << "Error: could not write all of the output\n";
        exitCode = 1;
    }
    return exitCode;
}
