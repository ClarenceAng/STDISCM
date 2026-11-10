#include "upload_service.h"

#include <cctype>
#include <exception>
#include <fstream>
#include <new>
#include <string>
#include <system_error>
#include <utility>

#include "error_text.h"
#include "sha256.h"

using namespace std;
using namespace mediaupload;
namespace fs = std::filesystem;

namespace {

using UploadStream = grpc::ServerReaderWriter<UploadReply, UploadMessage>;

bool isSha256Hex(const string& text) {
    if (text.size() != 64) return false;
    for (char c : text) {
        const bool hexDigit = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        if (!hexDigit) return false;
    }
    return true;
}

// names come from the network, so keep just the last path part and no control characters
string cleanName(const string& raw) {
    const size_t slash = raw.find_last_of("/\\");
    const string name = slash == string::npos ? raw : raw.substr(slash + 1);

    string clean;
    for (unsigned char c : name) {
        if (c >= 0x20 && c != 0x7f) clean += static_cast<char>(c);
    }
    if (clean.size() > 200) clean.resize(200);
    return clean;
}

// the extension helps FFmpeg recognise the temp file
string safeExtension(const string& name) {
    const size_t dot = name.rfind('.');
    if (dot == string::npos || name.size() - dot > 9) return ".video";

    string extension = ".";
    for (size_t i = dot + 1; i < name.size(); i++) {
        const unsigned char c = static_cast<unsigned char>(name[i]);
        if (!isalnum(c)) return ".video";
        extension += static_cast<char>(tolower(c));
    }
    return extension.size() > 1 ? extension : ".video";
}

// Holds the hash claim and queue slot for one upload, and gives back whichever
// of them it has on every way out of the handler unless the job reached the queue.
class Admission {
public:
    Admission(LeakyBucketQueue<UploadJob>& queue, DedupIndex& dedup, string sha256)
        : queue_(queue), dedup_(dedup), sha256_(std::move(sha256)) {}

    Admission(const Admission&) = delete;
    Admission& operator=(const Admission&) = delete;

    ~Admission() {
        if (!committed_) {
            if (reserved_) queue_.cancel();
            dedup_.release(sha256_);
        }
    }

    bool reserveSlot() {
        reserved_ = queue_.tryReserve();
        return reserved_;
    }

    void commit(UploadJob job) {
        queue_.commit(std::move(job));
        committed_ = true;
    }

private:
    LeakyBucketQueue<UploadJob>& queue_;
    DedupIndex& dedup_;
    string sha256_;
    bool reserved_ = false;
    bool committed_ = false;
};

}  // namespace

UploadService::UploadService(LeakyBucketQueue<UploadJob>& queue, DedupIndex& dedup, Activity& activity,
                             fs::path tempDir, uint64_t maxUploadBytes)
    : queue_(queue), dedup_(dedup), activity_(activity), tempDir_(std::move(tempDir)), maxUploadBytes_(maxUploadBytes) {}

grpc::Status UploadService::Upload(grpc::ServerContext* context, UploadStream* stream) {
    string producerId = "(unknown producer)";
    string fileName = "(unknown file)";
    fs::path tempPath;

    // gRPC turns an exception leaving a handler into a bare UNKNOWN status and nothing
    // records why. By the time this runs, Admission has already given the slot back.
    const auto unexpected = [&](const string& reason) {
        activity_.failed++;
        activity_.record("failed", producerId, fileName, reason);
        logLine(producerId + " / " + fileName + ": failed, " + reason);
        if (!tempPath.empty()) removeAndReport(tempPath);
        return grpc::Status(grpc::StatusCode::INTERNAL, reason);
    };

    try {
        return receive(context, stream, producerId, fileName, tempPath);
    } catch (const fs::filesystem_error& error) {
        return unexpected(string("a file operation failed: ") + error.what());
    } catch (const bad_alloc&) {
        return unexpected("the consumer ran out of memory");
    } catch (const exception& error) {
        return unexpected(error.what());
    } catch (...) {
        return unexpected("an unknown error");
    }
}

