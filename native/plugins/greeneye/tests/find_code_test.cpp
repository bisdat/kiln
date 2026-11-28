// SPDX-License-Identifier: MIT
// The production Find implementation, using a synthetic game image and exe file. No Windows code runs.
#include <greeneye/jumppatch.h>
#include <cctype>
#include <cstdio>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>
#include "../src/original_code.h"
#include "../src/start_notes.h"

static std::vector<uint8_t> file, live;
static std::string scenario;
constexpr size_t raw = 0x400, rva = 0x1000, text_size = 0x1000, site = 0x80;
static size_t mapped_size = text_size, image_size = 0x3000;
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
bool TextSection(uint8_t*& start, size_t& size) { start = live.data(); size = mapped_size; return true; }
bool ImageSpan(uintptr_t& base, size_t& size) { base = reinterpret_cast<uintptr_t>(live.data()) - rva; size = image_size; return true; }
bool ReadLive(const void* p, void* out, size_t n) { std::memcpy(out, p, n); return true; }
bool ReadPointer(uintptr_t at, uintptr_t* target) {
    const auto base = reinterpret_cast<uintptr_t>(live.data());
    if (at < base || at - base > live.size() || sizeof(*target) > live.size() - (at - base)) return false;
    std::memcpy(target, reinterpret_cast<const void*>(at), sizeof(*target));
    return true;
}
bool ReadOriginalCode(std::vector<uint8_t>& out, std::string& why) {
    if (scenario == "read-fault") throw std::runtime_error("mapped file read failed");
    if (scenario == "allocation-failure") throw std::bad_alloc();
    if (scenario == "unreadable" || scenario.rfind("kiln-view", 0) == 0) { why = "cannot open the exe file"; return false; }
    const code::Identity id{reinterpret_cast<uintptr_t>(live.data()) - rva, 42, static_cast<uint32_t>(image_size), rva, mapped_size};
    return code::OriginalText(file.data(), file.size(), id, live.data(), out, why, &ReadPointer);
}
// Kiln's verified view of the same exe (kiln.hooks read_original): the file's code at a live address, in bounded reads.
// Only "kiln-view" has one; elsewhere a failed own read keeps meaning no file code at all, as those scenarios test.
bool KilnView(uintptr_t at, void* out, size_t n) {
    const auto start = reinterpret_cast<uintptr_t>(live.data());
    if (scenario != "kiln-view" || n > 0x300 || at < start || at - start > mapped_size || n > mapped_size - (at - start))
        return false;
    std::memcpy(out, file.data() + raw + (at - start), n);
    return true;
}
StartNotes notes; // the starting mod's notes (hot.cpp: the generation starting on this thread)
StartNotes* CurrentNotes() { return &notes; }
bool ReadKilnCode(std::vector<uint8_t>& out, std::string& why) {
    const code::Identity id{reinterpret_cast<uintptr_t>(live.data()) - rva, 42, static_cast<uint32_t>(image_size), rva, mapped_size};
    return code::OriginalTextFrom(&KilnView, 0x300, id, live.data(), out, why, &ReadPointer);
}
#include "../src/find_code.h"
} // namespace
} // namespace hot

