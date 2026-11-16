// SPDX-License-Identifier: MIT
// The production Find implementation, using a synthetic game image and exe file. No Windows code runs.
#include <greeneye/jumppatch.h>
#include <cctype>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>
#include "../src/original_code.h"

static std::vector<uint8_t> file, live;
static std::string scenario;
constexpr size_t raw = 0x400, rva = 0x1000, text_size = 0x1000, site = 0x80;
static int failed = 0, checks = 0;
static void check(bool ok, const char* what) {
    ++checks;
    std::printf("%s %s: %s\n", ok ? "PASS" : "FAIL", scenario.c_str(), what);
    failed += !ok;
}
template<class T> static void put(size_t off, T value) { std::memcpy(file.data() + off, &value, sizeof value); }

static const uint8_t dispatch[] = {0x4C,0x8B,0xDC,0x55,0x56,0x57,0x49,0x8D,0xAB,0x18,0xFF,0xFF,0xFF};
static const char* dispatch_pattern = "4C 8B DC 55 56 57 49 8D AB 18 FF FF FF";
static const uint8_t loop[] = {0x40,0x53,0x55,0x56,0x57,0x41,0x54,0x41,0x55,0x41,0x56,0x41,0x57,
    0x48,0x81,0xEC,0x98,0,0,0,0x48,0x8D,0x81,0x78,0x4F,0x03,0};
static const char* loop_pattern = "40 53 55 56 57 41 54 41 55 41 56 41 57 48 81 EC 98 00 00 00 48 8D 81 78 4F 03 00";

static void fixture() {
    file.assign(raw + text_size, 0);
    put<uint16_t>(0, 0x5A4D); put<uint32_t>(0x3C, 0x40); put<uint32_t>(0x40, 0x4550);
    put<uint16_t>(0x44, 0x8664); put<uint16_t>(0x46, 1); put<uint32_t>(0x48, 42); put<uint16_t>(0x54, 0xF0);
    put<uint16_t>(0x58, 0x20B); put<uint64_t>(0x58 + 24, 0x140000000);
    put<uint32_t>(0x58 + 56, 0x3000); put<uint32_t>(0x58 + 60, raw); put<uint32_t>(0x58 + 108, 16);
    std::memcpy(file.data() + 0x148, ".text", 5);
    put<uint32_t>(0x150, text_size); put<uint32_t>(0x154, rva);
    put<uint32_t>(0x158, text_size); put<uint32_t>(0x15C, raw);
    std::memset(file.data() + raw, 0xCC, text_size);
    std::memcpy(file.data() + raw + site, dispatch, sizeof dispatch);
    std::memcpy(file.data() + raw + 0x180, loop, sizeof loop);
    live.assign(file.begin() + raw, file.end());
}

namespace hot {
namespace {
void* GetModuleHandleW(const wchar_t*) { return reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(live.data()) - rva); }
bool TextSection(uint8_t*& start, size_t& size) { start = live.data(); size = live.size(); return true; }
void Read(const void* p, void* out, size_t n) { std::memcpy(out, p, n); }
bool ReadOriginalCode(std::vector<uint8_t>& out, std::string& why) {
    if (scenario == "unreadable") { why = "cannot open the exe file"; return false; }
    const code::Identity id{reinterpret_cast<uintptr_t>(live.data()) - rva, 42, 0x3000, rva, live.size()};
    return code::OriginalText(file.data(), file.size(), id, out, why);
}
#include "../src/find_code.h"
} // namespace
} // namespace hot

int main(int argc, char** argv) {
    scenario = argc > 1 ? argv[1] : "short-hook";
    fixture();
    if (scenario == "short-hook" || scenario == "rel32-hook" || scenario == "indirect-hook" || scenario == "duplicate") {
        std::memset(live.data() + site, 0x90, sizeof dispatch);
        if (scenario == "rel32-hook") live[site] = 0xE9;
        else if (scenario == "indirect-hook") { live[site] = 0xFF; live[site + 1] = 0x25; }
        else { live[site] = 0x48; live[site + 1] = 0xB8; live[site + 10] = 0xFF; live[site + 11] = 0xE0; }
        if (scenario == "duplicate") {
            std::memcpy(file.data() + raw + 0x280, dispatch, sizeof dispatch);
            std::memcpy(live.data() + 0x280, dispatch, sizeof dispatch);
        }
    }
    if (scenario == "inside-pattern") live[0x180 + 20] = 0xE9;
    if (scenario == "mismatch") put<uint32_t>(0x48, 43);
    if (scenario == "mismatch" || scenario == "unreadable") {
        const uint8_t fake[] = {0xDE,0xAD,0xBE,0xEF};
        std::memcpy(file.data() + raw + 0x380, fake, sizeof fake);
    }
    if (scenario == "truncated") file.resize(0x200);
    if (scenario == "fallback-hook") {
        live[0x180] = 0xE9;
        std::memset(live.data() + 0x181, 0x90, 12);
        file.clear();
    }
    if (scenario == "duplicate") check(hot::Find(dispatch_pattern, nullptr) == nullptr, "a hidden duplicate stays ambiguous");
    else check(hot::Find(dispatch_pattern, nullptr) == live.data() + site, "UiDispatch resolves to the live function");
    check(hot::Find(loop_pattern, nullptr) == live.data() + 0x180, "UiCommandLoop resolves to the live function");
    check(hot::Find("DE AD BE EF", nullptr) == nullptr, "a missing signature stays missing");
    check(hot::Find("GG", nullptr) == nullptr, "malformed pattern is refused");
    check(hot::Find("?? 8B", nullptr) == nullptr, "leading wildcard keeps the existing contract");
    check(hot::Find(dispatch_pattern, reinterpret_cast<void*>(1)) == nullptr, "another module is refused");
    if (scenario == "plain") {
        std::memset(live.data() + site, 0x90, sizeof dispatch);
        check(hot::Find(dispatch_pattern, hot::GetModuleHandleW(nullptr)) == live.data() + site, "the snapshot survives later hooks");
        check(hot::Find("4C 8B ?? 55 56 57 49 8D AB 18 FF FF FF", nullptr) == live.data() + site, "wildcards still match");
        auto identity = hot::code::Identity{0x150000000, 42, 0x3000, rva, text_size};
        std::vector<uint8_t> out;
        std::string why;
        for (unsigned field = 0; field < 4; ++field) {
            auto wrong = identity;
            if (field == 0) ++wrong.timestamp;
            if (field == 1) ++wrong.image_size;
            if (field == 2) ++wrong.text_rva;
            if (field == 3) ++wrong.text_size;
            check(!hot::code::OriginalText(file.data(), file.size(), wrong, out, why), "mismatched file metadata is refused");
        }
        put<uint32_t>(0x150, 0x20000001);
        put<uint32_t>(0x58 + 56, 0x30000000);
        identity.text_size = 0x20000001;
        identity.image_size = 0x30000000;
        check(!hot::code::OriginalText(file.data(), file.size(), identity, out, why) && why == "the exe code exceeds 512 MiB",
              "oversized virtual code is refused before allocation");
    }
    std::printf("%d checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
}
