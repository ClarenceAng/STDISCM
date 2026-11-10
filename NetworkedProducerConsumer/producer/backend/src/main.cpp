// One producer instance: uploads the videos at its path to the consumer over
// gRPC, one at a time, and finishes when each of them has a result.

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
const int maxAttempts = 5;           // tries per video, and in a row for the folder, before giving up on it
const int consumerWaitSeconds = 120;  // how long the consumer may be unreachable before this producer gives up
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

    int all() const { return uploaded + dropped + duplicates + rejected + failed; }
};

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
    string problem;  // set if the folder could not be read, or not all of it
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

// only says anything if the consumer can't be reached straight away; false means it never answered
bool waitForConsumer(const shared_ptr<grpc::Channel>& channel, const string& address) {
    const auto giveUpAt = chrono::steady_clock::now() + chrono::seconds(consumerWaitSeconds);
    bool reported = false;
    while (!stopping) {
        const auto deadline = chrono::system_clock::now() + chrono::seconds(1);
        if (channel->WaitForConnected(deadline)) return true;

        if (!reported) {
            log("waiting for the consumer at " + address);
            reported = true;
        }
        if (chrono::steady_clock::now() >= giveUpAt) {
            log("gave up, the consumer at " + address + " did not answer for " + to_string(consumerWaitSeconds) + " seconds");
            return false;
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
        log("VIDEO_DIR is not set, it must name the folder to read");
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
        if (stopping) {
            log("stopped");
            return 0;
        }
        return 1;
    }

    map<string, FileState> lastSeen;  // seen on the previous scan, not uploaded yet
    map<string, FileState> finished;  // already has a final result
    map<string, int> attempts;        // failed tries so far
    set<string> gaveUp;               // ran out of tries
    set<string> mentioned;            // already logged as left out or still changing
    Totals totals;
    int folderFailures = 0;
    bool folderGaveUp = false;
    bool consumerGone = false;

    while (!stopping) {
        bool anyFinished = false;
        bool allFinished = true;

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
                totals.failed++;
                anyFinished = true;
                seen = lastSeen.erase(seen);
            }
        } else {
            allFinished = false;
            folderFailures++;
            if (folderFailures >= maxAttempts) {
                log("gave up after " + to_string(maxAttempts) + " tries, " + scan.problem);
                folderGaveUp = true;
                break;
            }
            log(scan.problem + ", will try again");
        }

        // counts a failed try and says whether that was the last one allowed
        const auto outOfTries = [&](const string& name) {
            if (++attempts[name] < maxAttempts) return false;
            gaveUp.insert(name);
            lastSeen.erase(name);
            totals.failed++;
            anyFinished = true;
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
                } else {
                    log(name + ": could not check the file, will try again: " + problem);
                    allFinished = false;
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
                allFinished = false;
                continue;
            }

            const Outcome outcome = uploadVideo(*stub, path);
            if (outcome == Outcome::RetryLater) {
                if (stopping) break;
                if (outOfTries(name)) {
                    log(name + ": FAILED, gave up after " + to_string(maxAttempts) + " tries");
                    continue;
                }
                allFinished = false;
                if (!waitForConsumer(channel, address)) {
                    consumerGone = !stopping;
                    break;
                }
                continue;
            }

            if (outcome == Outcome::Uploaded) totals.uploaded++;
            if (outcome == Outcome::Dropped) totals.dropped++;
            if (outcome == Outcome::Duplicate) totals.duplicates++;
            if (outcome == Outcome::Rejected) totals.rejected++;
            if (outcome == Outcome::Failed) totals.failed++;
            finished[name] = state;
            lastSeen.erase(name);
            anyFinished = true;
        }

        if (anyFinished) {
            log("totals: " + to_string(totals.uploaded) + " uploaded, " + to_string(totals.dropped) + " dropped, " +
                to_string(totals.duplicates) + " duplicate, " + to_string(totals.rejected) + " rejected, " +
                to_string(totals.failed) + " failed");
        }
        if (consumerGone || allFinished) break;
        sleepUnlessStopped(scanSeconds);
    }

    if (stopping) {
        log("stopped");
        return 0;
    }

    if (totals.all() == 0 && !folderGaveUp && !consumerGone) {
        log("no videos found");
    }
    log(folderGaveUp || consumerGone ? "finished early, not everything at this path was uploaded" : "finished");

    // the consumer's own answers, even a refusal, count as a result; 1 means something never got one
    return totals.failed > 0 || folderGaveUp || consumerGone ? 1 : 0;
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
