#include "video_worker.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <exception>
#include <iostream>
#include <new>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>

#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/videoio.hpp>

#include "av_mux.h"

using namespace std;
namespace fs = std::filesystem;

namespace {

const double previewSeconds = 10.0;
const int previewMaxHeight = 360;
const double decodedFramesExpected = 0.95;  // the declared frame count is only an estimate, so allow a little either way

struct Cancelled {};

// removes the job's temp files however processing ends
class TempCleanup {
public:
    TempCleanup(fs::path file, fs::path folder) : file_(std::move(file)), folder_(std::move(folder)) {}
    ~TempCleanup() {
        removeAndReport(file_);
        removeAndReport(folder_);
    }

private:
    fs::path file_;
    fs::path folder_;
};

// keeps the busy count right even if processing throws
class BusyMark {
public:
    explicit BusyMark(atomic<int>& count) : count_(count) { count_++; }
    ~BusyMark() { count_--; }

private:
    atomic<int>& count_;
};

void addNote(string& notes, const string& text) {
    if (text.empty()) return;
    if (!notes.empty()) notes += "; ";
    notes += text;
}

// scales down to maxHeight if taller; H.264 needs even dimensions either way
cv::Size scaledSize(const cv::Size& source, int maxHeight) {
    const double scale = source.height > maxHeight ? static_cast<double>(maxHeight) / source.height : 1.0;
    const int width = max(2, static_cast<int>(lround(source.width * scale))) & ~1;
    const int height = max(2, static_cast<int>(lround(source.height * scale))) & ~1;
    return {width, height};
}

void resizeTo(const cv::Mat& frame, cv::Mat& output, const cv::Size& size) {
    cv::resize(frame, output, size, 0, 0, cv::INTER_AREA);
}

// VideoWriter::write reports nothing, so a full disk only shows up when the result is read back
void requireReadable(const fs::path& path, const string& what) {
    cv::VideoCapture check(path.string(), cv::CAP_FFMPEG);
    cv::Mat frame;
    if (!check.isOpened() || !check.read(frame) || frame.empty()) {
        throw runtime_error(what + " could not be read back after encoding, the disk may be full");
    }
}

string storedNameFor(const string& fileName, const string& sha256) {
    const size_t dot = fileName.rfind('.');
    const string stem = dot == string::npos ? fileName : fileName.substr(0, dot);

    string safe;
    for (unsigned char c : stem) {
        const bool allowed = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
        safe += allowed ? static_cast<char>(c) : '_';
    }
    if (safe.size() > 80) safe.resize(80);
    if (safe.empty()) safe = "video";

    // the hash keeps two different videos with the same name apart in the one folder
    return safe + "_" + sha256.substr(0, 8) + ".mp4";
}

// copy then rename, so a half-written file never shows up under its real name
void publish(const fs::path& source, const fs::path& destination) {
    fs::path partial = destination;
    partial += ".part";
    try {
        fs::copy_file(source, partial, fs::copy_options::overwrite_existing);
        fs::rename(partial, destination);
    } catch (const fs::filesystem_error&) {
        removeAndReport(partial);
        throw;
    }
}

}  // namespace

VideoWorkerPool::VideoWorkerPool(int threadCount, int compressHeight, fs::path tempDir, LeakyBucketQueue<UploadJob>& queue,
                                 DedupIndex& dedup, VideoLibrary& library, Activity& activity)
    : threadCount_(threadCount),
      compressHeight_(compressHeight),
      tempDir_(std::move(tempDir)),
      queue_(queue),
      dedup_(dedup),
      library_(library),
      activity_(activity) {}

VideoWorkerPool::~VideoWorkerPool() {
    try {
        stop();
    } catch (const exception& error) {
        cerr << "[consumer] could not stop the consumer threads cleanly: " << error.what() << "\n" << flush;
    }
}

void VideoWorkerPool::start() {
    for (int i = 1; i <= threadCount_; i++) {
        try {
            threads_.emplace_back(&VideoWorkerPool::run, this, i);
        } catch (const system_error& error) {
            throw runtime_error("could not start consumer thread " + to_string(i) + " of " + to_string(threadCount_) + ": " + error.what());
        }
    }
}

void VideoWorkerPool::stop() {
    stopping_ = true;
    queue_.close();
    for (thread& worker : threads_) {
        if (worker.joinable()) worker.join();
    }

    // these were accepted, and their producers were told so, so each one has to be reported
    for (const UploadJob& job : queue_.drain()) {
        abandon(job, "the service stopped before a consumer thread took it, send it again after restarting");
        removeAndReport(job.tempPath);
    }
}

