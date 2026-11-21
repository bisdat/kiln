// SPDX-License-Identifier: MIT
// kiln/state/launch.json on the Windows code path (test-launch-wine.sh, headless Wine): common.cpp's
// write_file_atomic, cut out of the file as it is, writing launch::state_json (src/launch.cpp, as it is).
//  - A reader on another thread polling every millisecond, opening the file as the launcher's Rust code does (read,
//    write and delete sharing),
//    never sees a partial or unparsable record while the file is replaced 300 times; the last record wins and no
//    launch.json.tmp is left behind. A replacing rename is refused while the reader holds the file (counted, not
//    checked: it depends on timing); launch::write_retrying, as launch_boot.cpp uses it, gets every write through.
//  - The freeze writes it once per boot: a second freeze leaves the first record in the file.
#include <windows.h>

#include <atomic>
#include <cstdio>
#include <filesystem>
#include <string>
#include <string_view>
#include <thread>

#include "launch.h"
#include "nlohmann/json.hpp"

namespace kiln {
#include "write_file_atomic.inc"
}  // namespace kiln

namespace {
int checks = 0, failures = 0;
void check(bool ok, const char* what) {
    ++checks;
    if (!ok) {
        ++failures;
        std::printf("FAIL %s\n", what);
    }
}

// The whole file as the launcher reads it (Rust's std::fs::File::open shares read, write and delete), "" when absent.
std::string read_shared(const std::filesystem::path& file) {
    HANDLE h = CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return {};
    std::string out;
    char buf[4096];
    DWORD got = 0;
    while (ReadFile(h, buf, sizeof buf, &got, nullptr) && got) out.append(buf, got);
    CloseHandle(h);
    return out;
}

std::string id_in(const std::string& text) {
    const auto doc = nlohmann::json::parse(text, nullptr, false);
    return doc.is_object() && doc.contains("launchId") && doc["launchId"].is_string() ? doc["launchId"].get<std::string>() : "";
}
}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) return 2;
    const std::filesystem::path dir = argv[1];
    const std::filesystem::path file = dir / L"launch.json";
    std::filesystem::create_directories(dir);
    kiln::launch::Decision d;
    d.backend = KILN_LAUNCH_BACKEND_KILN;
    d.reason_code = KILN_LAUNCH_REASON_PLAN;
    d.reason = std::string(2000, 'r');  // a record of a few KiB, written in one piece or not at all
    d.notice = "Your ShroudForge mods run on Kiln's own interpreter this start.";
    check(kiln::write_file_atomic(file, kiln::launch::state_json(d, 1, 1, 1, "test")), "the previous boot's record");

    std::atomic<bool> stop{false};
    std::atomic<int> reads{0}, bad{0};
    std::thread reader([&] {
        while (!stop.load()) {
            const std::string text = read_shared(file);
            if (text.empty()) continue;  // between a reader's open attempts the name always exists; empty = not opened
            ++reads;
            if (id_in(text).empty()) ++bad;
            Sleep(1);  // a launcher polling every millisecond: far more often than any does
        }
    });
    // First without the retry, to see the contention: a replacing rename is refused while the reader has the file open.
    int raw_failed = 0;
    DWORD raw_error = 0;
    for (uint64_t id = 2; id <= 101; ++id)
        if (!kiln::write_file_atomic(file, kiln::launch::state_json(d, id, id, 1, "test"))) ++raw_failed, raw_error = GetLastError();
    // Then as launch_boot.cpp writes it: retried, 25 ms apart, 20 tries at most.
    bool all_written = true;
    int retried = 0;
    for (uint64_t id = 102; id <= 301; ++id) {
        const std::string text = kiln::launch::state_json(d, id, id, 1, "test");
        int tries = 0;
        all_written &= kiln::launch::write_retrying([&] { return ++tries, kiln::write_file_atomic(file, text); },
                                                    [](uint32_t ms) { Sleep(ms); });
        retried += tries > 1;
    }
    stop.store(true);
    reader.join();
    std::printf("without the retry: %d of 100 replacements refused (last error %lu); with it, %d of 200 needed a retry\n",
                raw_failed, raw_error, retried);
    check(all_written, "with the retry, 200 replacements written while a shared reader reads");
    check(reads.load() > 0, "the reader read the file meanwhile");
    check(bad.load() == 0, "the reader never saw a partial or unparsable record");
    check(id_in(read_shared(file)) == "000000000000012d", "the last record is the one in the file");
    std::error_code ec;
    check(!std::filesystem::exists(dir / L"launch.json.tmp", ec), "no launch.json.tmp is left behind");

    // once per boot: the freeze that takes writes, a second one does not
    const auto write = [&](uint64_t id) {
        return [&, id] { kiln::write_file_atomic(file, kiln::launch::state_json(d, id, id, 1, "test")); };
    };
    check(kiln::launch::freeze(d, 1000, 1, write(1000)), "the boot's freeze");
    check(!kiln::launch::freeze(d, 1001, 1, write(1001)), "a second freeze is refused");
    check(id_in(read_shared(file)) == "00000000000003e8", "launch.json holds the first freeze's record only");
    std::printf("launch wine: %d checks, %d failures (%d shared reads)\n", checks, failures, reads.load());
    return failures ? 1 : 0;
}
