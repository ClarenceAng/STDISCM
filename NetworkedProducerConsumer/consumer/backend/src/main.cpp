// The media upload service: receives videos over gRPC, processes them on c
// consumer threads and shows them in a browser GUI.

#include <atomic>
#include <charconv>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <limits>
#include <memory>
#include <new>
#include <pthread.h>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <unistd.h>

#include <grpcpp/grpcpp.h>
#include <opencv2/core/utils/logger.hpp>

extern "C" {
#include <libavutil/log.h>
}

#include "activity.h"
#include "dedup_index.h"
#include "leaky_bucket_queue.h"
#include "upload_job.h"
#include "upload_service.h"
#include "video_library.h"
#include "video_worker.h"
#include "web_server.h"

using namespace std;
namespace fs = std::filesystem;

namespace {

const int largestPort = 65535;
const int keepalivePingMs = 20000;
const int keepaliveTimeoutMs = 10000;

string envOr(const char* name, const string& fallback) {
    const char* value = getenv(name);
    return value != nullptr && *value != '\0' ? value : fallback;
}

// the compose generator has already checked these, this guards a hand-run binary
int envNumber(const char* name, int fallback, int largest = numeric_limits<int>::max()) {
    const string text = envOr(name, to_string(fallback));
    const char* const end = text.data() + text.size();
    int number = 0;
    const from_chars_result parsed = from_chars(text.data(), end, number);
    if (parsed.ec != errc{} || parsed.ptr != end || number < 1 || number > largest) {
        throw runtime_error(string(name) + " must be a whole number from 1 to " + to_string(largest) + ", not '" + text + "'");
    }
    return number;
}

// The folder is emptied on every start, so a mistaken TEMP_DIR must not be able
// to take the saved videos or the whole disk with it.
void resetTempDir(const fs::path& tempDir, const fs::path& dataDir) {
    const fs::path temp = fs::weakly_canonical(tempDir);
    const fs::path dataFromTemp = fs::weakly_canonical(dataDir).lexically_relative(temp);
    const bool holdsData = !dataFromTemp.empty() && *dataFromTemp.begin() != "..";
    if (temp == temp.root_path() || holdsData) {
        throw runtime_error("TEMP_DIR " + tempDir.string() + " can't be used, emptying it would delete the saved videos or the whole disk");
    }

    // anything left here is from a run that was killed; the contents go, not the folder, as it may be a mount point
    fs::create_directories(temp);
    for (const fs::directory_entry& entry : fs::directory_iterator(temp)) {
        fs::remove_all(entry.path());
    }
}

// Runs on every way out of main, because a std::thread that is still running
// when it is destroyed ends the program before the real error can be reported.
struct StopWebServer {
    WebServer& web;
    thread& webThread;

    ~StopWebServer() {
        web.stop();
        webThread.join();
    }
};

}  // namespace

