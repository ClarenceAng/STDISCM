// One producer instance: watches its own folder for as long as the service runs
// and uploads each video that appears there to the consumer over gRPC, one at a
// time. The scheduler in tools/upload_cli is what puts the videos there.
//
// Videos are left in the folder after their result. Each result is also printed
// as a "result:" line, which is how the scheduler knows how many are still waiting.

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <memory>
#include <new>
#include <set>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#include <grpcpp/grpcpp.h>

#include "error_text.h"
#include "media_upload.grpc.pb.h"
#include "sha256.h"
#include "video_files.h"

using namespace std;
using namespace mediaupload;
namespace fs = std::filesystem;

const size_t chunkSize = 1024 * 1024;
const int maxAttempts = 5;               // tries per video, and in a row for the folder, before giving up on it
const int consumerReminderSeconds = 60;  // how often a long wait for the consumer is mentioned again
const int keepalivePingMs = 20000;
const int keepaliveTimeoutMs = 10000;

// A transfer that gets slower than this is treated as stuck, otherwise it could
// hold its queue slot for ever. The fixed part covers small files and the set-up.
const int uploadBaseSeconds = 120;
const uint64_t slowestBytesPerSecond = 256 * 1024;

atomic<bool> stopping{false};
string producerId;

void onStopSignal(int) {
    stopping = true;
}

void log(const string& message) {
    cout << ("[" + producerId + "] " + message + "\n") << flush;
}

string envOr(const char* name, const string& fallback) {
    const char* value = getenv(name);
    return value != nullptr && *value != '\0' ? value : fallback;
}

int envPositiveNumber(const char* name, const string& fallback) {
    const string text = envOr(name, fallback);
    const char* const end = text.data() + text.size();
    int number = 0;
    const from_chars_result parsed = from_chars(text.data(), end, number);
    if (parsed.ec != errc{} || parsed.ptr != end || number < 1) {
        throw runtime_error(string(name) + " must be a whole number of at least 1, not '" + text + "'");
    }
    return number;
}

// a scan only uploads a file once these haven't changed since the previous
// scan, so a video that is still being copied in is left alone
struct FileState {
    uintmax_t size = 0;
    fs::file_time_type modified;

    bool operator==(const FileState& other) const {
        return size == other.size && modified == other.modified;
    }
};

// RetryLater can be tried again. Everything else is the file's final result, so
// a video dropped for a full queue is not sent again. Failed means this producer
// could not get an answer about it at all.
enum class Outcome { Uploaded, Dropped, Duplicate, Rejected, Failed, RetryLater };

struct Totals {
    int uploaded = 0;
    int dropped = 0;
    int duplicates = 0;
    int rejected = 0;
    int failed = 0;

    string text() const {
        return "totals: " + to_string(uploaded) + " uploaded, " + to_string(dropped) + " dropped, " + to_string(duplicates) +
               " duplicate, " + to_string(rejected) + " rejected, " + to_string(failed) + " failed";
    }
};

// The scheduler in tools/upload_cli reads these lines to know how many videos are
// still waiting in this folder, so their form must stay "result: <OUTCOME> <file name>".
void reportResult(Totals& totals, Outcome outcome, const string& name) {
    string word;
    switch (outcome) {
        case Outcome::Uploaded:
            totals.uploaded++;
            word = "UPLOADED";
            break;
        case Outcome::Dropped:
            totals.dropped++;
            word = "DROPPED";
            break;
        case Outcome::Duplicate:
            totals.duplicates++;
            word = "DUPLICATE";
            break;
        case Outcome::Rejected:
            totals.rejected++;
            word = "REJECTED";
            break;
        case Outcome::Failed:
            totals.failed++;
            word = "FAILED";
            break;
        case Outcome::RetryLater:
            throw logic_error(name + " was reported as finished while it still had tries left");
    }
    log("result: " + word + " " + name);
}

