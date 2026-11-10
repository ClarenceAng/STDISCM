#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

struct VideoRecord {
    std::string sha256;
    std::string displayName;
    std::string producerId;
    std::string storedName;  // file name inside the videos folder
    std::uint64_t originalBytes = 0;
    std::uint64_t savedBytes = 0;
    double durationSeconds = 0;
    bool hasAudio = false;
    std::int64_t savedAtMs = 0;
};

// The saved videos. Consumer threads add to it and web requests read it, so it
// uses a shared_mutex: many readers at once, one writer at a time. A manifest
// file brings the list (and duplicate detection) back after a restart.
class VideoLibrary {
public:
    explicit VideoLibrary(std::filesystem::path dataDir);

    std::filesystem::path videosDir() const { return dataDir_ / "videos"; }
    std::filesystem::path previewsDir() const { return dataDir_ / "previews"; }

    void add(const VideoRecord& record);
    std::vector<VideoRecord> list() const;
    std::optional<VideoRecord> find(const std::string& sha256) const;

private:
    void load();

    std::filesystem::path dataDir_;
    mutable std::shared_mutex mutex_;
    std::vector<VideoRecord> records_;
    std::unordered_map<std::string, std::size_t> indexBySha_;
    bool needsNewline_ = false;  // the manifest ends part way through a line, from a crash or a failed write
};
