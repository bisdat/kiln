// SPDX-License-Identifier: MIT
// kiln-emf's disk thread (io_worker.h): emf.json, world settings kept per slot, mod settings, emf_core.ini and
// emf/commands.txt. The server world's tick only hands work over (0.2.0-dev.29.17).
#include "emf_internal.h"
#include "io_worker.h"

namespace kemf {

namespace {

std::mutex g_slow_mutex;
uint64_t g_slow_last = 0, g_slow_skipped = 0;

io::Worker& worker() {
    // Never deleted: the process ends with it (on_unload stops the thread while every thread still runs).
    static io::Worker* w = [] {
        io::Worker::Hooks h;
        h.write = [](const std::filesystem::path& p, const std::string& text) { return write_file_atomic(p, text); };
        h.write_failed = [](const std::filesystem::path& p) { logf(KILN_LOG_WARN, "cannot write %s", path_utf8(p).c_str()); };
        h.poll = [] { commands_poll_files(); };
        h.poll_what = "checking emf\\commands.txt and emf_core.ini";
        h.poll_ms = 500;
        h.slow_ms = 1000.0;
        h.slow = [](const std::string& what, double ms) {
            uint64_t skipped = 0;
            {
                std::lock_guard lock(g_slow_mutex);
                const uint64_t now = now_ms();
                if (g_slow_last && now - g_slow_last < 60000) {
                    ++g_slow_skipped;
                    return;
                }
                g_slow_last = now;
                skipped = g_slow_skipped;
                g_slow_skipped = 0;
            }
            logf(KILN_LOG_INFO, "disk: %s took %.0f ms on kiln-emf's own thread (the disk is slow; the game did not wait)%s",
                 what.c_str(), ms, skipped ? format("; %llu more since the last line", static_cast<unsigned long long>(skipped)).c_str() : "");
        };
        return new io::Worker(std::move(h));
    }();
    return *w;
}

std::atomic<bool> g_threaded{false};

// Without a thread (it could not start) the work runs at once, as before 0.2.0-dev.29.17.
void pump_if_unthreaded() {
    if (!g_threaded) worker().pump();
}

}  // namespace

void io_start() {
    if (g_threaded) return;
    g_threaded = worker().start();
    if (!g_threaded) logf(KILN_LOG_WARN, "disk: kiln-emf's disk thread did not start; its files are written on the calling thread");
}

void io_write(const std::filesystem::path& p, std::string text) {
    worker().write(p, std::move(text));
    pump_if_unthreaded();
}

void io_post(const char* what, std::function<void()> job) {
    worker().post(what ? what : "a disk job", std::move(job));
    pump_if_unthreaded();
}

void io_stop(uint64_t timeout_ms) {
    worker().stop(timeout_ms);
    g_threaded = false;
}

}  // namespace kemf
