// The command line for the media upload service. It starts the consumer and p
// producer containers, and each producer watches its own subfolder of the main
// folder (videos/producer-1 ... videos/producer-p). A scheduler thread here watches
// the rest of the main folder and moves each new video into the subfolder of the
// producer with the fewest videos still waiting. Nothing is typed here: the window
// only shows what happens, and Ctrl+C stops the service, removes the containers
// and clears the producers' folders.
//
// usage: upload_cli <config file> <compose file>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <signal.h>
#endif

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
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
const string mainFolderName = "videos";     // the main folder, next to config.txt
const string producerPrefix = "producer-";  // producer-1 ... producer-p, as services and as folders
const string resultText = "result: ";       // a producer's line for each video it is done with, see producer/backend/src/main.cpp
const int deepestFolder = 32;               // folders in the main folder deeper than this are left alone

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
    {"p", "producers, each with its own folder", 1, 64, true, 0, 0},
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

string lowercase(string text) {
    transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(tolower(c)); });
    return text;
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

atomic<bool> stopping{false};
atomic<bool> consumerDown{false};  // it stopped by itself, so the scheduler holds new videos back
bool containersMayExist = false;   // Docker works and start-up got far enough to have created something
string composeFile;
fs::path rootFolder;
fs::path mainFolder;

mutex consoleMutex;

// stateMutex can be held while taking consoleMutex, never the other way round.
// spawnMutex is only held while a pipe is set up and nothing else is locked inside it.
mutex spawnMutex;

// What the scheduler knows about one producer.
struct Producer {
    fs::path folder;
    set<string> waiting;  // names handed to it that have no result yet, the one being sent included
    set<string> placed;   // every name handed to it this run, never handed out again (see freeName)
    map<string, string> results;  // each name that had a result this run, and what it was
    bool running = false;
    string stopReason;    // why it stopped, once it has
    string containerId;
    size_t linesHandled = 0;  // lines of its log already shown or counted, see catchUpProducerLogs
};

vector<thread> followers;  // one per producer, following its log; only the main thread starts and joins them

mutex stateMutex;
vector<Producer> producers;          // producers[0] is producer-1, the list never changes size after start-up
atomic<size_t> mainFolderVideos{0};  // videos the last scan found in the main folder

// producer folders past p, left by a run with a larger p that didn't clear up; emptied into the main folder on the way out
vector<fs::path> leftoverFolders;

// the consumer's "abandoned:" lines from when it stopped, read on the main thread only
vector<string> abandonedLines;
bool abandonedKnown = false;  // false if its log couldn't be read, so any upload may have been abandoned

