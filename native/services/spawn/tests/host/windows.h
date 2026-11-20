// SPDX-License-Identifier: MIT
// Host-only stand-in for tracker.cpp's one Windows call (tests/tracker_test.cpp): MoveFileExW as a rename, which the
// test can slow down to play a busy hard disk.
#pragma once
#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>

constexpr unsigned MOVEFILE_REPLACE_EXISTING = 1, MOVEFILE_WRITE_THROUGH = 8;
inline std::atomic<int> g_test_disk_ms{0};      // each MoveFileExW takes this long
inline std::atomic<int> g_test_disk_calls{0};
inline int MoveFileExW(const char* from, const char* to, unsigned) {
    if (int ms = g_test_disk_ms.load()) std::this_thread::sleep_for(std::chrono::milliseconds(ms));
    ++g_test_disk_calls;
    return std::rename(from, to) == 0;
}
