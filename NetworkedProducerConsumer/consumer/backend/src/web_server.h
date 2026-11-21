#pragma once

#include <filesystem>
#include <string>

// CMakeLists.txt defines CPPHTTPLIB_OPENSSL_SUPPORT, which is what gives httplib.h its SSLServer
#include <httplib.h>

#include "activity.h"
#include "leaky_bucket_queue.h"
#include "tls_files.h"
#include "upload_job.h"
#include "video_library.h"
#include "video_worker.h"

// Serves the browser GUI, its JSON API and the video files over HTTPS.
class WebServer {
public:
    // throws std::runtime_error if the GUI's files aren't there or TLS can't be set up with `tls`
    WebServer(VideoLibrary& library, LeakyBucketQueue<UploadJob>& queue, VideoWorkerPool& workers, Activity& activity,
              const std::filesystem::path& frontendDir, const ServerTls& tls);

    bool bind(int port);  // false means the port can't be used
    bool serve();         // blocks until stop() is called, false means it failed while running
    void stop();

private:
    std::string videosJson() const;
    std::string statusJson() const;

    VideoLibrary& library_;
    LeakyBucketQueue<UploadJob>& queue_;
    VideoWorkerPool& workers_;
    Activity& activity_;
    std::string tlsProblem_;  // why server_ could not set up TLS; declared first because server_'s constructor sets it
    httplib::SSLServer server_;
};