// Sets size to the number of bytes actually hashed, which is what gets sent.
// Returns what went wrong, or an empty string if it worked.
string hashFile(const fs::path& path, string& hex, uint64_t& size) {
    errno = 0;
    ifstream file(path, ios::binary);
    if (!file) return "could not open the file: " + lastErrorText();

    Sha256 hash;
    vector<char> buffer(chunkSize);
    size = 0;
    while (!stopping) {
        errno = 0;
        file.read(buffer.data(), static_cast<streamsize>(buffer.size()));
        const streamsize got = file.gcount();
        if (got > 0) {
            hash.update(buffer.data(), static_cast<size_t>(got));
            size += static_cast<uint64_t>(got);
        }
        if (file.bad()) return "could not read the file: " + lastErrorText();
        if (!file) break;
    }
    if (stopping) return "stopped while reading the file";
    hex = hash.finishHex();
    return "";
}

// retrying can't help with these: they mean this request or this server is wrong
bool canRetry(const grpc::Status& status) {
    switch (status.error_code()) {
        case grpc::StatusCode::INVALID_ARGUMENT:
        case grpc::StatusCode::NOT_FOUND:
        case grpc::StatusCode::ALREADY_EXISTS:
        case grpc::StatusCode::PERMISSION_DENIED:
        case grpc::StatusCode::FAILED_PRECONDITION:
        case grpc::StatusCode::OUT_OF_RANGE:
        case grpc::StatusCode::UNIMPLEMENTED:
        case grpc::StatusCode::UNAUTHENTICATED:
            return false;
        default:
            return true;
    }
}

template <typename Stream>
Outcome transportFailure(Stream& stream, const string& fileName) {
    const grpc::Status status = stream->Finish();
    if (status.ok()) {
        log(fileName + ": the consumer closed the call without answering, will try again");
        return Outcome::RetryLater;
    }

    const string cause = status.error_message() + " (gRPC code " + to_string(status.error_code()) + ")";
    if (!canRetry(status)) {
        log(fileName + ": FAILED, the consumer can't take this upload: " + cause);
        return Outcome::Failed;
    }
    log(fileName + ": connection problem, will try again: " + cause);
    return Outcome::RetryLater;
}

// the answer has already arrived, so a bad ending here doesn't change the result but is still reported
template <typename Stream>
void finishAnswered(Stream& stream, const string& fileName) {
    const grpc::Status status = stream->Finish();
    if (!status.ok()) {
        log(fileName + ": the call ended badly after its answer arrived: " + status.error_message());
    }
}

