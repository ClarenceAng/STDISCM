#include "video_library.h"

#include <stdexcept>
#include <system_error>
#include <utility>

#include <nlohmann/json.hpp>

#include "activity.h"
#include "error_text.h"

using namespace std;
using json = nlohmann::json;
namespace fs = std::filesystem;

namespace {

json toJson(const VideoRecord& record) {
    return {{"sha256", record.sha256},
            {"displayName", record.displayName},
            {"producerId", record.producerId},
            {"storedName", record.storedName},
            {"originalBytes", record.originalBytes},
            {"savedBytes", record.savedBytes},
            {"durationSeconds", record.durationSeconds},
            {"hasAudio", record.hasAudio},
            {"savedAtMs", record.savedAtMs}};
}

VideoRecord fromJson(const json& value) {
    VideoRecord record;
    value.at("sha256").get_to(record.sha256);
    value.at("displayName").get_to(record.displayName);
    value.at("producerId").get_to(record.producerId);
    value.at("storedName").get_to(record.storedName);
    value.at("originalBytes").get_to(record.originalBytes);
    value.at("savedBytes").get_to(record.savedBytes);
    value.at("durationSeconds").get_to(record.durationSeconds);
    value.at("hasAudio").get_to(record.hasAudio);
    value.at("savedAtMs").get_to(record.savedAtMs);
    return record;
}

// the manifest can be edited by hand and these two end up in file paths
void requireSafePaths(const VideoRecord& record) {
    if (record.sha256.size() != 64 || record.sha256.find_first_not_of("0123456789abcdef") != string::npos) {
        throw runtime_error("its sha256 is not 64 lowercase hex digits");
    }
    if (record.storedName.empty() || record.storedName != fs::path(record.storedName).filename().string() ||
        record.storedName == "." || record.storedName == "..") {
        throw runtime_error("its storedName is not a plain file name");
    }
}

// empty means the file is there
string fileProblem(const fs::path& path) {
    error_code error;
    const bool there = fs::exists(path, error);
    if (error) return "could not check " + path.string() + ": " + error.message();
    return there ? "" : path.string() + " is missing";
}

}  // namespace

VideoLibrary::VideoLibrary(fs::path dataDir) : dataDir_(std::move(dataDir)) {
    fs::create_directories(videosDir());
    fs::create_directories(previewsDir());
    load();
}

void VideoLibrary::load() {
    const fs::path path = dataDir_ / "manifest.jsonl";

    // No manifest just means nothing has been saved yet. One that exists but can't
    // be read must stop the start-up, or every saved video would look new again.
    error_code statusError;
    const fs::file_status status = fs::status(path, statusError);
    if (status.type() == fs::file_type::not_found) {
        logLine("library is empty, there is no manifest yet");
        return;
    }
    if (statusError) {
        throw runtime_error("could not check " + path.string() + ": " + statusError.message());
    }

    errno = 0;
    ifstream manifest(path, ios::binary);
    if (!manifest) {
        throw runtime_error("could not open " + path.string() + ": " + lastErrorText());
    }

    string line;
    int lineNumber = 0;
    bool endedWithNewline = true;
    while (getline(manifest, line)) {
        lineNumber++;
        endedWithNewline = !manifest.eof();
        if (line.empty()) continue;

        const string where = "manifest line " + to_string(lineNumber);
        try {
            VideoRecord record = fromJson(json::parse(line));
            requireSafePaths(record);

            // a video deleted by hand should be allowed to come back
            const string missingVideo = fileProblem(videosDir() / record.storedName);
            if (!missingVideo.empty()) {
                logLine(where + ": " + record.displayName + " left out of the library, it can be uploaded again (" + missingVideo + ")");
                continue;
            }

            // the video itself still plays, so it stays listed and this says how to repair it
            for (const fs::path& extra : {previewsDir() / (record.sha256 + ".mp4"), previewsDir() / (record.sha256 + ".jpg")}) {
                const string missingExtra = fileProblem(extra);
                if (!missingExtra.empty()) {
                    logLine(where + ": " + record.displayName + " has no preview or poster (" + missingExtra +
                            "), delete it from storage/videos and upload it again to rebuild them");
                }
            }

            // a video deleted by hand and uploaded again has two lines, and the newer one is the real one
            const auto earlier = indexBySha_.find(record.sha256);
            if (earlier != indexBySha_.end()) {
                logLine(where + ": " + record.displayName + " replaces an earlier line with the same SHA-256");
                records_[earlier->second] = std::move(record);
                continue;
            }

            indexBySha_[record.sha256] = records_.size();
            records_.push_back(std::move(record));
        } catch (const json::exception& error) {
            logLine(where + " skipped, it is not a valid record: " + error.what());
        } catch (const runtime_error& error) {
            logLine(where + " skipped: " + error.what());
        }
    }

    if (manifest.bad()) {
        throw runtime_error("could not read all of " + path.string() + ": " + lastErrorText());
    }

    needsNewline_ = !endedWithNewline;
    if (needsNewline_) {
        logLine("the manifest's last line is unfinished, the next video starts on a new line");
    }
    logLine("library has " + to_string(records_.size()) + " saved video(s)");
}

void VideoLibrary::add(const VideoRecord& record) {
    unique_lock<shared_mutex> lock(mutex_);

    errno = 0;
    ofstream manifest(dataDir_ / "manifest.jsonl", ios::app | ios::binary);
    if (!manifest) {
        throw runtime_error("could not open the manifest: " + lastErrorText());
    }

    if (needsNewline_) manifest << '\n';
    needsNewline_ = true;  // stays set if this write fails part way through the line
    manifest << toJson(record).dump(-1, ' ', false, json::error_handler_t::replace) << '\n';
    manifest.close();
    if (!manifest) {
        throw runtime_error("could not update the manifest: " + lastErrorText());
    }
    needsNewline_ = false;

    indexBySha_[record.sha256] = records_.size();
    records_.push_back(record);
}

vector<VideoRecord> VideoLibrary::list() const {
    shared_lock<shared_mutex> lock(mutex_);
    return records_;
}

optional<VideoRecord> VideoLibrary::find(const string& sha256) const {
    shared_lock<shared_mutex> lock(mutex_);
    auto found = indexBySha_.find(sha256);
    if (found == indexBySha_.end()) return nullopt;
    return records_[found->second];
}