#ifdef _WIN32
BOOL WINAPI onConsoleSignal(DWORD signal) {
    if (signal == CTRL_C_EVENT || signal == CTRL_BREAK_EVENT) {
        stopping = true;
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
    // Ctrl+C is the only way to stop, so without it nothing could clear up
    if (!SetConsoleCtrlHandler(onConsoleSignal, TRUE)) {
        throw runtime_error("could not take over Ctrl+C, so the service could not be stopped cleanly (Windows error " +
                            to_string(GetLastError()) + ")");
    }
#else
    struct sigaction action {};
    action.sa_handler = onStopSignal;
    if (sigaction(SIGINT, &action, nullptr) != 0 || sigaction(SIGTERM, &action, nullptr) != 0) {
        throw runtime_error("could not take over Ctrl+C, so the service could not be stopped cleanly: " + lastErrorText());
    }
    // a closed terminal should show up as failed writes, not end the program mid-cleanup
    if (signal(SIGPIPE, SIG_IGN) == SIG_ERR) {
        cerr << "Warning: if this terminal is closed, the containers may be left running (" << lastErrorText() << ")\n";
    }
#endif
}

// the producers, the scheduler and the main thread all report here, one whole line at a time
void printLine(const string& text) {
    lock_guard<mutex> lock(consoleMutex);
    cout << text << '\n' << flush;
}

// Ctrl+C reaches docker's own processes as well as this one, so a docker command
// that ends at that moment may be the start of a stop rather than a failure. This
// gives the stop a moment to be noticed before anything is called a failure.
bool stopIsComing() {
    for (int i = 0; i < 20 && !stopping; i++) {
        this_thread::sleep_for(chrono::milliseconds(50));
    }
    return stopping;
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

// one line from a pipe without its line ending; false once the output has ended.
// complete is false for a last line that was cut off without its line ending.
bool readPipeLine(FILE* pipe, string& line, bool& complete) {
    line.clear();
    char buffer[4096];
    while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
        line += buffer;
        if (line.back() == '\n') break;
    }
    if (line.empty()) return false;
    complete = line.back() == '\n';
    while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();
    return true;
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

string producerName(size_t number) {
    return producerPrefix + to_string(number);
}

// "producer-" and a number, in any case since Windows folder names ignore it.
// Folders like this are never searched for new videos, even ones past p, because
// what is in them was handed out to a producer before.
bool isProducerFolderName(const string& name) {
    if (name.size() <= producerPrefix.size() || lowercase(name.substr(0, producerPrefix.size())) != producerPrefix) return false;
    return all_of(name.begin() + static_cast<ptrdiff_t>(producerPrefix.size()), name.end(),
                  [](unsigned char c) { return c >= '0' && c <= '9'; });
}

void sleepUnlessStopped(uint64_t seconds) {
    const auto wakeAt = chrono::steady_clock::now() + chrono::seconds(seconds);
    while (!stopping && chrono::steady_clock::now() < wakeAt) {
        this_thread::sleep_for(chrono::milliseconds(100));
    }
}

// a video is only moved once these haven't changed since the previous scan,
// so one that is still being copied in is left alone
struct FileState {
    uintmax_t size = 0;
    fs::file_time_type modified;

    bool operator==(const FileState& other) const {
        return size == other.size && modified == other.modified;
    }
};

// the videos sitting directly in a producer's folder, the same ones the producer itself will send
vector<string> videosIn(const fs::path& folder) {
    vector<string> names;
    error_code error;
    fs::directory_iterator it(folder, error);
    if (error) {
        throw runtime_error("cannot read " + folder.u8string() + ": " + error.message());
    }
    for (const fs::directory_iterator end; !error && it != end; it.increment(error)) {
        // anything that can't be checked is left out by the producer too, which says so in its own log
        error_code typeError;
        const fs::file_status status = it->symlink_status(typeError);
        if (!typeError && fs::is_regular_file(status) && isVideoFile(it->path())) {
            names.push_back(it->path().filename().u8string());
        }
    }
    if (error) {
        throw runtime_error("cannot read all of " + folder.u8string() + ": " + error.message());
    }
    return names;
}

// Makes the main folder and one folder per producer. The producers' folders are
// removed when the service stops, so videos already in one were left by a run that didn't stop
// cleanly. That producer sends them again when it starts, so they count as waiting.
void prepareFolders(size_t count) {
    mainFolder = rootFolder / mainFolderName;
    error_code error;
    fs::create_directories(mainFolder, error);
    if (error) {
        throw runtime_error("cannot create the main folder " + mainFolder.u8string() + ": " + error.message());
    }

    set<string> current;
    for (size_t number = 1; number <= count; number++) {
        Producer producer;
        producer.folder = mainFolder / producerName(number);
        current.insert(producerName(number));
        fs::create_directories(producer.folder, error);
        if (error) {
            throw runtime_error("cannot create " + producer.folder.u8string() + ": " + error.message());
        }

        for (const string& name : videosIn(producer.folder)) {
            producer.waiting.insert(name);
            producer.placed.insert(name);
        }
        if (!producer.waiting.empty()) {
            cout << producerName(number) << "'s folder still has " << producer.waiting.size()
                 << " video(s) from a run that didn't stop cleanly. They are sent again, and any already saved come back as duplicates.\n";
        }
        producers.push_back(producer);
    }

    // folders left from a run with a larger p
    fs::directory_iterator it(mainFolder, error);
    if (error) {
        throw runtime_error("cannot read the main folder " + mainFolder.u8string() + ": " + error.message());
    }
    for (const fs::directory_iterator end; !error && it != end; it.increment(error)) {
        const string name = it->path().filename().u8string();
        if (!isProducerFolderName(name) || current.count(lowercase(name)) > 0) continue;

        error_code typeError;
        const bool isFolder = it->is_directory(typeError);
        if (typeError) {
            cout << "Warning: could not check what " << name << " in the main folder is: " << typeError.message() << "\n";
            continue;
        }
        if (!isFolder) continue;

        leftoverFolders.push_back(it->path());
        try {
            const size_t left = videosIn(it->path()).size();
            if (left > 0) {
                cout << name << " holds " << left << " video(s), but p is " << count
                     << ", so no producer reads that folder. They go back into the main folder when the service stops, to be sent next time.\n";
            }
        } catch (const exception& readError) {
            cout << "Warning: " << readError.what() << "\n";
        }
    }
    if (error) {
        throw runtime_error("cannot read all of the main folder " + mainFolder.u8string() + ": " + error.message());
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

string composeText() {
    ostringstream yaml;

    yaml << "# Generated by tools/upload_cli from config.txt (p=" << valueOf("p") << ", c=" << valueOf("c")
         << ", q=" << valueOf("q") << ").\n"
         << "# Don't edit this file, it is written again every time the service starts.\n"
         << "name: media-upload\n\n"
         << "services:\n"
         << "  consumer:\n"
         << "    build:\n"
         << "      context: " << composeString(rootFolder.u8string()) << "\n"
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

    // each producer only sees its own folder, and only reads it
    for (size_t number = 1; number <= producers.size(); number++) {
        const string name = producerName(number);
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
             << "        source: " << composeString(producers[number - 1].folder.u8string()) << "\n"
             << "        target: " << composeString(containerFolder) << "\n"
             << "        read_only: true\n";
    }
    return yaml.str();
}

// Written beside the real file and renamed over it, so a full disk can't leave
// a half-written file that stops compose from being able to take the containers down.
void writeCompose() {
    const string partial = composeFile + ".part";
    errno = 0;
    ofstream output(partial, ios::binary);
    if (!output) {
        throw runtime_error("cannot write " + partial + ": " + lastErrorText());
    }
    output << composeText();
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
    if (stopping || (result != 0 && stopIsComing())) {
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
        if (stopIsComing()) return;

        if (status != 0) {
            printLine("Warning: the consumer can no longer be watched, so if it stops nothing will say so here: " + trim(output));
            return;
        }

        consumerDown = true;
        string logs;
        if (runCaptured(composeCommand() + " logs --no-log-prefix --no-color --tail 20 consumer 2>&1", logs) != 0) {
            logs = "(its log could not be read: " + trim(logs) + ")";
        }
        printLine("The consumer stopped unexpectedly (exit code " + trim(output) + "). Its last messages:\n" + trim(logs) +
                  "\nNew videos stay in the main folder. Press Ctrl+C and start again.");
    } catch (const exception& error) {
        printLine(string("Warning: stopped watching the consumer: ") + error.what());
    }
}

void startService() {
    checkDocker();
    containersMayExist = true;

    fs::create_directories(rootFolder / "storage");
    writeCompose();

    cout << "Starting the service, the first build takes several minutes.\n" << flush;

    runDocker("docker build -t media-upload-producer -f " + quotedForShell((rootFolder / "producer" / "backend" / "Dockerfile").string()) +
                  " " + quotedForShell(rootFolder.string()),
              "could not build the producer image");
    runDocker(composeCommand() + " up -d --build --remove-orphans consumer", "could not start the consumer");
    thread(watchConsumer, waitUntilConsumerReady()).detach();

    // always fresh containers, so their logs only have this run in them
    string names;
    for (size_t number = 1; number <= producers.size(); number++) {
        names += " " + producerName(number);
    }
    runDocker(composeCommand() + " up -d --force-recreate" + names, "could not start the producers");
}

// A producer prints "[producer-N] result: <OUTCOME> <file name>" once for every
// video it is done with. Returns false for any other line.
bool takeResult(size_t index, const string& line) {
    const string name = producerName(index + 1);
    const string prefix = "[" + name + "] " + resultText;
    if (line.compare(0, prefix.size(), prefix) != 0) return false;

    static const set<string> outcomes = {"UPLOADED", "DROPPED", "DUPLICATE", "REJECTED", "FAILED"};
    const string rest = line.substr(prefix.size());
    const size_t space = rest.find(' ');
    if (space == string::npos || space + 1 == rest.size() || outcomes.count(rest.substr(0, space)) == 0) {
        printLine(name + " printed a result the scheduler doesn't understand, so its count of waiting videos may be off: " + line);
        return true;
    }

    const string file = rest.substr(space + 1);
    lock_guard<mutex> lock(stateMutex);
    producers[index].results[file] = rest.substr(0, space);
    // A name that isn't being counted was put in the folder by hand, or was
    // already found to be gone from it, so there is nothing to take off.
    producers[index].waiting.erase(file);
    return true;
}

void stopProducer(size_t index, const string& reason) {
    size_t left = 0;
    {
        lock_guard<mutex> lock(stateMutex);
        producers[index].running = false;
        producers[index].stopReason = reason;
        left = producers[index].waiting.size();
    }
    const string waiting =
        left == 0 ? "" : ", and the " + to_string(left) + " still waiting in its folder go back into the main folder when the service stops";
    printLine(producerName(index + 1) + " has stopped: " + reason + ". The scheduler gives it no more videos" + waiting + ".");
}

// Shows one line of a producer's log, or passes it to the scheduler if it is a result.
// A result line repeats what the producer has just said in words, so it isn't shown.
void handleProducerLine(size_t index, const string& line) {
    if (!takeResult(index, line)) printLine(trim(line));
    lock_guard<mutex> lock(stateMutex);
    producers[index].linesHandled++;
}

// Shows a producer's messages in the window and passes its result lines to the
// scheduler. Producers run until the service stops, so a log that ends before
// then means the producer stopped by itself.
void followProducer(size_t index, const string containerId) {
    string reason;
    try {
        errno = 0;
        FILE* logs = openPipe("docker logs -f " + containerId + " 2>&1");
        if (logs == nullptr) {
            reason = "its log could not be followed, so the scheduler can't tell when its videos are done: " + lastErrorText();
        } else {
            string line;
            bool complete = true;
            while (readPipeLine(logs, line, complete)) {
                // a line cut off by Ctrl+C is read whole after the stop, by catchUpProducerLogs
                if (!complete && stopIsComing()) break;
                handleProducerLine(index, line);
            }
            const bool readFailed = ferror(logs) != 0;
            const int logStatus = closePipe(logs);
            if (stopIsComing()) return;

            if (readFailed) {
                reason = "not all of its log could be read, so the scheduler can't tell when its videos are done";
            } else if (logStatus != 0) {
                reason = "following its log failed (status " + to_string(logStatus) + "), so the scheduler can't tell when its videos are done";
            } else {
                // a log that ended cleanly means the container has stopped, so this returns straight away
                string exitCode;
                if (runCaptured("docker wait " + containerId + " 2>&1", exitCode) != 0) {
                    reason = "it ended, and its exit code could not be read: " + trim(exitCode);
                } else {
                    reason = "it ended with exit code " + trim(exitCode) + ", see its messages above";
                }
            }
        }
    } catch (const exception& error) {
        reason = string("following its log failed: ") + error.what();
    }
    if (!stopping) stopProducer(index, reason);
}

void followProducers() {
    for (size_t index = 0; index < producers.size(); index++) {
        const string name = producerName(index + 1);
        const string id = containerOf(name);
        if (id.empty()) {
            throw runtime_error(name + " started but docker could not say which container it is");
        }
        {
            lock_guard<mutex> lock(stateMutex);
            producers[index].running = true;
            producers[index].containerId = id;
        }
        try {
            followers.emplace_back(followProducer, index, id);
        } catch (const system_error& error) {
            throw runtime_error("could not start following " + name + ": " + error.what());
        }
    }
}

// Ctrl+C stops the "docker logs" processes following the producers as well, so
// what a producer printed after that (its last results and totals) is read here
// from its whole log, once its container has stopped and the followers have ended.
void catchUpProducerLogs() {
    for (thread& follower : followers) {
        if (follower.joinable()) follower.join();
    }

    for (size_t index = 0; index < producers.size(); index++) {
        const string id = producers[index].containerId;
        if (id.empty()) continue;  // never started, so it printed nothing

        string output;
        if (runCaptured("docker logs " + id + " 2>&1", output) != 0) {
            printLine("Could not read the end of " + producerName(index + 1) + "'s log, so its last results are missing and its unsent videos "
                      "count as not sent: " + trim(output));
            continue;
        }
        istringstream lines(output);
        string line;
        size_t number = 0;
        while (getline(lines, line)) {
            if (++number <= producers[index].linesHandled) continue;
            while (!line.empty() && line.back() == '\r') line.pop_back();
            handleProducerLine(index, line);
        }
    }
}

// how a path in the main folder is shown, e.g. videos\trip\clip.mp4
string shownPath(const fs::path& path) {
    const fs::path relative = path.lexically_relative(mainFolder);
    if (relative.empty() || relative == ".") return mainFolderName;
    return (fs::path(mainFolderName) / relative).u8string();
}

// a producer reports each video on one line, so a name can't hold a line break or similar
bool hasControlCharacter(const string& name) {
    return any_of(name.begin(), name.end(), [](unsigned char c) { return c < 0x20 || c == 0x7f; });
}

// Every video in the main folder and the folders inside it, except the producers' folders.
// What can't be used goes in problems, keyed by how its path is shown.
void scanFolder(const fs::path& folder, int depth, map<fs::path, FileState>& videos, map<string, string>& problems) {
    error_code error;
    fs::directory_iterator it(folder, error);
    if (error) {
        problems[shownPath(folder)] = "could not open this folder, will try again: " + error.message();
        return;
    }

    for (const fs::directory_iterator end; !error && it != end; it.increment(error)) {
        const fs::path path = it->path();
        const string shown = shownPath(path);

        error_code typeError;
        const fs::file_status status = it->symlink_status(typeError);
        if (typeError) {
            problems[shown] = "left where it is, could not check what it is: " + typeError.message();
        } else if (fs::is_symlink(status)) {
            problems[shown] = "left where it is, it is a link. Put the video itself here instead";
        } else if (fs::is_directory(status)) {
            if (depth == 0 && isProducerFolderName(path.filename().u8string())) continue;
            if (depth + 1 >= deepestFolder) {
                problems[shown] = "left where it is, it is more than " + to_string(deepestFolder) + " folders deep";
            } else {
                scanFolder(path, depth + 1, videos, problems);
            }
        } else if (!fs::is_regular_file(status)) {
            problems[shown] = "left where it is, it is not a regular file";
        } else if (!isVideoFile(path)) {
            problems[shown] = "left where it is, not one of the supported video types";
        } else if (hasControlCharacter(path.filename().u8string())) {
            problems[shown] = "left where it is, its name has a control character in it. Rename it to send it";
        } else {
            error_code sizeError, timeError;
            FileState state;
            state.size = fs::file_size(path, sizeError);
            state.modified = fs::last_write_time(path, timeError);
            if (sizeError || timeError) {
                problems[shown] = "could not check the file, will try again: " + (sizeError ? sizeError : timeError).message();
            } else if (state.size == 0) {
                // a copy that hasn't written anything yet looks like this too, so it isn't handed out
                problems[shown] = "left where it is for now, the file is empty";
            } else {
                videos[path] = state;
            }
        }
    }
    if (error) {
        problems[shownPath(folder)] = "could not read all of this folder, will try again: " + error.message();
    }
}

// A name in folder that nothing there has and that isn't in used, adding " (2)"
// and so on if it must. Nothing is ever replaced. An empty result means the folder
// couldn't be checked, with the reason in problem.
string freeName(const fs::path& folder, const set<string>& used, const fs::path& original, string& problem) {
    const string stem = original.stem().u8string();
    const string extension = original.extension().u8string();
    for (int copy = 1;; copy++) {
        const string name = copy == 1 ? stem + extension : stem + " (" + to_string(copy) + ")" + extension;
        if (used.count(name) > 0) continue;

        error_code error;
        const bool taken = fs::exists(folder / fs::u8path(name), error);
        if (error) {
            problem = "could not check whether " + name + " is already there: " + error.message();
            return "";
        }
        if (!taken) return name;
    }
}

// Windows refuses to move or delete a file another program has open, and MinGW's text for that is unclear
string fileErrorText(const error_code& error) {
    return error == errc::device_or_resource_busy ? "another program has it open" : error.message();
}

// A video taken out of a producer's folder before the producer saw it would
// never get a result line, and would be counted as waiting for ever.
void forgetGoneVideos(map<string, string>& problems) {
    vector<string> gone;
    {
        lock_guard<mutex> lock(stateMutex);
        for (size_t index = 0; index < producers.size(); index++) {
            Producer& producer = producers[index];
            for (auto it = producer.waiting.begin(); it != producer.waiting.end();) {
                const fs::path path = producer.folder / fs::u8path(*it);
                error_code error;
                const bool there = fs::exists(path, error);
                if (error) {
                    problems[shownPath(path)] = "could not check whether it is still there: " + error.message();
                    ++it;
                } else if (there) {
                    ++it;
                } else {
                    gone.push_back(shownPath(path));
                    it = producer.waiting.erase(it);
                }
            }
        }
    }
    for (const string& path : gone) {
        printLine("scheduler: " + path + " was taken out of its producer's folder before it had a result, so it no longer counts as waiting");
    }
}

struct SchedulerMemory {
    map<fs::path, FileState> lastSeen;  // videos in the main folder at the previous scan
    map<string, string> reported;       // what was last said about each path, so each problem is said once
    string pauseReported;               // why videos are being held back, once that has been said
};

void scheduleOnce(SchedulerMemory& memory) {
    map<string, string> problems;
    forgetGoneVideos(problems);

    map<fs::path, FileState> videos;
    scanFolder(mainFolder, 0, videos, problems);
    mainFolderVideos = videos.size();

    // only a video that hasn't changed since the previous scan has finished being copied in
    vector<fs::path> ready;
    for (const auto& video : videos) {
        const auto seen = memory.lastSeen.find(video.first);
        if (seen != memory.lastSeen.end() && seen->second == video.second) ready.push_back(video.first);
    }
    memory.lastSeen = videos;

    string pause;
    if (consumerDown) {
        pause = "the consumer is not running, so new videos stay in the main folder. Press Ctrl+C and start again.";
    }

    for (const fs::path& path : ready) {
        if (stopping || !pause.empty()) break;
        const string shown = shownPath(path);

        try {
            size_t index = 0;
            fs::path folder;
            string name;
            string problem;
            size_t waitingThere = 0;
            {
                lock_guard<mutex> lock(stateMutex);
                // the producer with the fewest videos waiting, the lowest number if several are tied
                bool found = false;
                for (size_t i = 0; i < producers.size(); i++) {
                    if (!producers[i].running) continue;
                    if (!found || producers[i].waiting.size() < producers[index].waiting.size()) {
                        index = i;
                        found = true;
                    }
                }
                if (!found) {
                    pause = "no producer is running, so new videos stay in the main folder. Press Ctrl+C and start again.";
                    break;
                }

                // The producer could take a file put under a name it has already finished, with the
                // same size and time, for the one it finished. So a name is never handed out twice.
                folder = producers[index].folder;
                name = freeName(folder, producers[index].placed, path.filename(), problem);
                if (!name.empty()) {
                    // counted before the move, so a result can never arrive for a name that isn't counted yet
                    producers[index].waiting.insert(name);
                    producers[index].placed.insert(name);
                    waitingThere = producers[index].waiting.size();
                }
            }
            if (name.empty()) {
                problems[shown] = "can't be handed to " + producerName(index + 1) + " yet, will try again: " + problem;
                continue;
            }

            // the same folder tree, so this is a rename and the producer never sees half a file
            error_code moveError;
            fs::rename(path, folder / fs::u8path(name), moveError);
            if (moveError) {
                {
                    lock_guard<mutex> lock(stateMutex);
                    producers[index].waiting.erase(name);
                    producers[index].placed.erase(name);
                }
                problems[shown] = "can't be moved to " + producerName(index + 1) + " yet, will try again: " + fileErrorText(moveError);
                continue;
            }

            memory.lastSeen.erase(path);
            mainFolderVideos--;
            const string renamed = name != path.filename().u8string() ? " as " + name : "";
            printLine("scheduler: " + shown + " -> " + producerName(index + 1) + renamed + " (" + to_string(waitingThere) + " waiting there)");
        } catch (const exception& error) {
            problems[shown] = string("could not be handed out, will try again: ") + error.what();
        }
    }

    if (pause.empty() || ready.empty()) {
        memory.pauseReported.clear();
    } else if (pause != memory.pauseReported) {
        printLine("scheduler: " + pause);
        memory.pauseReported = pause;
    }

    for (const auto& problem : problems) {
        const auto said = memory.reported.find(problem.first);
        if (said == memory.reported.end() || said->second != problem.second) {
            printLine("scheduler: " + problem.first + ": " + problem.second);
        }
    }
    memory.reported = problems;
}

void runScheduler() {
    // an exception leaving a thread would end the whole program without taking the containers down
    try {
        SchedulerMemory memory;
        while (!stopping) {
            scheduleOnce(memory);
            sleepUnlessStopped(valueOf("scan_seconds"));
        }
    } catch (const exception& error) {
        printLine(string("The scheduler has stopped, so new videos in the main folder won't be handed out. Press Ctrl+C and start again: ") +
                  error.what());
    }
}

void stopService() {
    stopping = true;
    printLine("Stopping the service, this takes a few seconds...");

    string output;
    // The consumer lists what it couldn't finish as it stops, and the logs go when the containers are removed.
    if (runCaptured(composeCommand() + " stop --timeout 15 2>&1", output) != 0) {
        // the abandoned list stays unknown, so every upload goes back into the main folder in case it wasn't saved
        printLine("Could not stop the containers: " + trim(output));
    } else {
        // with the containers stopped, every follower's log has ended, so this can wait for them
        catchUpProducerLogs();

        if (runCaptured(composeCommand() + " logs --no-log-prefix --no-color consumer 2>&1", output) != 0) {
            printLine("Could not read the consumer's log, so any videos it didn't finish can't be listed: " + trim(output));
        } else {
            abandonedKnown = true;
            istringstream lines(output);
            string line;
            while (getline(lines, line)) {
                if (line.find(abandonedText) == string::npos) continue;
                abandonedLines.push_back(trim(line));
                printLine(trim(line));
            }
        }
    }

    if (runCaptured(composeCommand() + " down --remove-orphans --timeout 15 2>&1", output) != 0) {
        printLine("Could not remove the containers: " + trim(output));
    }
}

// Whether the consumer said it stopped before saving this upload. Its line is
// "abandoned: <producer> / <name>, <reason>", with the name cleaned the way
// cleanName in consumer/backend/src/upload_service.cpp does it.
bool wasAbandoned(const string& producer, const string& file) {
    string name;
    for (unsigned char c : file) {
        if (c >= 0x20 && c != 0x7f) name += static_cast<char>(c);
    }
    if (name.size() > 200) name.resize(200);

    const string wanted = abandonedText + " " + producer + " / " + name + ", ";
    return any_of(abandonedLines.begin(), abandonedLines.end(), [&](const string& line) { return line.find(wanted) != string::npos; });
}

// false if it had to stay where it is, which has been said
bool moveBack(const fs::path& path, const string& why) {
    string problem;
    const string name = freeName(mainFolder, {}, path.filename(), problem);
    if (name.empty()) {
        printLine("Could not move " + shownPath(path) + " back into the main folder, so it stays where it is: " + problem);
        return false;
    }

    error_code error;
    fs::rename(path, mainFolder / fs::u8path(name), error);
    if (error) {
        printLine("Could not move " + shownPath(path) + " back into the main folder, so it stays where it is: " + fileErrorText(error));
        return false;
    }
    printLine(shownPath(path) + " -> " + shownPath(mainFolder / fs::u8path(name)) + ", " + why);
    return true;
}

// Empties one producer's folder and removes it. Only videos that are done with are
// deleted. Everything else goes back into the main folder: a video with no result
// in this run, one the consumer accepted but didn't save, and anything that isn't
// a video the producer would send. A folder that can't be emptied is left in place.
void clearFolder(const string& producer, const fs::path& folder, const map<string, string>& results, size_t& deleted, size_t& movedBack) {
    vector<fs::path> entries;
    error_code error;
    fs::directory_iterator it(folder, error);
    if (error == errc::no_such_file_or_directory) return;  // someone removed it already, so there is nothing to clear
    if (error) {
        printLine("Could not open " + shownPath(folder) + ", so it is left in place: " + error.message());
        return;
    }
    for (const fs::directory_iterator end; !error && it != end; it.increment(error)) {
        entries.push_back(it->path());
    }
    if (error) {
        printLine("Could not read all of " + shownPath(folder) + ", so it is left in place: " + error.message());
        return;
    }

    bool emptied = true;
    for (const fs::path& path : entries) {
        const auto result = results.find(path.filename().u8string());
        error_code typeError;
        const fs::file_status status = fs::symlink_status(path, typeError);

        string why;  // why it goes back rather than being deleted
        if (typeError) {
            why = "could not check what it is: " + typeError.message();
        } else if (!fs::is_regular_file(status) || !isVideoFile(path)) {
            why = "it isn't a video the producer sends";
        } else if (result == results.end()) {
            why = "it had no result yet";
        } else if (result->second == "UPLOADED" && !abandonedKnown) {
            why = "the consumer's log couldn't be read to check that it was saved";
        } else if (result->second == "UPLOADED" && wasAbandoned(producer, path.filename().u8string())) {
            why = "the consumer stopped before saving it";
        }

        if (!why.empty()) {
            if (moveBack(path, why)) {
                movedBack++;
            } else {
                emptied = false;
            }
            continue;
        }

        error_code removeError;
        fs::remove(path, removeError);
        if (removeError) {
            printLine("Could not delete " + shownPath(path) + ": " + fileErrorText(removeError));
            emptied = false;
        } else {
            deleted++;
        }
    }

    if (!emptied) {
        printLine(shownPath(folder) + " is left in place, because not everything in it could be cleared");
        return;
    }
    error_code removeError;
    fs::remove(folder, removeError);
    if (removeError) {
        printLine("Could not remove " + shownPath(folder) + ": " + fileErrorText(removeError));
    }
}

// On exit the producers' folders are removed, and any left from a run with a larger p.
// Runs after the threads have ended, so every result line has been counted.
void cleanUpFolders() {
    if (producers.empty()) return;  // start-up ended before any folder was made
    size_t deleted = 0;
    size_t movedBack = 0;

    for (size_t index = 0; index < producers.size(); index++) {
        clearFolder(producerName(index + 1), producers[index].folder, producers[index].results, deleted, movedBack);
    }
    // nothing in these was sent in this run, so all of it goes back
    for (const fs::path& folder : leftoverFolders) {
        clearFolder(folder.filename().u8string(), folder, {}, deleted, movedBack);
    }

    printLine("Cleared the producers' folders: " + to_string(deleted) + " video(s) that had a result were deleted, " + to_string(movedBack) +
              " moved back into the main folder.");
    const size_t left = mainFolderVideos + movedBack;
    if (left > 0) {
        printLine(to_string(left) + " video(s) stay in " + mainFolder.u8string() + " for next time");
    }
}

int main(int argc, char* argv[]) {
    if (argc != 3) {
        cerr << "usage: upload_cli <config file> <compose file>\n";
        if (argc > 3) {
            cerr << "Paths are no longer given here. Put videos, or folders of videos, in the " << mainFolderName
                 << " folder next to config.txt instead.\n";
        }
        return 1;
    }

    thread scheduler;
    int exitCode = 0;

    try {
        setUpConsole();
        loadConfig(argv[1]);
        const fs::path composePath = fs::absolute(argv[2]);
        composeFile = composePath.string();
        rootFolder = composePath.parent_path();

        prepareFolders(valueOf("p"));
        startService();
        followProducers();
        try {
            scheduler = thread(runScheduler);
        } catch (const system_error& error) {
            throw runtime_error(string("could not start the scheduler: ") + error.what());
        }

        printLine("");
        printLine("The service is running. GUI: http://localhost:" + to_string(valueOf("http_port")));
        printLine("Put videos, or folders of videos, in " + mainFolder.u8string());
        printLine("The scheduler moves each one into the folder of the producer with the fewest videos waiting, producer-1 to producer-" +
                  to_string(valueOf("p")) + ".");
        printLine("Press Ctrl+C once to stop. The containers are removed and the producers' folders cleared.");
        printLine("");

        // the other threads do the work, so this one only waits for Ctrl+C
        while (!stopping) {
            this_thread::sleep_for(chrono::milliseconds(100));
        }
        stopService();
    } catch (const exception& error) {
        stopping = true;
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

    if (scheduler.joinable()) scheduler.join();
    // stopService has already waited for these unless the containers couldn't be stopped
    for (thread& follower : followers) {
        if (follower.joinable()) follower.join();
    }

    // after the threads, so every result line the producers printed has been counted
    try {
        cleanUpFolders();
    } catch (const exception& error) {
        cerr << "Error: could not finish clearing the producers' folders: " << error.what() << "\n";
        exitCode = 1;
    }

    // cout doesn't throw when a write fails, so check it here
    cout.flush();
    if (!cout) {
        cerr << "Error: could not write all of the output\n";
        exitCode = 1;
    }
    return exitCode;
}