Outcome uploadVideo(MediaUpload::Stub& stub, const fs::path& path) {
    const string fileName = path.filename().string();

    string sha256;
    uint64_t size = 0;
    const string hashProblem = hashFile(path, sha256, size);
    if (!hashProblem.empty()) {
        if (!stopping) log(fileName + ": " + hashProblem + ", will try again");
        return Outcome::RetryLater;
    }
    if (size == 0) {
        log(fileName + ": skipped, the file is empty");
        return Outcome::Rejected;
    }

    grpc::ClientContext context;
    context.set_deadline(chrono::system_clock::now() + chrono::seconds(uploadBaseSeconds + size / slowestBytesPerSecond));
    auto stream = stub.Upload(&context);

    UploadMessage headerMessage;
    UploadHeader* header = headerMessage.mutable_header();
    header->set_producer_id(producerId);
    header->set_file_name(fileName);
    header->set_size_bytes(size);
    header->set_sha256(sha256);

    UploadReply reply;
    if (!stream->Write(headerMessage) || !stream->Read(&reply)) {
        return transportFailure(stream, fileName);
    }

    if (reply.status() != ACCEPTED) {
        stream->WritesDone();
        finishAnswered(stream, fileName);
        switch (reply.status()) {
            case QUEUE_FULL:
                log(fileName + ": DROPPED, consumer queue is full (" + to_string(reply.queue_occupied()) + "/" +
                    to_string(reply.queue_capacity()) + ")");
                return Outcome::Dropped;
            case DUPLICATE:
                log(fileName + ": DUPLICATE, not sent (sha256 " + sha256.substr(0, 12) + "...)");
                return Outcome::Duplicate;
            case REJECTED:
                log(fileName + ": REJECTED, " + reply.detail());
                return Outcome::Rejected;
            default:
                log(fileName + ": FAILED, the consumer gave an answer that makes no sense here (" + UploadStatus_Name(reply.status()) + ")");
                return Outcome::Failed;
        }
    }

    log(fileName + ": accepted, sending " + to_string(size) + " bytes");

    errno = 0;
    ifstream file(path, ios::binary);
    string readProblem;
    if (!file) readProblem = "could not open the file again: " + lastErrorText();

    vector<char> buffer(chunkSize);
    uint64_t remaining = size;
    bool writeFailed = false;
    while (readProblem.empty() && remaining > 0 && !stopping) {
        errno = 0;
        file.read(buffer.data(), static_cast<streamsize>(min<uint64_t>(remaining, buffer.size())));
        const streamsize got = file.gcount();
        if (got <= 0) {
            readProblem = file.bad() ? "could not read the file: " + lastErrorText() : "the file got shorter while it was being sent";
            break;
        }

        UploadMessage chunk;
        chunk.set_chunk(buffer.data(), static_cast<size_t>(got));
        if (!stream->Write(chunk)) {
            writeFailed = true;
            break;
        }
        remaining -= static_cast<uint64_t>(got);
    }

    if (remaining > 0) {
        // the consumer may have refused part way through and said why before closing the stream
        if (writeFailed && stream->Read(&reply) && reply.status() == REJECTED) {
            finishAnswered(stream, fileName);
            log(fileName + ": REJECTED part way through, " + reply.detail());
            return Outcome::Rejected;
        }

        context.TryCancel();
        const grpc::Status status = stream->Finish();
        if (!stopping) {
            const string reason = !readProblem.empty() ? readProblem : status.error_message();
            log(fileName + ": transfer stopped with " + to_string(remaining) + " of " + to_string(size) + " bytes left, will try again: " + reason);
        }
        return Outcome::RetryLater;
    }

    if (!stream->WritesDone() || !stream->Read(&reply)) {
        return transportFailure(stream, fileName);
    }
    finishAnswered(stream, fileName);

    if (reply.status() == QUEUED) {
        log(fileName + ": uploaded, waiting in the consumer's queue");
        return Outcome::Uploaded;
    }
    if (reply.status() == REJECTED) {
        log(fileName + ": REJECTED, " + reply.detail());
        return Outcome::Rejected;
    }
    log(fileName + ": FAILED, the consumer gave an answer that makes no sense here (" + UploadStatus_Name(reply.status()) + ")");
    return Outcome::Failed;
}

struct FolderScan {
    vector<fs::path> videos;
    set<string> names;  // every entry, videos or not
    string problem;     // set if the folder could not be read, or not all of it
};

// each file left out is only mentioned once, not on every scan
FolderScan listVideos(const fs::path& folder, set<string>& alreadyMentioned) {
    FolderScan scan;
    error_code error;
    fs::directory_iterator it(folder, error);
    if (error) {
        scan.problem = "could not open the folder " + folder.string() + ": " + error.message();
        return scan;
    }

    for (const fs::directory_iterator end; !error && it != end; it.increment(error)) {
        const string name = it->path().filename().string();
        scan.names.insert(name);

        error_code typeError;
        const bool isFile = it->is_regular_file(typeError);
        if (typeError) {
            if (alreadyMentioned.insert(name).second) log(name + ": left out, could not check what it is: " + typeError.message());
        } else if (!isFile) {
            if (alreadyMentioned.insert(name).second) log(name + ": left out, it is not a regular file");
        } else if (!isVideoFile(it->path())) {
            if (alreadyMentioned.insert(name).second) log(name + ": left out, not one of the supported video types");
        } else {
            scan.videos.push_back(it->path());
        }
    }
    if (error) {
        scan.problem = "could not read all of the folder " + folder.string() + ": " + error.message();
    }

    sort(scan.videos.begin(), scan.videos.end());
    return scan;
}