int main() {
    // Blocked here so every thread inherits it and the signal is only taken by sigwait below.
    sigset_t stopSignals;
    sigemptyset(&stopSignals);
    sigaddset(&stopSignals, SIGINT);
    sigaddset(&stopSignals, SIGTERM);
    const int maskError = pthread_sigmask(SIG_BLOCK, &stopSignals, nullptr);
    if (maskError != 0) {
        logLine("fatal: could not set up the stop signals: " + generic_category().message(maskError));
        return 1;
    }

    atomic<bool> webFailed{false};

    try {
        av_log_set_level(AV_LOG_ERROR);
        cv::utils::logging::setLogLevel(cv::utils::logging::LOG_LEVEL_ERROR);

        const int consumerThreads = envNumber("CONSUMER_THREADS", 2);
        const int queueCapacity = envNumber("QUEUE_CAPACITY", 5);
        const int grpcPort = envNumber("GRPC_PORT", 50051, largestPort);
        const int httpPort = envNumber("HTTP_PORT", 8080, largestPort);
        if (grpcPort == httpPort) {
            // both servers allow a shared port, so binding would work and the connections would be split between them
            throw runtime_error("GRPC_PORT and HTTP_PORT must be different, both are " + to_string(grpcPort));
        }
        const int compressHeight = envNumber("COMPRESS_HEIGHT", 480);
        const uint64_t maxUploadBytes = static_cast<uint64_t>(envNumber("MAX_UPLOAD_MB", 2048)) * 1024 * 1024;
        const fs::path dataDir = envOr("DATA_DIR", "/data");
        const fs::path frontendDir = envOr("FRONTEND_DIR", "/app/frontend");
        const fs::path tempDir = envOr("TEMP_DIR", "/tmp/media-upload");

        resetTempDir(tempDir, dataDir);

        VideoLibrary library(dataDir);
        DedupIndex dedup;
        for (const VideoRecord& record : library.list()) {
            dedup.claim(record.sha256);
        }

        LeakyBucketQueue<UploadJob> queue(static_cast<size_t>(queueCapacity));
        Activity activity;

        VideoWorkerPool workers(consumerThreads, compressHeight, tempDir, queue, dedup, library, activity);
        workers.start();

        // the GUI is checked before anything can be uploaded, so a broken one stops the start-up
        WebServer web(library, queue, workers, activity, frontendDir);
        if (!web.bind(httpPort)) {
            throw runtime_error("could not listen for HTTP on port " + to_string(httpPort) + ", is something else using it?");
        }

        UploadService service(queue, dedup, activity, tempDir, maxUploadBytes);
        grpc::ServerBuilder builder;
        const string grpcAddress = "0.0.0.0:" + to_string(grpcPort);
        int boundPort = 0;
        builder.AddListeningPort(grpcAddress, grpc::InsecureServerCredentials(), &boundPort);
        builder.RegisterService(&service);

        // A producer that freezes or loses its network never closes its stream and would
        // hold its queue slot for ever. These pings notice that and cancel the upload.
        builder.AddChannelArgument(GRPC_ARG_KEEPALIVE_TIME_MS, keepalivePingMs);
        builder.AddChannelArgument(GRPC_ARG_KEEPALIVE_TIMEOUT_MS, keepaliveTimeoutMs);
        builder.AddChannelArgument(GRPC_ARG_KEEPALIVE_PERMIT_WITHOUT_CALLS, 0);
        // nothing is sent back during an upload, and by default that stops the pings after two
        builder.AddChannelArgument(GRPC_ARG_HTTP2_MAX_PINGS_WITHOUT_DATA, 0);
        // producers ping too, and by default a few of those during a long upload count as abuse
        builder.AddChannelArgument(GRPC_ARG_HTTP2_MIN_RECV_PING_INTERVAL_WITHOUT_DATA_MS, keepalivePingMs / 2);
        builder.AddChannelArgument(GRPC_ARG_HTTP2_MAX_PING_STRIKES, 0);

        unique_ptr<grpc::Server> grpcServer = builder.BuildAndStart();
        if (!grpcServer || boundPort == 0) {
            throw runtime_error("could not listen for gRPC on " + grpcAddress + ", is something else using it?");
        }

        thread webThread([&] {
            try {
                if (web.serve()) return;
                logLine("fatal: the HTTP server stopped accepting connections");
            } catch (const exception& error) {
                logLine(string("fatal: the HTTP server failed: ") + error.what());
            } catch (...) {
                logLine("fatal: the HTTP server failed with an unknown error");
            }
            webFailed = true;
            // sent to the process, not this thread, so that it reaches the sigwait below
            if (kill(getpid(), SIGTERM) != 0) {
                logLine("could not ask the service to stop, restart it to get the GUI back");
            }
        });
        StopWebServer stopWeb{web, webThread};

        logLine("ready: " + to_string(consumerThreads) + " consumer thread(s), queue length " + to_string(queueCapacity) +
                ", gRPC on " + to_string(grpcPort) + ", GUI on http://localhost:" + to_string(httpPort));

        int signalNumber = 0;
        const int waitError = sigwait(&stopSignals, &signalNumber);
        if (waitError != 0) {
            throw system_error(waitError, generic_category(), "could not wait for a stop signal");
        }
        logLine(webFailed ? "stopping because the GUI failed" : "stopping");

        grpcServer->Shutdown(chrono::system_clock::now() + chrono::seconds(2));
        workers.stop();
    } catch (const fs::filesystem_error& error) {
        logLine(string("fatal: a storage or temp folder can't be used: ") + error.what());
        return 1;
    } catch (const system_error& error) {
        logLine(string("fatal: the system refused a thread or lock: ") + error.what());
        return 1;
    } catch (const bad_alloc&) {
        logLine("fatal: out of memory");
        return 1;
    } catch (const exception& error) {
        logLine(string("fatal: ") + error.what());
        return 1;
    } catch (...) {
        logLine("fatal: an unknown error");
        return 1;
    }

    return webFailed ? 1 : 0;
}
