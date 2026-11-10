#pragma once

#include <filesystem>
#include <string>

#include <httplib.h>

#include "activity.h"
#include "leaky_bucket_queue.h"
#include "upload_job.h"
#include "video_library.h"
#include "video_worker.h"

// Serves the browser GUI, its JSON API and the video files.
class WebServer {
public:
    // throws std::runtime_error if the GUI's files aren't there
    WebServer(VideoLibrary& library, LeakyBucketQueue<UploadJob>& queue, VideoWorkerPool& workers, Activity& activity,
              const std::filesystem::path& frontendDir);

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
    httplib::Server server_;
};
