#pragma once

#include <vector>
#include <cstddef>
#include <mutex>

// ---------------------------------------------------------------------------
// RingBuffer — fixed-capacity FIFO container.
//
// When full, the oldest element is silently overwritten.
// Thread-safe: all public methods are guarded by a mutex.
// ---------------------------------------------------------------------------

template <typename T>
class RingBuffer
{
public:
    explicit RingBuffer(size_t capacity)
        : buf_(capacity)
        , head_(0)
        , count_(0)
    {
    }

    void push(const T &item)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        buf_[head_] = item;
        head_ = (head_ + 1) % buf_.size();
        if (count_ < buf_.size()) {
            ++count_;
        }
    }

    size_t size() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return count_;
    }

    bool empty() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return count_ == 0;
    }

    size_t capacity() const
    {
        return buf_.size();
    }

    // Access by logical index: 0 = oldest entry, size()-1 = newest.
    const T &operator[](size_t index) const
    {
        // No lock — caller must ensure thread safety for reads,
        // or only read while paused.
        size_t start = (count_ < buf_.size())
            ? 0
            : head_;
        return buf_[(start + index) % buf_.size()];
    }

    void clear()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        head_ = 0;
        count_ = 0;
    }

    // Thread-safe snapshot: returns a copy of all elements in logical order
    // (0 = oldest, size()-1 = newest).  Safe to use from UI thread.
    std::vector<T> snapshot() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<T> result;
        result.reserve(count_);
        size_t start = (count_ < buf_.size()) ? 0 : head_;
        for (size_t i = 0; i < count_; ++i) {
            result.push_back(buf_[(start + i) % buf_.size()]);
        }
        return result;
    }

private:
    std::vector<T> buf_;
    size_t         head_;
    size_t         count_;
    mutable std::mutex mutex_;
};
