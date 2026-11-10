#pragma once

#include <mutex>
#include <string>
#include <unordered_set>

// Every SHA-256 that is saved or currently on its way in. claim() checks and
// inserts under one lock, so if two producers send the same video at the same
// moment exactly one of them gets it.
class DedupIndex {
public:
    // true: this upload owns the hash. false: it's a duplicate.
    bool claim(const std::string& sha256) {
        std::lock_guard<std::mutex> lock(mutex_);
        return known_.insert(sha256).second;
    }

    // the upload or its processing failed, so the video may be sent again
    void release(const std::string& sha256) {
        std::lock_guard<std::mutex> lock(mutex_);
        known_.erase(sha256);
    }

private:
    std::mutex mutex_;
    std::unordered_set<std::string> known_;
};
