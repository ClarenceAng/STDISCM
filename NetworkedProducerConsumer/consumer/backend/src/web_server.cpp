#include "web_server.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <fstream>
#include <memory>
#include <new>
#include <optional>
#include <stdexcept>
#include <system_error>

#include <nlohmann/json.hpp>
#include <openssl/err.h>
#include <openssl/ssl.h>

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

string handshakeAdvice(int reason) {
    switch (reason) {
        case SSL_R_HTTP_REQUEST:
            return "it asked for http://, and the GUI only answers https://";
        case SSL_R_TLSV1_ALERT_UNKNOWN_CA:
        case SSL_R_SSLV3_ALERT_CERTIFICATE_UNKNOWN:
        case SSL_R_SSLV3_ALERT_BAD_CERTIFICATE:
            return "the browser refused the certificate. Either mkcert's certificate authority is not trusted on that "
                   "computer (run.bat or run.sh adds it with mkcert -install), or the address used is not localhost or 127.0.0.1";
        case SSL_R_UNEXPECTED_EOF_WHILE_READING:
            // only reached once the browser has sent something, see reportFailedHandshake
            return "the browser hung up part way through. Some (Windows' own TLS, which curl uses there, for one) do that "
                   "instead of saying why when they don't trust the certificate, so check mkcert's certificate authority "
                   "is trusted on that computer";
        case SSL_R_NO_SHARED_CIPHER:
        case SSL_R_UNSUPPORTED_PROTOCOL:
            return "the browser and the consumer have no TLS version or cipher in common";
        default:
            return "for the reason OpenSSL gives";
    }
}

// OpenSSL calls this as each TLS handshake moves along. cpp-httplib closes a
// connection whose handshake failed without saying anything, so this says why.
void reportFailedHandshake(const SSL* ssl, int where, int ret) {
    // this runs inside OpenSSL's C code, which an exception must not pass through
    try {
        if ((where & SSL_CB_HANDSHAKE_START) != 0) {
            // OpenSSL's error queue belongs to the thread, and a connection this thread
            // served before may have left errors in it that have nothing to do with this one
            ERR_clear_error();
            return;
        }
        if ((where & SSL_CB_ACCEPT_EXIT) != SSL_CB_ACCEPT_EXIT || ret > 0) return;

        const unsigned long error = ERR_peek_last_error();
        if (error == 0) {
            // cpp-httplib's sockets don't block, so the handshake stops each time it waits for the browser
            if (SSL_want_read(ssl) || SSL_want_write(ssl)) return;
            logLine("GUI: a browser's connection broke during the TLS handshake, OpenSSL gave no reason");
            return;
        }
        if (ERR_GET_REASON(error) == SSL_R_UNEXPECTED_EOF_WHILE_READING && SSL_get_state(ssl) == TLS_ST_BEFORE) {
            // Closed before sending anything. Browsers open spare connections ahead of
            // time and close the ones they don't need, so this is not a failure.
            return;
        }

        char text[256];
        ERR_error_string_n(error, text, sizeof(text));  // peeked, not taken: cpp-httplib still reads the queue after this
        logLine("GUI: a browser's TLS handshake failed, " + handshakeAdvice(ERR_GET_REASON(error)) + " (OpenSSL: " + text + ")");
    } catch (...) {
        // only building the message can throw (out of memory), and logLine reports its own failed writes
        fputs("[consumer] GUI: a browser's TLS handshake failed, and there wasn't the memory to say why\n", stderr);
    }
}

}  // namespace

WebServer::WebServer(VideoLibrary& library, LeakyBucketQueue<UploadJob>& queue, VideoWorkerPool& workers, Activity& activity,
                     const fs::path& frontendDir, const ServerTls& tls)
    : library_(library),
      queue_(queue),
      workers_(workers),
      activity_(activity),
      server_([this, &tls](SSL_CTX& context) {
          // cpp-httplib only learns true or false from here, so the reason is kept for the constructor to report
          try {
              useCertificateAndKey(context, tls.certificatePem, tls.privateKeyPem, tls.certificateWhat, tls.keyWhat);
              if (SSL_CTX_set_min_proto_version(&context, TLS1_2_VERSION) != 1) {
                  throw runtime_error("OpenSSL refused TLS 1.2 as the oldest version allowed: " + openSslErrors());
              }
              SSL_CTX_set_options(&context, SSL_OP_NO_COMPRESSION | SSL_OP_NO_RENEGOTIATION);
              SSL_CTX_set_info_callback(&context, reportFailedHandshake);
              return true;
          } catch (const exception& error) {
              tlsProblem_ = error.what();
              return false;
          }
      }) {
    if (!server_.is_valid()) {
        throw runtime_error("could not set up HTTPS for the GUI: " +
                            (tlsProblem_.empty() ? "OpenSSL could not create a TLS context: " + openSslErrors() : tlsProblem_));
    }

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