// Files stay in the folder after their result, so this state is only kept while
// they do. Once one is taken away, a new file given its name counts as new.
template <typename Value>
void forgetMissing(map<string, Value>& entries, const set<string>& present) {
    for (auto it = entries.begin(); it != entries.end();) {
        it = present.count(it->first) > 0 ? next(it) : entries.erase(it);
    }
}

void forgetMissing(set<string>& entries, const set<string>& present) {
    for (auto it = entries.begin(); it != entries.end();) {
        it = present.count(*it) > 0 ? next(it) : entries.erase(it);
    }
}

// Waits for as long as it takes, because the command line program watches the
// consumer and says when it has stopped. Only says anything if the consumer can't
// be reached straight away. false means this producer is being stopped.
bool waitForConsumer(const shared_ptr<grpc::Channel>& channel, const string& address) {
    const auto started = chrono::steady_clock::now();
    auto remindAt = started + chrono::seconds(consumerReminderSeconds);
    bool reported = false;
    while (!stopping) {
        const auto deadline = chrono::system_clock::now() + chrono::seconds(1);
        if (channel->WaitForConnected(deadline)) {
            if (reported) log("the consumer at " + address + " can be reached again");
            return true;
        }

        const auto now = chrono::steady_clock::now();
        if (!reported) {
            log("waiting for the consumer at " + address);
            reported = true;
        } else if (now >= remindAt) {
            const auto waited = chrono::duration_cast<chrono::seconds>(now - started).count();
            log("still waiting for the consumer at " + address + ", " + to_string(waited) + " seconds so far");
            remindAt += chrono::seconds(consumerReminderSeconds);
        }
    }
    return false;
}

void sleepUnlessStopped(int seconds) {
    const auto wakeAt = chrono::steady_clock::now() + chrono::seconds(seconds);
    while (!stopping && chrono::steady_clock::now() < wakeAt) {
        this_thread::sleep_for(chrono::milliseconds(100));
    }
}

