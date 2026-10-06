#pragma once

#include <algorithm>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <exception>
#include <mutex>
#include <thread>
#include <vector>

namespace hft::data {

// Runs `src.read(dst, n)` on its own thread, a few blocks ahead of the consumer, so
// decompression overlaps with parsing. Errors from the source rethrow in read().
template <class Source>
class Prefetch {
   public:
    explicit Prefetch(Source& src, std::size_t block = 4u << 20, std::size_t depth = 4)
        : depth_(depth), thread_([this, &src, block] { run(src, block); }) {}

    ~Prefetch() {
        {
            std::lock_guard lock(mu_);
            stop_ = true;
        }
        space_.notify_all();
        thread_.join();
    }

    Prefetch(const Prefetch&) = delete;
    Prefetch& operator=(const Prefetch&) = delete;

    // Fills dst with up to n bytes; returns less than n only at the end of the stream.
    std::size_t read(std::uint8_t* dst, std::size_t n) {
        std::size_t done = 0;
        while (done < n) {
            if (pos_ == cur_.size() && !next()) break;
            const std::size_t k = std::min(n - done, cur_.size() - pos_);
            std::memcpy(dst + done, cur_.data() + pos_, k);
            pos_ += k;
            done += k;
        }
        return done;
    }

   private:
    bool next() {
        std::unique_lock lock(mu_);
        data_.wait(lock, [this] { return !queue_.empty() || finished_; });
        if (queue_.empty()) {
            if (error_) std::rethrow_exception(error_);
            return false;
        }
        cur_ = std::move(queue_.front());
        queue_.pop_front();
        pos_ = 0;
        space_.notify_one();
        return true;
    }

    void run(Source& src, std::size_t block) {
        try {
            for (;;) {
                std::vector<std::uint8_t> b(block);
                std::size_t n = 0;
                while (n < block) {
                    const std::size_t k = src.read(b.data() + n, block - n);
                    if (k == 0) break;
                    n += k;
                }
                if (n == 0) break;
                b.resize(n);
                std::unique_lock lock(mu_);
                space_.wait(lock, [this] { return queue_.size() < depth_ || stop_; });
                if (stop_) return;
                queue_.push_back(std::move(b));
                data_.notify_one();
            }
        } catch (...) {
            std::lock_guard lock(mu_);
            error_ = std::current_exception();
        }
        {
            std::lock_guard lock(mu_);
            finished_ = true;
        }
        data_.notify_all();
    }

    std::size_t depth_;
    std::mutex mu_;
    std::condition_variable data_;
    std::condition_variable space_;
    std::deque<std::vector<std::uint8_t>> queue_;
    std::exception_ptr error_;
    bool finished_ = false;
    bool stop_ = false;
    std::vector<std::uint8_t> cur_;
    std::size_t pos_ = 0;
    std::thread thread_;
};

}  // namespace hft::data
