#pragma once

#include <atomic>
#include <cstddef>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include "activity.h"
#include "dedup_index.h"
#include "leaky_bucket_queue.h"
#include "upload_job.h"
#include "video_library.h"

// The c consumer threads. Each one takes finished uploads off the queue,
// compresses them, cuts the 10 second preview and saves the results.
class VideoWorkerPool {
public:
    VideoWorkerPool(int threadCount, int compressHeight, std::filesystem::path tempDir, LeakyBucketQueue<UploadJob>& queue,
                    DedupIndex& dedup, VideoLibrary& library, Activity& activity);
    ~VideoWorkerPool();  // a thread still running when its std::thread is destroyed would end the whole program

    void start();
    void stop();  // closes the queue, abandons work in progress, joins the threads and reports what was left

    int threadCount() const { return threadCount_; }
    int busyThreads() const { return busy_.load(); }

private:
    void run(int workerNumber);
    void process(int workerNumber, const UploadJob& job);
    void abandon(const UploadJob& job, const std::string& reason);

    struct Encoded {
        double durationSeconds = 0;
        std::string note;  // anything about the source that could affect the result
    };
    Encoded encode(const std::filesystem::path& source, const std::filesystem::path& videoOut,
                   const std::filesystem::path& previewOut, const std::filesystem::path& posterOut);

    int threadCount_;
    int compressHeight_;
    std::filesystem::path tempDir_;
    LeakyBucketQueue<UploadJob>& queue_;
    DedupIndex& dedup_;
    VideoLibrary& library_;
    Activity& activity_;

    std::vector<std::thread> threads_;
    std::atomic<bool> stopping_{false};
    std::atomic<int> busy_{0};
};