int run() {
    producerId = envOr("PRODUCER_ID", "producer");

    if (signal(SIGINT, onStopSignal) == SIG_ERR || signal(SIGTERM, onStopSignal) == SIG_ERR) {
        log("warning: could not set up the stop signals, stopping this producer won't be clean");
    }
    // a closed log pipe should show up as failed writes, not end the upload
    if (signal(SIGPIPE, SIG_IGN) == SIG_ERR) {
        log("warning: if this producer's log is closed, the producer will stop with it");
    }

    const string address = envOr("CONSUMER_ADDRESS", "localhost:50051");
    const fs::path folder = envOr("VIDEO_DIR", "");
    const int scanSeconds = envPositiveNumber("SCAN_SECONDS", "2");

    if (folder.empty()) {
        log("VIDEO_DIR is not set, it must name the folder to watch");
        return 1;
    }

    // notices a consumer that freezes or loses its network, which would otherwise look like a very slow upload
    grpc::ChannelArguments channelArguments;
    channelArguments.SetInt(GRPC_ARG_KEEPALIVE_TIME_MS, keepalivePingMs);
    channelArguments.SetInt(GRPC_ARG_KEEPALIVE_TIMEOUT_MS, keepaliveTimeoutMs);
    channelArguments.SetInt(GRPC_ARG_HTTP2_MAX_PINGS_WITHOUT_DATA, 0);

    auto channel = grpc::CreateCustomChannel(address, grpc::InsecureChannelCredentials(), channelArguments);
    auto stub = MediaUpload::NewStub(channel);
    if (!waitForConsumer(channel, address)) {
        log("stopped");
        return 0;
    }
    log("watching " + folder.string() + " for videos");

    map<string, FileState> lastSeen;  // seen on the previous scan, not uploaded yet
    map<string, FileState> finished;  // already has a final result
    map<string, int> attempts;        // failed tries so far
    set<string> gaveUp;               // ran out of tries
    set<string> mentioned;            // already logged as left out or still changing
    Totals totals;
    int folderFailures = 0;

    while (!stopping) {
        bool anyFinished = false;

        const FolderScan scan = listVideos(folder, mentioned);
        if (scan.problem.empty()) {
            folderFailures = 0;

            // a whole listing is needed to be sure something has really gone
            set<string> present;
            for (const fs::path& path : scan.videos) present.insert(path.filename().string());
            for (auto seen = lastSeen.begin(); seen != lastSeen.end();) {
                if (present.count(seen->first) > 0) {
                    ++seen;
                    continue;
                }
                log(seen->first + ": FAILED, it disappeared before it could be uploaded");
                reportResult(totals, Outcome::Failed, seen->first);
                anyFinished = true;
                seen = lastSeen.erase(seen);
            }
            forgetMissing(finished, present);
            forgetMissing(attempts, present);
            forgetMissing(gaveUp, present);
            forgetMissing(mentioned, scan.names);
        } else {
            folderFailures++;
            if (folderFailures >= maxAttempts) {
                log("stopped watching after " + to_string(maxAttempts) + " tries in a row, " + scan.problem + ", " + totals.text());
                return 1;
            }
            log(scan.problem + ", will try again");
        }

        // counts a failed try and says whether that was the last one allowed
        const auto outOfTries = [&](const string& name) {
            if (++attempts[name] < maxAttempts) return false;
            gaveUp.insert(name);
            lastSeen.erase(name);
            return true;
        };

        for (const fs::path& path : scan.videos) {
            if (stopping) break;

            const string name = path.filename().string();
            if (gaveUp.count(name) > 0) continue;

            error_code sizeError, timeError;
            FileState state;
            state.size = fs::file_size(path, sizeError);
            state.modified = fs::last_write_time(path, timeError);
            if (sizeError || timeError) {
                const string problem = (sizeError ? sizeError : timeError).message();
                if (outOfTries(name)) {
                    log(name + ": FAILED after " + to_string(maxAttempts) + " tries, could not check the file: " + problem);
                    reportResult(totals, Outcome::Failed, name);
                    anyFinished = true;
                } else {
                    log(name + ": could not check the file, will try again: " + problem);
                }
                continue;
            }

            auto done = finished.find(name);
            if (done != finished.end() && done->second == state) continue;

            auto seen = lastSeen.find(name);
            if (seen == lastSeen.end() || !(seen->second == state)) {
                if (seen != lastSeen.end() && mentioned.insert(name).second) {
                    log(name + ": still being written, it will be sent when it stops changing");
                }
                lastSeen[name] = state;  // new or still changing, look again next scan
                continue;
            }

            const Outcome outcome = uploadVideo(*stub, path);
            if (outcome == Outcome::RetryLater) {
                if (stopping) break;
                if (outOfTries(name)) {
                    log(name + ": FAILED, gave up after " + to_string(maxAttempts) + " tries");
                    reportResult(totals, Outcome::Failed, name);
                    anyFinished = true;
                    continue;
                }
                // only false when this producer is being stopped
                if (!waitForConsumer(channel, address)) break;
                continue;
            }

            reportResult(totals, outcome, name);
            finished[name] = state;
            lastSeen.erase(name);
            anyFinished = true;
        }

        if (anyFinished) log(totals.text());
        sleepUnlessStopped(scanSeconds);
    }

    log("stopped, " + totals.text());
    return 0;
}

int main() {
    try {
        return run();
    } catch (const fs::filesystem_error& error) {
        log(string("fatal: a file operation failed: ") + error.what());
    } catch (const bad_alloc&) {
        log("fatal: out of memory");
    } catch (const exception& error) {
        log(string("fatal: ") + error.what());
    } catch (...) {
        log("fatal: an unknown error");
    }
    return 1;
}