void VideoWorkerPool::abandon(const UploadJob& job, const string& reason) {
    dedup_.release(job.sha256);
    activity_.failed++;
    activity_.record("failed", job.producerId, job.fileName, reason);
    logLine("abandoned: " + job.producerId + " / " + job.fileName + ", " + reason);
}

void VideoWorkerPool::run(int workerNumber) {
    const string name = "worker " + to_string(workerNumber);

    // an exception leaving this function would end the whole program, not just this thread
    try {
        while (optional<UploadJob> job = queue_.pop()) {
            BusyMark busy(busy_);
            try {
                process(workerNumber, *job);
            } catch (const exception& error) {
                // process reports its own failures, so this is the reporting itself having failed
                logLine(name + ": could not finish with " + job->fileName + ": " + error.what());
            }
        }
    } catch (const exception& error) {
        logLine(name + ": stopped early, the other threads carry on: " + error.what());
    } catch (...) {
        logLine(name + ": stopped early because of an unknown error, the other threads carry on");
    }
}

VideoWorkerPool::Encoded VideoWorkerPool::encode(const fs::path& source, const fs::path& videoOut, const fs::path& previewOut,
                                                 const fs::path& posterOut) {
    Encoded result;

    cv::VideoCapture capture(source.string(), cv::CAP_FFMPEG);
    if (!capture.isOpened()) {
        throw runtime_error("FFmpeg could not open the file, it is not a video or it is damaged");
    }
    cv::Mat frame;
    if (!capture.read(frame) || frame.empty()) {
        throw runtime_error("the file opened but no video frame could be decoded from it");
    }

    double fps = capture.get(cv::CAP_PROP_FPS);
    if (!(fps >= 1.0 && fps <= 240.0)) {
        addNote(result.note, "its frame rate (" + to_string(fps) + ") isn't usable so 30 fps was assumed, the timing may be off");
        fps = 30.0;
    }
    const double declaredFrames = capture.get(cv::CAP_PROP_FRAME_COUNT);

    // sizes come from the first decoded frame because OpenCV has already applied any rotation to it
    const cv::Size videoSize = scaledSize(frame.size(), compressHeight_);
    const cv::Size previewSize = scaledSize(frame.size(), min(compressHeight_, previewMaxHeight));

    const int h264 = cv::VideoWriter::fourcc('a', 'v', 'c', '1');
    cv::VideoWriter videoWriter(videoOut.string(), cv::CAP_FFMPEG, h264, fps, videoSize);
    if (!videoWriter.isOpened()) {
        throw runtime_error("OpenCV could not open an H.264 writer for the compressed video");
    }
    cv::VideoWriter previewWriter(previewOut.string(), cv::CAP_FFMPEG, h264, fps, previewSize);
    if (!previewWriter.isOpened()) {
        throw runtime_error("OpenCV could not open an H.264 writer for the preview");
    }

    const uint64_t previewFrames = static_cast<uint64_t>(ceil(previewSeconds * fps));
    uint64_t frames = 0;
    cv::Mat scaled;
    cv::Mat small;

    do {
        if (stopping_) throw Cancelled{};

        resizeTo(frame, scaled, videoSize);
        videoWriter.write(scaled);

        if (frames < previewFrames) {
            resizeTo(frame, small, previewSize);
            previewWriter.write(small);
            if (frames == 0 && !cv::imwrite(posterOut.string(), small)) {
                throw runtime_error("could not write the poster image");
            }
        }
        frames++;
    } while (capture.read(frame) && !frame.empty());

    videoWriter.release();
    previewWriter.release();
    requireReadable(videoOut, "the compressed video");
    requireReadable(previewOut, "the preview");

    // capture.read can't tell the end of the file from damage, so a short count is the only sign of it
    if (declaredFrames > 0 && static_cast<double>(frames) < declaredFrames * decodedFramesExpected) {
        addNote(result.note, "only " + to_string(frames) + " of about " + to_string(static_cast<uint64_t>(declaredFrames)) +
                                 " frames could be decoded, the source is probably damaged and was saved up to that point");
    }

    result.durationSeconds = static_cast<double>(frames) / fps;
    return result;
}

