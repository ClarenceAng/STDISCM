#include "web_server.h"

#include <algorithm>
#include <cstdint>
#include <exception>
#include <fstream>
#include <memory>
#include <new>
#include <optional>
#include <stdexcept>
#include <system_error>

#include <nlohmann/json.hpp>

#include "error_text.h"

using namespace std;
using json = nlohmann::json;
namespace fs = std::filesystem;

namespace {

const int webThreads = 64;  // browsers hold connections open while a video plays

string dump(const json& value) {
    return value.dump(-1, ' ', false, json::error_handler_t::replace);
}

void refuse(httplib::Response& response, int status, const string& reason) {
    response.status = status;
    response.set_content(reason, "text/plain; charset=utf-8");
}

// streams from disk; cpp-httplib applies the browser's Range header to this,
// which is what lets the video player seek
void sendFile(httplib::Response& response, const fs::path& path, const char* contentType) {
    error_code error;
    const uintmax_t size = fs::file_size(path, error);
    if (error == errc::no_such_file_or_directory) {
        logLine("GUI request: " + path.string() + " is in the library but missing from storage");
        refuse(response, 404, "this file is no longer in storage");
        return;
    }
    if (error) {
        logLine("GUI request: could not check " + path.string() + ": " + error.message());
        refuse(response, 500, "the file could not be read");
        return;
    }
    if (size == 0) {
        logLine("GUI request: " + path.string() + " is empty");
        refuse(response, 500, "the file in storage is empty");
        return;
    }

    errno = 0;
    auto file = make_shared<ifstream>(path, ios::binary);
    if (!*file) {
        logLine("GUI request: could not open " + path.string() + ": " + lastErrorText());
        refuse(response, 500, "the file could not be read");
        return;
    }

    response.set_header("Accept-Ranges", "bytes");
    response.set_content_provider(
        static_cast<size_t>(size), contentType,
        [file, path](size_t offset, size_t length, httplib::DataSink& sink) {
            // this runs after the route handler, where the exception handler can't help
            try {
                char buffer[64 * 1024];
                errno = 0;
                file->clear();
                file->seekg(static_cast<streamoff>(offset));
                file->read(buffer, static_cast<streamsize>(min(length, sizeof(buffer))));
                const streamsize got = file->gcount();
                if (got <= 0) {
                    // the size went out with the headers, so all that's left is to cut the response short
                    logLine("GUI request: could not read " + path.string() + " at byte " + to_string(offset) + ": " + lastErrorText());
                    return false;
                }

                // false means the browser hung up, which it does on purpose every time the viewer seeks
                return sink.write(buffer, static_cast<size_t>(got));
            } catch (const exception& problem) {
                logLine("GUI request: failed while sending " + path.string() + ": " + problem.what());
                return false;
            }
        });
}

}  // namespace

WebServer::WebServer(VideoLibrary& library, LeakyBucketQueue<UploadJob>& queue, VideoWorkerPool& workers, Activity& activity,
                     const fs::path& frontendDir)
    : library_(library), queue_(queue), workers_(workers), activity_(activity) {
    server_.new_task_queue = [] { return new httplib::ThreadPool(webThreads); };

    // without this cpp-httplib answers 500 and nothing records why
    server_.set_exception_handler([](const httplib::Request& request, httplib::Response& response, exception_ptr thrown) {
        string reason;
        try {
            rethrow_exception(thrown);
        } catch (const bad_alloc&) {
            reason = "the consumer ran out of memory";
        } catch (const exception& error) {
            reason = error.what();
        } catch (...) {
            reason = "an unknown error";
        }
        logLine("GUI request " + request.path + " failed: " + reason);
        refuse(response, 500, reason);
    });

    server_.Get("/api/videos", [this](const httplib::Request&, httplib::Response& response) {
        response.set_header("Cache-Control", "no-store");
        response.set_content(videosJson(), "application/json");
    });

    server_.Get("/api/status", [this](const httplib::Request&, httplib::Response& response) {
        response.set_header("Cache-Control", "no-store");
        response.set_content(statusJson(), "application/json");
    });

    // the hash is looked up in the library, so a URL can't name any other file
    server_.Get(R"(/media/([0-9a-f]{64})/(video|preview|poster))", [this](const httplib::Request& request, httplib::Response& response) {
        const string sha256 = request.matches[1];
        const string kind = request.matches[2];

        const optional<VideoRecord> record = library_.find(sha256);
        if (!record) {
            refuse(response, 404, "there is no video with this id");
            return;
        }

        if (kind == "video") {
            sendFile(response, library_.videosDir() / record->storedName, "video/mp4");
        } else if (kind == "preview") {
            sendFile(response, library_.previewsDir() / (sha256 + ".mp4"), "video/mp4");
        } else {
            sendFile(response, library_.previewsDir() / (sha256 + ".jpg"), "image/jpeg");
        }
    });

    error_code error;
    if (!fs::exists(frontendDir / "index.html", error) || !server_.set_mount_point("/", frontendDir.string())) {
        throw runtime_error("the GUI's files are not in " + frontendDir.string() + (error ? ": " + error.message() : ""));
    }
}

bool WebServer::bind(int port) {
    return server_.bind_to_port("0.0.0.0", port);
}

bool WebServer::serve() {
    try {
        return server_.listen_after_bind();
    } catch (...) {
        server_.decommission();  // so stop() doesn't wait for a listener that is never coming
        throw;
    }
}

void WebServer::stop() {
    // cpp-httplib's stop() does nothing until listening has begun, so a stop right at start-up has to wait for that
    server_.wait_until_ready();
    server_.stop();
}

string WebServer::videosJson() const {
    json videos = json::array();
    for (const VideoRecord& record : library_.list()) {
        videos.push_back({{"id", record.sha256},
                          {"name", record.displayName},
                          {"producer", record.producerId},
                          {"originalBytes", record.originalBytes},
                          {"savedBytes", record.savedBytes},
                          {"durationSeconds", record.durationSeconds},
                          {"hasAudio", record.hasAudio},
                          {"savedAtMs", record.savedAtMs}});
    }
    return dump(videos);
}

string WebServer::statusJson() const {
    const QueueSnapshot queue = queue_.snapshot();

    json recent = json::array();
    for (const ActivityEvent& event : activity_.recent()) {
        recent.push_back({{"atMs", event.atMs},
                          {"kind", event.kind},
                          {"producer", event.producerId},
                          {"file", event.fileName},
                          {"detail", event.detail}});
    }

    return dump({{"queue", {{"capacity", queue.capacity}, {"occupied", queue.occupied}, {"waiting", queue.waiting}}},
                 {"workers", {{"total", workers_.threadCount()}, {"busy", workers_.busyThreads()}}},
                 {"totals",
                  {{"accepted", activity_.accepted.load()},
                   {"saved", activity_.saved.load()},
                   {"dropped", activity_.dropped.load()},
                   {"duplicates", activity_.duplicates.load()},
                   {"failed", activity_.failed.load()}}},
                 {"recent", recent}});
}