grpc::Status UploadService::receive(grpc::ServerContext* context, UploadStream* stream, string& producerId, string& fileName,
                                    fs::path& tempPath) {
    const auto who = [&] { return producerId + " / " + fileName; };

    // false means the producer never got this answer
    const auto reply = [&](UploadStatus status, const string& detail = "") {
        const QueueSnapshot snapshot = queue_.snapshot();
        UploadReply message;
        message.set_status(status);
        message.set_detail(detail);
        message.set_queue_occupied(static_cast<uint32_t>(snapshot.occupied));
        message.set_queue_capacity(static_cast<uint32_t>(snapshot.capacity));

        const bool sent = stream->Write(message);
        if (!sent) {
            logLine(who() + ": could not send the " + UploadStatus_Name(status) + " answer, the producer's connection has gone");
        }
        return sent;
    };

    const auto refuseBeforeHeader = [&](const string& reason, grpc::StatusCode code) {
        activity_.record("rejected", producerId, fileName, reason);
        logLine(who() + ": rejected, " + reason);
        return grpc::Status(code, reason);
    };

    UploadMessage first;
    if (!stream->Read(&first)) {
        return refuseBeforeHeader("the connection closed before the upload header arrived", grpc::StatusCode::CANCELLED);
    }
    if (!first.has_header()) {
        return refuseBeforeHeader("the first message must be the upload header", grpc::StatusCode::INVALID_ARGUMENT);
    }

    const UploadHeader& header = first.header();
    const string cleanProducer = cleanName(header.producer_id());
    const string cleanFile = cleanName(header.file_name());
    if (!cleanProducer.empty()) producerId = cleanProducer;
    if (!cleanFile.empty()) fileName = cleanFile;
    const string& sha256 = header.sha256();
    const uint64_t expectedSize = header.size_bytes();

    string problem;
    if (cleanProducer.empty()) problem = "the producer id is empty";
    else if (cleanFile.empty()) problem = "the file name is empty";
    else if (!isSha256Hex(sha256)) problem = "the sha256 is not 64 lowercase hex digits";
    else if (expectedSize == 0) problem = "the file is empty";
    else if (expectedSize > maxUploadBytes_) problem = "the file is larger than the " + to_string(maxUploadBytes_ / (1024 * 1024)) + " MB limit";

    if (!problem.empty()) {
        activity_.record("rejected", producerId, fileName, problem);
        logLine(who() + ": rejected, " + problem);
        reply(REJECTED, problem);
        return grpc::Status::OK;
    }

    // The claim comes first so a duplicate never takes up a queue slot.
    if (!dedup_.claim(sha256)) {
        activity_.duplicates++;
        activity_.record("duplicate", producerId, fileName, "same SHA-256 as an earlier video");
        logLine(who() + ": duplicate, sha256 " + sha256.substr(0, 12) + "...");
        reply(DUPLICATE);
        return grpc::Status::OK;
    }

    Admission admission(queue_, dedup_, sha256);
    if (!admission.reserveSlot()) {
        activity_.dropped++;
        activity_.record("dropped", producerId, fileName, "queue full");
        logLine(who() + ": dropped, queue full");
        reply(QUEUE_FULL);
        return grpc::Status::OK;
    }

    tempPath = tempDir_ / (sha256 + safeExtension(fileName));

    const auto fail = [&](const string& reason) {
        activity_.failed++;
        activity_.record("failed", producerId, fileName, reason);
        logLine(who() + ": failed, " + reason);
        removeAndReport(tempPath);
    };

    const auto rejectUpload = [&](const string& reason) {
        fail(reason);
        reply(REJECTED, reason);
        return grpc::Status::OK;
    };

    if (!reply(ACCEPTED)) {
        fail("the producer disconnected before it was told to send");
        return grpc::Status(grpc::StatusCode::CANCELLED, "the producer disconnected");
    }

    errno = 0;
    ofstream output(tempPath, ios::binary | ios::trunc);
    if (!output) {
        return rejectUpload("could not create the temp file: " + lastErrorText());
    }

    Sha256 hash;
    uint64_t received = 0;
    UploadMessage message;
    while (stream->Read(&message)) {
        if (message.content_case() != UploadMessage::kChunk) {
            return rejectUpload("expected a chunk after the header");
        }

        const string& chunk = message.chunk();
        received += chunk.size();
        if (received > expectedSize) {
            return rejectUpload("received more bytes than the header declared");
        }

        hash.update(chunk.data(), chunk.size());
        errno = 0;
        output.write(chunk.data(), static_cast<streamsize>(chunk.size()));
        if (!output) {
            return rejectUpload("could not write the temp file: " + lastErrorText());
        }
    }

    errno = 0;
    output.close();
    const bool closed = static_cast<bool>(output);
    const string closeProblem = closed ? "" : lastErrorText();

    const string progress = to_string(received) + " of " + to_string(expectedSize) + " bytes";
    if (context->IsCancelled()) {
        fail("the producer disconnected or timed out after " + progress);
        return grpc::Status(grpc::StatusCode::ABORTED, "incomplete upload");
    }
    if (received != expectedSize) {
        fail("the producer stopped sending after " + progress);
        return grpc::Status(grpc::StatusCode::ABORTED, "incomplete upload");
    }
    if (!closed) {
        return rejectUpload("could not finish the temp file: " + closeProblem);
    }
    if (hash.finishHex() != sha256) {
        return rejectUpload("the received bytes do not match the SHA-256");
    }

    const fs::path queuedPath = tempPath;
    admission.commit({sha256, producerId, fileName, queuedPath, expectedSize});
    tempPath.clear();  // the job owns the file now, so a failure below must not delete it
    activity_.accepted++;
    logLine(who() + ": received " + to_string(expectedSize) + " bytes, queued");

    // If this fails the video is still queued. The producer will send it again and be told it's a duplicate.
    reply(QUEUED);
    return grpc::Status::OK;
}
