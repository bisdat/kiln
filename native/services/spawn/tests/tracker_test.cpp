// SPDX-License-Identifier: MIT
// kiln.spawn's tracker sidecar (tracker.cpp) on a slow disk, host only (tests/host/windows.h: MoveFileExW is a rename
// the test slows down). The tracker's calls run on the server world's tick: since 0.2.0-dev.29.17 none of them waits for
// the disk (a player's world stood still 2.3 s at its first save, in the rekey). Checked: add, replace and rekey return
// while the disk takes 400 ms a write; flush waits; the files end up as before (the newest text in the new sidecar, the
// old one removed); a waiting write is replaced by a newer one; open reads what the last world wrote.
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>

#include "tracker.h"
#include <windows.h>  // tests/host/windows.h: the slow MoveFileExW

namespace kspawn {
// game.cpp's Guid (the game's parts of game.cpp do not build on the host)
bool Guid::zero() const {
    for (uint8_t x : b)
        if (x) return false;
    return true;
}
bool Guid::operator==(const Guid& o) const { return std::memcmp(b, o.b, 16) == 0; }
bool Guid::operator<(const Guid& o) const { return std::memcmp(b, o.b, 16) < 0; }
std::string Guid::text() const {
    char s[40];
    std::snprintf(s, sizeof s, "%02x%02x%02x%02x-0000-0000-0000-000000000000", b[0], b[1], b[2], b[3]);
    return s;
}
bool Guid::parse(std::string_view s, Guid* out) {
    if (s.size() < 8) return false;
    *out = Guid{};
    for (int i = 0; i < 4; ++i) out->b[i] = static_cast<uint8_t>(std::stoul(std::string(s.substr(i * 2, 2)), nullptr, 16));
    return true;
}
}  // namespace kspawn

namespace {
int checks = 0, failures = 0;
void check(bool ok, const char* what) {
    ++checks;
    if (!ok) {
        ++failures;
        std::printf("FAIL: %s\n", what);
    }
}
double ms_since(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}
std::string read(const std::filesystem::path& p) {
    std::ifstream f(p, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}
kspawn::tracker::Record rec(uint64_t serial, const char* name) {
    kspawn::tracker::Record r;
    r.serial = serial;
    r.tmpl.b[0] = 0xab;
    r.name = name;
    r.live = true;
    r.entity = static_cast<uint32_t>(serial + 100);
    return r;
}
}  // namespace

int main(int argc, char** argv) {
    namespace tr = kspawn::tracker;
    const std::filesystem::path dir = std::filesystem::path(argc > 1 ? argv[1] : "/tmp/kiln-spawn-tracker-test") / "spawn";
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    const auto unknown = dir / "unknown-world.json", keyed = dir / "abcd.json";

    tr::open(unknown, "unknown-world", "");
    check(tr::count() == 0, "a new world: no records");

    g_test_disk_ms = 400;  // a busy hard disk
    auto t0 = std::chrono::steady_clock::now();
    tr::add(rec(tr::next_serial(), "first"));
    tr::add(rec(tr::next_serial(), "second"));  // replaces the first add's text if it still waits
    tr::add(rec(tr::next_serial(), "third"));
    check(ms_since(t0) < 200, "three adds return while each write takes 400 ms");
    t0 = std::chrono::steady_clock::now();
    tr::rekey(keyed, "abcd", "world8.ksd");  // the world's first save
    check(ms_since(t0) < 100, "the rekey (the 2.3 s step of the report) returns at once");
    check(tr::world_key() == "abcd" && tr::file() == keyed, "the key and the file change at once");
    check(tr::flush(10000), "flush waits until the sidecar is on disk");
    check(g_test_disk_calls.load() <= 3, "waiting writes of the same file were merged (at most 3 writes for 3 adds + rekey)");
    check(!std::filesystem::exists(unknown), "the old sidecar is removed after the new one is written");
    const std::string text = read(keyed);
    check(text.find("\"world\": \"abcd\"") != std::string::npos, "the new sidecar has the new key");
    check(text.find("third") != std::string::npos && text.find("first") != std::string::npos, "and every record");
    check(!std::filesystem::exists(dir / "abcd.json.tmp"), "no .tmp left");

    t0 = std::chrono::steady_clock::now();
    std::vector<tr::Record> one{rec(9, "only")};
    tr::replace(std::move(one), true);
    check(ms_since(t0) < 100, "replace (the refresh's save) returns at once");
    t0 = std::chrono::steady_clock::now();
    tr::save_now();
    check(ms_since(t0) < 100, "save_now (the world's close) returns at once");

    // the next world session opens the same sidecar: it reads what was written last (open flushes first)
    tr::open(keyed, "abcd", "world8.ksd");
    check(tr::count() == 1, "open read the last written sidecar (one record)");
    tr::Record r;
    check(tr::at(0, &r) && r.name == "only" && !r.live && r.entity == 0, "the record as replace left it (not live yet)");

    g_test_disk_ms = 0;
    check(tr::flush(1000), "flush with nothing waiting");
    std::filesystem::remove_all(dir.parent_path(), ec);
    std::printf("tracker_test: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