// kiln#157: what a mod's read returns for n bytes at text offset `at`.
static std::vector<uint8_t> mod_read(size_t at, size_t n) {
    std::vector<uint8_t> out(n, 0xA5);
    hot::Read(live.data() + at, out.data(), n);
    return out;
}
static bool same(const std::vector<uint8_t>& got, const uint8_t* want) { return std::memcmp(got.data(), want, got.size()) == 0; }
static void put_rel32(std::vector<uint8_t>& code, size_t at, size_t to) {
    const int32_t d = static_cast<int32_t>(static_cast<intptr_t>(to) - static_cast<intptr_t>(at + 5));
    std::memcpy(code.data() + at + 1, &d, 4);
}
// kiln#157: a caller (a "call rel32" to UiCommandLoop, 0x3380 bytes away: further than read's 4 KiB margin) and a global
// load ("mov rax, [rip+disp32]") in the file of a 16 KiB code section; a mod finds each by its pattern and reads the
// call's or the global's operand inside the match.
constexpr size_t caller = 0x3500, call = caller + 3, global = 0x600, read_text = 0x4000;
static const char* caller_pattern = "48 8B CB E8 ?? ?? ?? ?? 48 85 C0 74 05";
static const char* global_pattern = "48 8B 05 ?? ?? ?? ?? 48 85 C0 0F 84";
static void read_fixture() {
    file.resize(raw + read_text, 0xCC);
    mapped_size = read_text;
    image_size = rva + read_text + 0x1000;
    put<uint32_t>(0x150, read_text); put<uint32_t>(0x158, read_text); put<uint32_t>(0x58 + 56, static_cast<uint32_t>(image_size));
    std::vector<uint8_t> text(file.begin() + raw, file.end());
    const uint8_t calls[] = {0x48,0x8B,0xCB,0xE8,0,0,0,0,0x48,0x85,0xC0,0x74,0x05};
    std::memcpy(text.data() + caller, calls, sizeof calls);
    put_rel32(text, call, 0x180);
    const uint8_t load[] = {0x48,0x8B,0x05,0x78,0x56,0x34,0x02,0x48,0x85,0xC0,0x0F,0x84,0x10,0,0,0};
    std::memcpy(text.data() + global, load, sizeof load);
    std::copy(text.begin(), text.end(), file.begin() + raw);
    live.assign(file.begin() + raw, file.end());
}
// Another module's hooks over those operands, as hosted ShroudForge and Kiln's own hooks write them: the call sent to
// a relay outside the image (its E8 kept), and a jump to outside the image with nops over the 7-byte instruction.
static void hook_operands() {
    put_rel32(live, call, read_text + 0x10000);
    live[global] = 0xE9;
    put_rel32(live, global, read_text + 0x20000);
    live[global + 5] = live[global + 6] = 0x90;
}

