// SPDX-License-Identifier: MIT
// kiln-emf's disk thread (0.2.0-dev.29.17). Nothing on the server world's tick thread may wait for the disk: on a busy
// hard disk (the game's autosave writing, an antivirus scan) one file create, rename or metadata call there can take
// seconds, and the world stands still for as long (a player's tick-stats.jsonl: kiln-emf's Pre at 0.2-1.9 s about once
// every 5-25 minutes, on the minute its emf.json refresh ran, with no EMF mod installed).
//
// So the tick hands its file work to this worker and goes on:
//  - write(path, text): an atomic file write. A write of a file that is still waiting is replaced by the newer text
//    (the last one wins, in the place of the first: a status file refreshed twice while the disk is slow is written once),
//    unless a job was posted after it (the job may read the file: then the newer text is a write of its own, after it).
//  - post(job): any other disk work, in order with the writes (FIFO), on the worker's thread.
//  - poll: called about every poll_ms between jobs (kiln-emf: emf/commands.txt and emf_core.ini while the server steps).
// A job that takes longer than slow_ms is told to `slow` (kiln-emf logs it, rate-limited: the next report then says the
// disk was slow, and that the game did not wait for it).
//
// Portable (std::thread), so that the host test (tests/io_worker_test.cpp) runs it. Without start() nothing runs on a
// thread of its own: pump() runs what waits on the caller (the test, or a worker that could not start).
#pragma once
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

namespace kemf::io {

class Worker {
public:
    struct Hooks {
        std::function<bool(const std::filesystem::path&, const std::string&)> write;  // the atomic write itself
        std::function<void()> poll;                                                     // optional
        std::function<void(const std::string& what, double ms)> slow;                  // optional
        std::function<void(const std::filesystem::path&)> write_failed;                // optional
        std::string poll_what = "polling";                                              // how `slow` names a poll
        uint64_t poll_ms = 500;
        double slow_ms = 1000.0;
    };

    explicit Worker(Hooks h) : h_(std::move(h)) {}
    ~Worker() { stop(0); }  // kiln-emf never destroys its worker (io.cpp); the tests do
    Worker(const Worker&) = delete;
    Worker& operator=(const Worker&) = delete;

    // Starts the thread (once). False when it cannot start: the caller then pumps.
    bool start() {
        std::lock_guard lock(m_);
        if (thread_.joinable() || stopping_) return thread_.joinable();
        try {
            thread_ = std::thread([this] { run(); });
        } catch (...) {
            return false;
        }
        return true;
    }
    bool running() const {
        std::lock_guard lock(m_);
        return thread_.joinable() && !stopping_;
    }

    void write(const std::filesystem::path& file, std::string text) {
        {
            std::lock_guard lock(m_);
            // the newest waiting write of this file, unless a job waits behind it (that job may read the file)
            for (auto it = q_.rbegin(); it != q_.rend() && it->is_write; ++it)
                if (it->file == file) {
                    it->text = std::move(text);
                    ++coalesced_;
                    return;
                }
            q_.push_back(Item{true, file, std::move(text), {}, {}});
        }
        cv_.notify_all();
    }

    void post(std::string what, std::function<void()> job) {
        {
            std::lock_guard lock(m_);
            q_.push_back(Item{false, {}, {}, std::move(job), std::move(what)});
        }
        cv_.notify_all();
    }

    // Runs everything that waits, on the caller (a test; or a worker whose thread never started). Returns how many ran.
    size_t pump() {
        size_t n = 0;
        for (;;) {
            Item it;
            {
                std::lock_guard lock(m_);
                if (q_.empty()) break;
                it = std::move(q_.front());
                q_.pop_front();
                busy_ = true;
            }
            run_item(it);
            {
                std::lock_guard lock(m_);
                busy_ = false;
            }
            ++n;
        }
        done_cv_.notify_all();
        return n;
    }
    void poll_now() {
        if (h_.poll) h_.poll();
    }

    // Waits until nothing waits or runs, at most timeout_ms. False on timeout.
    bool flush(uint64_t timeout_ms) {
        if (!running()) {
            pump();
            return true;
        }
        std::unique_lock lock(m_);
        return done_cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms), [this] { return q_.empty() && !busy_; });
    }

    // Flushes (at most timeout_ms), then ends the thread. A thread still busy after that is left to the process's end
    // (detached: on_unload runs while the process exits and must not hang).
    void stop(uint64_t timeout_ms) {
        const bool drained = flush(timeout_ms);
        std::thread t;
        {
            std::lock_guard lock(m_);
            stopping_ = true;
            t = std::move(thread_);
        }
        cv_.notify_all();
        if (!t.joinable()) return;
        if (drained) t.join();
        else t.detach();
    }

    uint64_t coalesced() const {
        std::lock_guard lock(m_);
        return coalesced_;
    }
    size_t waiting() const {
        std::lock_guard lock(m_);
        return q_.size();
    }

private:
    struct Item {
        bool is_write = false;
        std::filesystem::path file;
        std::string text;
        std::function<void()> job;
        std::string what;
    };

    void run_item(Item& it) {
        const auto t0 = std::chrono::steady_clock::now();
        std::string what;
        if (it.is_write) {
            bool ok = false;
            try {
                ok = h_.write && h_.write(it.file, it.text);
            } catch (...) {
            }
            if (!ok && h_.write_failed) h_.write_failed(it.file);
            what = "writing " + it.file.filename().string();
        } else {
            try {
                if (it.job) it.job();
            } catch (...) {
            }
            what = it.what;
        }
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        if (ms > h_.slow_ms && h_.slow) h_.slow(what, ms);
    }

    void run() {
        auto next_poll = std::chrono::steady_clock::now() + std::chrono::milliseconds(h_.poll_ms);
        for (;;) {
            Item it;
            bool have = false, poll = false;
            {
                std::unique_lock lock(m_);
                cv_.wait_until(lock, next_poll, [this] { return stopping_ || !q_.empty(); });
                if (!q_.empty()) {
                    it = std::move(q_.front());
                    q_.pop_front();
                    busy_ = have = true;
                } else if (stopping_) {
                    break;
                } else if (std::chrono::steady_clock::now() >= next_poll) {
                    poll = true;
                    busy_ = true;
                }
            }
            if (have) run_item(it);
            if (poll) {
                const auto t0 = std::chrono::steady_clock::now();
                try {
                    poll_now();
                } catch (...) {
                }
                const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
                if (ms > h_.slow_ms && h_.slow) h_.slow(h_.poll_what, ms);
                next_poll = std::chrono::steady_clock::now() + std::chrono::milliseconds(h_.poll_ms);
            }
            {
                std::lock_guard lock(m_);
                busy_ = false;
                if (q_.empty()) done_cv_.notify_all();
            }
        }
        std::lock_guard lock(m_);
        done_cv_.notify_all();
    }

    Hooks h_;
    mutable std::mutex m_;
    std::condition_variable cv_, done_cv_;
    std::deque<Item> q_;
    std::thread thread_;
    bool busy_ = false, stopping_ = false;
    uint64_t coalesced_ = 0;
};

}  // namespace kemf::io
