// SPDX-License-Identifier: MIT
#include <cassert>
#include <cstdio>
#include <stdexcept>
#include "natives_core.h"
using namespace kab;
InsideTable<1> g_inside;
std::atomic<bool> g_watchdog_on{true};
std::atomic<uint64_t> c_watchdog_reports{0};
int64_t g_qpf = 1000;
int sleeps = 0;
int64_t qpc() { return 2000; }
void logw(const char*, ...) {}
void Sleep(int) { if (++sleeps > 1) throw std::runtime_error("watchdog failed to stop"); g_watchdog_on = false; }
#include "watchdog_under_test.h"
int main() {
    g_inside.claim(1, "test.Native", 1);
    try { watchdog_loop(); } catch (const std::runtime_error&) {}
    assert(sleeps == 1 && c_watchdog_reports == 0);
    puts("watchdog_test: shutdown stops scanning without joining a handler");
}
