#pragma once

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <iterator>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

struct QueueSnapshot {
    std::size_t capacity;
    std::size_t occupied;  // uploads being received + uploads waiting for a consumer thread
    std::size_t waiting;   // just the ones waiting for a consumer thread
};

// The bounded buffer between the gRPC handlers (producer side) and the consumer
// threads. A slot is reserved before an upload's bytes arrive, so q counts
// uploads in progress as well as uploads waiting, and a full bucket can turn a
// video away before anything is transferred.
//
// Only the consumer side ever blocks. The producer side never waits for space,
// which is what makes this a leaky bucket: whatever doesn't fit is dropped.
template <typename T>
class LeakyBucketQueue {
public:
    explicit LeakyBucketQueue(std::size_t capacity) : capacity_(capacity) {}

    // false means the bucket is full and the video should be dropped
    bool tryReserve() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (closed_ || occupied_ >= capacity_) return false;
        occupied_++;
        return true;
    }

    // the upload finished: its reserved slot now holds the item
    void commit(T item) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            waiting_.push_back(std::move(item));
        }
        notEmpty_.notify_one();
    }

    // the upload failed: gives its reserved slot back
    void cancel() {
        std::lock_guard<std::mutex> lock(mutex_);
        occupied_--;
    }

    // waits for an item, which frees its slot; empty means the queue was closed
    std::optional<T> pop() {
        std::unique_lock<std::mutex> lock(mutex_);
        notEmpty_.wait(lock, [this] { return closed_ || !waiting_.empty(); });
        if (closed_) return std::nullopt;

        T item = std::move(waiting_.front());
        waiting_.pop_front();
        occupied_--;
        return item;
    }

    // wakes every waiting thread for shutdown
    void close() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            closed_ = true;
        }
        notEmpty_.notify_all();
    }

    // after close(): hands back the items no consumer thread took, so they can be reported
    std::vector<T> drain() {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<T> left(std::make_move_iterator(waiting_.begin()), std::make_move_iterator(waiting_.end()));
        occupied_ -= waiting_.size();
        waiting_.clear();
        return left;
    }

    QueueSnapshot snapshot() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return {capacity_, occupied_, waiting_.size()};
    }

private:
    const std::size_t capacity_;
    mutable std::mutex mutex_;
    std::condition_variable notEmpty_;
    std::deque<T> waiting_;
    std::size_t occupied_ = 0;
    bool closed_ = false;
};