void VideoWorkerPool::process(int workerNumber, const UploadJob& job) {
    const string who = "worker " + to_string(workerNumber) + ": " + job.producerId + " / " + job.fileName;
    const fs::path workDir = tempDir_ / (job.sha256 + ".work");
    TempCleanup cleanup(job.tempPath, workDir);
    vector<fs::path> published;  // removed again if the video doesn't make it into the library

    const auto undoPublished = [&] {
        for (const fs::path& path : published) removeAndReport(path);
    };

    const auto failed = [&](const string& reason) {
        undoPublished();
        dedup_.release(job.sha256);
        activity_.failed++;
        activity_.record("failed", job.producerId, job.fileName, reason);
        logLine(who + ": failed, " + reason);
    };

    try {
        logLine(who + ": compressing");
        fs::create_directories(workDir);

        const fs::path encodedPath = workDir / "encoded.mp4";
        const fs::path previewPath = workDir / "preview.mp4";
        const fs::path posterPath = workDir / "poster.jpg";
        const fs::path finalPath = workDir / "final.mp4";

        const Encoded encoded = encode(job.tempPath, encodedPath, previewPath, posterPath);

        // OpenCV only wrote the picture, so the original audio is copied back in.
        // A problem with the audio should never lose the video, so that retries
        // without it and the note says why it has no sound.
        MuxResult muxed;
        try {
            muxed = muxToMp4(encodedPath.string(), job.tempPath.string(), finalPath.string());
        } catch (const exception& audioError) {
            const string withAudio = audioError.what();
            try {
                muxed = muxToMp4(encodedPath.string(), "", finalPath.string());
            } catch (const exception& videoError) {
                throw runtime_error("could not write the MP4 with audio (" + withAudio + ") or without it (" + videoError.what() + ")");
            }
            muxed.note = "the audio could not be copied, saved without sound: " + withAudio;
        }

        // Already small videos can grow when re-encoded. If so and the original
        // can be played as it is, keep the original stream instead.
        fs::path savedPath = finalPath;
        if (fs::file_size(finalPath) >= job.sizeBytes) {
            const fs::path originalCopy = workDir / "original.mp4";
            try {
                if (canCopyVideoStream(job.tempPath.string())) {
                    MuxResult copied = muxToMp4(job.tempPath.string(), job.tempPath.string(), originalCopy.string());
                    savedPath = originalCopy;
                    muxed = copied;
                    logLine(who + ": re-encoding did not shrink it, kept the original stream");
                }
            } catch (const exception& error) {
                logLine(who + ": could not keep the original stream, using the re-encoded one (" + error.what() + ")");
            }
        }

        string notes = encoded.note;
        addNote(notes, muxed.note);
        if (!notes.empty()) {
            logLine(who + ": " + notes);
        }

        VideoRecord record;
        record.sha256 = job.sha256;
        record.displayName = job.fileName;
        record.producerId = job.producerId;
        record.storedName = storedNameFor(job.fileName, job.sha256);
        record.originalBytes = job.sizeBytes;
        record.savedBytes = fs::file_size(savedPath);
        record.durationSeconds = encoded.durationSeconds;
        record.hasAudio = muxed.hasAudio;
        record.savedAtMs = unixMillisNow();

        // the video goes last so anything listed has all of its files
        const auto publishTracked = [&](const fs::path& source, const fs::path& destination) {
            publish(source, destination);
            published.push_back(destination);
        };
        publishTracked(previewPath, library_.previewsDir() / (job.sha256 + ".mp4"));
        publishTracked(posterPath, library_.previewsDir() / (job.sha256 + ".jpg"));
        publishTracked(savedPath, library_.videosDir() / record.storedName);
        library_.add(record);
        published.clear();  // it's in the library now, so nothing below may remove its files

        activity_.saved++;
        activity_.record("saved", job.producerId, job.fileName, notes);
        logLine(who + ": saved as " + record.storedName + " (" + to_string(record.originalBytes) + " -> " +
                to_string(record.savedBytes) + " bytes" + (record.hasAudio ? ", with audio)" : ", no audio)"));
    } catch (const Cancelled&) {
        undoPublished();
        abandon(job, "the service stopped while it was being compressed, send it again after restarting");
    } catch (const fs::filesystem_error& error) {
        failed(string("a file could not be saved: ") + error.what());
    } catch (const cv::Exception& error) {
        failed("OpenCV failed in " + error.func + ": " + error.err);
    } catch (const bad_alloc&) {
        failed("the consumer ran out of memory");
    } catch (const exception& error) {
        failed(error.what());
    } catch (...) {
        failed("an unknown error");
    }
}