int main(int argc, char** argv) {
    scenario = argc > 1 ? argv[1] : "short-hook";
    fixture();
    const bool reads = scenario.rfind("read-", 0) == 0 && scenario != "read-fault";
    if (reads) read_fixture();
    if (scenario == "read-hooked" || scenario == "read-first" || scenario == "read-dense" || scenario == "read-notes")
        hook_operands();
    if (scenario == "read-first") {
        // A mod may read before its first find: the read makes the code find matches.
        check(same(mod_read(call + 1, 4), file.data() + raw + call + 1), "a read before any find sees the file's call operand");
    }
    if (scenario == "zero-tail") {
        file.resize(raw + 0x800);
        put<uint32_t>(0x158, 0x800);
        std::memset(live.data() + 0x800, 0, text_size - 0x800);
    }
    if (scenario == "raw-tail") {
        mapped_size = 0x800;
        put<uint32_t>(0x150, mapped_size);
        const uint8_t fake[] = {0xDE,0xAD,0xBE,0xEF};
        std::memcpy(file.data() + raw + 0x900, fake, sizeof fake);
    }
    if (scenario == "relocations") {
        put<uint32_t>(0x58 + 112 + 5 * 8, rva + 0x700);
        put<uint32_t>(0x58 + 112 + 5 * 8 + 4, 12);
        put<uint32_t>(raw + 0x700, rva);
        put<uint32_t>(raw + 0x704, 12);
        put<uint16_t>(raw + 0x708, 0xA600);
        put<uint16_t>(raw + 0x70A, 0x3608);
        put<uint64_t>(raw + 0x600, 0x140000033);
        put<uint32_t>(raw + 0x608, 0x40000088);
        std::memcpy(live.data(), file.data() + raw, text_size);
        const uint64_t actual = reinterpret_cast<uintptr_t>(live.data()) - rva;
        const uint64_t relocated64 = actual + 0x33;
        const uint32_t relocated32 = static_cast<uint32_t>(actual + 0x88);
        std::memcpy(live.data() + 0x600, &relocated64, sizeof relocated64);
        std::memcpy(live.data() + 0x608, &relocated32, sizeof relocated32);
    }
    if (scenario == "short-hook" || scenario == "rel32-hook" || scenario == "indirect-hook" || scenario == "duplicate" ||
        scenario == "mixed-patches") {
        std::memset(live.data() + site, 0x90, sizeof dispatch);
        if (scenario == "rel32-hook") live[site] = 0xE9;
        else if (scenario == "indirect-hook") {
            live[site] = 0xFF; live[site + 1] = 0x25;
            std::memset(live.data() + site + 2, 0, 4);
        }
        else { live[site] = 0x48; live[site + 1] = 0xB8; live[site + 10] = 0xFF; live[site + 11] = 0xE0; }
        if (scenario == "duplicate") {
            std::memcpy(file.data() + raw + 0x280, dispatch, sizeof dispatch);
            std::memcpy(live.data() + 0x280, dispatch, sizeof dispatch);
        }
    }
    if (scenario == "r11-hook") {
        const uintptr_t target = 1;
        live[site] = 0x49; live[site + 1] = 0xBB;
        std::memcpy(live.data() + site + 2, &target, 8);
        live[site + 10] = 0x41; live[site + 11] = 0xFF; live[site + 12] = 0xE3;
    }
    if (scenario == "inside-pattern" || scenario == "kiln-view") live[0x180 + 20] = 0xE9;
    if (scenario == "kiln-view") { // the plugin cannot open the exe (kiln#157's first suspect), and the code is hooked
        std::memset(live.data() + site, 0x90, sizeof dispatch);
        live[site] = 0x48; live[site + 1] = 0xB8; live[site + 10] = 0xFF; live[site + 11] = 0xE0;
    }
    if (scenario == "mixed-patches") live[0x380] = 0xCD;
    if (scenario == "indirect-slot" || scenario == "unreadable-slot" || scenario == "inside-image-slot") {
        // A six-byte RIP-indirect hook can keep its destination outside .text, rather than inline at +6.
        live.resize(text_size + 0x100, 0);
        std::memset(live.data() + site, 0x90, sizeof dispatch);
        live[site] = 0xFF; live[site + 1] = 0x25;
        const int32_t displacement = scenario == "unreadable-slot" ? 0x100000 : text_size + 0x10 - (site + 6);
        const uintptr_t destination = scenario == "inside-image-slot" ? reinterpret_cast<uintptr_t>(live.data()) + 0x300 : 1;
        std::memcpy(live.data() + site + 2, &displacement, 4);
        std::memcpy(live.data() + text_size + 0x10, &destination, sizeof destination);
    }
    if (scenario == "mismatch") put<uint32_t>(0x48, 43);
    if (scenario == "mismatch" || scenario == "unreadable" || scenario == "changed-file") {
        const uint8_t fake[] = {0xDE,0xAD,0xBE,0xEF};
        std::memcpy(file.data() + raw + 0x380, fake, sizeof fake);
    }
    if (scenario == "live-patch") live[site + 4] = 0x54;
    if (scenario == "truncated") file.resize(0x200);
    if (scenario == "fallback-hook") {
        live[0x180] = 0xE9;
        std::memset(live.data() + 0x181, 0x90, 12);
        file.clear();
    }
    // An acquisition exception must fall back just like a failed open, including outside a mod's SafeStart.
    try { hot::Find("GG", nullptr); }
    catch (...) {
        check(false, "an exe read failure must not escape Find");
        return 1;
    }
    if (scenario == "unreadable-slot" || scenario == "inside-image-slot") {
        check(hot::Find(dispatch_pattern, nullptr) == nullptr, "an unverified indirect jump cannot use file code");
        check(std::memcmp(hot::g_canonical.data() + site, live.data() + site, sizeof dispatch) == 0,
              "an unreadable or in-image jump keeps its live bytes");
    }
    else if (scenario == "live-patch") {
        check(hot::Find(dispatch_pattern, nullptr) == nullptr, "an in-place patch cannot resolve the stale file signature");
        check(hot::Find("4C 8B DC 55 54 57 49 8D AB 18 FF FF FF", nullptr) == live.data() + site,
              "a signature in the patched live code remains available");
    }
    else if (scenario == "duplicate") check(hot::Find(dispatch_pattern, nullptr) == nullptr, "a hidden duplicate stays ambiguous");
    else check(hot::Find(dispatch_pattern, nullptr) == live.data() + site, "UiDispatch resolves to the live function");
    check(hot::Find(loop_pattern, nullptr) == live.data() + 0x180, "UiCommandLoop resolves to the live function");
    check(hot::Find("DE AD BE EF", nullptr) == nullptr, "a missing signature stays missing");
    check(hot::Find("GG", nullptr) == nullptr, "malformed pattern is refused");
    check(hot::Find("?? 8B", nullptr) == nullptr, "leading wildcard keeps the existing contract");
    check(hot::Find(dispatch_pattern, reinterpret_cast<void*>(1)) == nullptr, "another module is refused");
    if (scenario == "zero-tail" || scenario == "raw-tail" || scenario == "relocations") {
        check(hot::g_codeFromFile && hot::g_canonical.size() == mapped_size,
              "the file snapshot covers VirtualSize");
        check(std::memcmp(hot::g_canonical.data(), live.data(), mapped_size) == 0,
              "zero-fill, raw padding and ASLR agree with the loaded code");
    }
    if (scenario == "relocations") {
        std::string pattern;
        for (size_t i = 0; i < 12; ++i) {
            char byte[4];
            std::snprintf(byte, sizeof byte, "%02X ", live[0x600 + i]);
            pattern += byte;
        }
        check(hot::Find(pattern.c_str(), nullptr) == live.data() + 0x600,
              "DIR64 and HIGHLOW operands resolve at the loaded base");
    }
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
            check(!hot::code::OriginalText(file.data(), file.size(), wrong, live.data(), out, why), "mismatched file metadata is refused");
        }
        put<uint32_t>(0x150, 0x20000001);
        put<uint32_t>(0x58 + 56, 0x30000000);
        identity.text_size = 0x20000001;
        identity.image_size = 0x30000000;
        check(!hot::code::OriginalText(file.data(), file.size(), identity, live.data(), out, why) && why == "the exe code exceeds 512 MiB",
              "oversized virtual code is refused before allocation");
        file.resize(raw + 0x80000, 0xCC);
        put<uint32_t>(0x150, 0x80000);
        put<uint32_t>(0x158, 0x80000);
        put<uint32_t>(0x58 + 56, 0x82000);
        live.assign(file.begin() + raw, file.end());
        for (size_t i = 0; i < 4097; ++i) live[0x400 + i * 2] = 0xCD;
        identity = {reinterpret_cast<uintptr_t>(live.data()) - rva, 42, 0x82000, rva, 0x80000};
        check(!hot::code::OriginalText(file.data(), file.size(), identity, live.data(), out, why) &&
              why == "too many exe code changes", "fragmented live changes have bounded metadata");
        std::memset(live.data(), 0xCD, 0x10000);
        check(!hot::code::OriginalText(file.data(), file.size(), identity, live.data(), out, why),
              "dense file/live changes select the previous live snapshot");
    }
    if (reads) {
        const uint8_t* orig = file.data() + raw;
        check(hot::Find(caller_pattern, nullptr) == live.data() + caller, "the caller's signature resolves");
        check(hot::Find(global_pattern, nullptr) == live.data() + global, "the global load's signature resolves");
        if (scenario == "read-later") hook_operands(); // hooks written after the code was read: Kiln's later ones
        if (scenario == "read-dense") {
            // more than 1% of the code around the read changed in place: not sorted, the live bytes as before
            for (size_t i = 0; i < 128; ++i) live[caller + 0x100 + i * 2] ^= 0x5A;
            check(same(mod_read(call, 5), live.data() + call), "a heavily patched place reads live");
        } else {
            check(same(mod_read(call, 5), orig + call), "the call's operand reads as the file has it, not the relay's");
            check(same(mod_read(caller, 13), orig + caller), "the whole caller reads as find matched it");
            check(same(mod_read(global + 3, 4), orig + global + 3), "the global's displacement reads as the file has it");
            check(same(mod_read(global - 2, 20), orig + global - 2), "a read across the jump and its nops reads the file's");
            live[global + 11] = 0x85; // an in-place patch next to the hook (jz -> jnz): still the live byte
            const auto mixed = mod_read(global, 12);
            check(same(std::vector<uint8_t>(mixed.begin(), mixed.begin() + 11), orig + global) && mixed[11] == 0x85,
                  "an in-place patch beside a hook stays visible");
        }
    }
    if (scenario == "kiln-view") {
        check(hot::g_codeFromFile, "Kiln's view of the exe file stands in for the plugin's own read");
        check(same(mod_read(site, sizeof dispatch), dispatch), "a read under a hook returns the file's bytes from Kiln's view");
        check(hot::CodeSource() == "exe file (Kiln's view; the plugin's own read: cannot open the exe file)",
              "kiln.log names Kiln's view and why the plugin's own read failed");
    }
    if (scenario == "kiln-view-none") {
        check(!hot::g_codeFromFile && mod_read(0x380, 1)[0] == 0xCC, "without either view the code is the live snapshot");
        check(hot::CodeSource() == "live: cannot open the exe file; Kiln's view of the exe: Kiln has no view of the exe file's code",
              "kiln.log names both reasons");
    }
    if (scenario == "plain") check(hot::CodeSource() == "exe file", "kiln.log says the code is the exe file's");
    if (scenario == "read-notes") {
        hot::notes = {};
        hot::Find(caller_pattern, nullptr);
        hot::Find("DE AD BE EF 01", nullptr);
        hot::Find(global_pattern, nullptr);
        mod_read(call, 5);
        mod_read(global + 3, 4);
        mod_read(0x180, 8); // a read of unhooked code is no note
        check(hot::notes.finds == 3 && hot::notes.missed == 1 && hot::notes.missedPatterns.size() == 1 &&
              hot::notes.missedPatterns[0] == "DE AD BE EF 01", "find's answers are noted");
        // the first byte that read differently: the call's operand (its E8 stayed), the displacement
        check(hot::notes.unhooked == 2 && hot::notes.unhookedRvas.size() == 2 && hot::notes.unhookedRvas[0] == rva + call + 1 &&
              hot::notes.unhookedRvas[1] == rva + global + 3, "reads under another module's hook are noted with the RVA");
        check(hot::notes.Line(false, "exe file", true) ==
              "while starting it asked find for 3 signatures in the game's code (exe file); not found: 1 (\"DE AD BE EF 01\"); "
              "2 reads returned the game's own bytes under another module's hook (rva 0x4504, 0x1603)",
              "the line for a mod that did not start names the signature it missed");
        check(hot::notes.Line(true, "exe file", false) ==
              "2 reads returned the game's own bytes under another module's hook (rva 0x4504, 0x1603)",
              "the line for a started mod names only the reads");
        hot::StartNotes all_found;
        all_found.Find("48 8B", true);
        check(all_found.Line(false, "live: x", true) == "while starting it asked find for 1 signature in the game's code (live: x), "
              "and every one was found; so it declined for a reason of its own (see its lines above)",
              "a mod that found everything and declined says so");
        // A mod that crashed, had a hook refused or failed its commit did not decline: the outcome after the line says why.
        check(all_found.Line(false, "live: x", false) == "while starting it asked find for 1 signature in the game's code (live: x), "
              "and every one was found",
              "a mod that found everything and failed for another reason is not said to have declined");
        // Without a find the code was never read: the line names no source (CodeSource would say "live: " then).
        check(hot::StartNotes{}.Line(false, "live: ", true) ==
              "while starting it asked find for no signatures; so it declined for a reason of its own (see its lines above)",
              "a mod that asked find for nothing names no code source");
        check(hot::StartNotes{}.Line(false, "live: ", false).empty(),
              "a mod that failed before asking anything (no GreeneyeModStart, an early crash) gets no line");
        check(hot::StartNotes{}.Line(true, "exe file", false).empty(), "a started mod without such reads gets no line");
    }
    if (scenario == "live-patch")
        check(mod_read(site + 4, 1)[0] == 0x54, "a read of an in-place patch returns the patched byte");
    if (scenario == "unreadable")
        check(mod_read(0x380, 4)[0] == 0xCC, "without the file's code a read returns the live bytes");
    if (scenario == "short-hook" || scenario == "rel32-hook" || scenario == "indirect-hook")
        check(same(mod_read(site, sizeof dispatch), dispatch), "a function another module hooked reads as find matched it");
    std::printf("%d checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
}
