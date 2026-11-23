// SPDX-License-Identifier: MIT
// Native Windows test of the save-region hook's guarded paths (regions.cpp, after the game's builder returned): a list
// whose entries fault when read, whose grow faults while Kiln appends, and whose header itself faults. Pages come from
// VirtualAlloc, a fault is a real access violation caught by the structured-exception guards the service uses.
// Built explicitly (CMake target build-regions-win, into bin/tests/); never a game target or a bundle file.
#include <windows.h>

#include <intrin.h>

#include <cstdio>
#include <filesystem>
#include <string>
#define KILN_REGIONS_SUPPORT_STUBS
#include "regions_test_support.h"
#undef KILN_REGIONS_SUPPORT_STUBS
#include "../regions.cpp"
#include "regions_test_support.h"

namespace {
int passed = 0, failed = 0;
void check(bool ok, const char* text) {
    if (ok) ++passed;
    else { ++failed; std::fprintf(stderr, "FAIL: %s\n", text); }
}
uint8_t* g_noaccess = nullptr;  // one page nobody may touch
// A new buffer with room for 64 entries, once: the old (unreadable) entries are not copied, Kiln's appends stay.
bool grow_into_fresh(kb::GameArray* a, int64_t neg_required, uint8_t) {
    static std::vector<uint8_t> fresh;
    const uint64_t need = static_cast<uint64_t>(-neg_required);
    if (a->data == fresh.data() && need <= a->capacity) return true;
    if (need > 64) return false;
    fresh.assign(64 * 0x30, 0);
    a->data = fresh.data();
    a->capacity = 64;
    return true;
}
int g_grow_calls = 0;
bool grow_faults(kb::GameArray*, int64_t, uint8_t) {
    ++g_grow_calls;
    *reinterpret_cast<volatile uint8_t*>(g_noaccess) = 1;  // the game's grow faulting while Kiln appends
    return true;
}

// ---- placing a call by walking the real stack (kb::walk_from: RtlCaptureStackBackTrace, RtlPcToFileHeader). This exe
// stands in for the game; msvcrt.dll's qsort, calling back into it, stands in for another mod's DLL between a "game"
// caller and the hook. fake_hook plays Kiln's hook: its return address is `ret`.
using QsortFn = void(__cdecl*)(void*, size_t, size_t, int(__cdecl*)(const void*, const void*));
kb::CallSites g_probe_sites;
std::vector<kb::Frame> g_probe_frames;
kb::CallerVerdict g_probe;
volatile int g_sink = 0;
__declspec(noinline) void fake_hook() {
    const uintptr_t ret = reinterpret_cast<uintptr_t>(_ReturnAddress());
    g_probe_frames = kb::walk_from(ret);
    g_probe = kb::judge_caller(ret, g_probe_frames, g_probe_sites);
    g_sink = g_sink + 1;
}
int __cdecl compare_ints(const void* a, const void* b) {
    fake_hook();
    return *static_cast<const int*>(a) - *static_cast<const int*>(b);
}
__declspec(noinline) void fake_world_save(QsortFn sort) {
    int v[2] = {2, 1};
    sort(v, 2, sizeof(int), &compare_ints);
    g_sink = g_sink + v[0];
}
__declspec(noinline) void fake_base_zones(QsortFn sort) {
    int v[2] = {2, 1};
    sort(v, 2, sizeof(int), &compare_ints);
    g_sink = g_sink + v[1];
}
__declspec(noinline) int game_depth(int n) {
    if (n == 0) {
        fake_hook();
        return g_sink;
    }
    const int r = game_depth(n - 1);
    return r + g_sink;
}
// The return address among the walked frames that lies in `fn` (by its unwind data's function start): the call site
// of the caller fn, as sigdb would resolve it for the game.
uintptr_t site_in(const std::vector<kb::Frame>& frames, const void* fn) {
    for (const auto& f : frames) {
        DWORD64 base = 0;
        const RUNTIME_FUNCTION* rf = RtlLookupFunctionEntry(f.at, &base, nullptr);
        if (rf && base + rf->BeginAddress == reinterpret_cast<DWORD64>(fn)) return f.at;
    }
    return 0;
}
bool named(const kb::Frame& f, const char* module) { return _stricmp(f.module.c_str(), module) == 0; }
}  // namespace

int main() {
    g_noaccess = static_cast<uint8_t*>(VirtualAlloc(nullptr, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_NOACCESS));
    if (!g_noaccess) return 2;
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / ("kiln-regions-win-" + std::to_string(GetCurrentProcessId()));
    std::filesystem::create_directories(dir);
    kb::g_dir = dir;
    kb::regions_lifecycle_ready(true, true);
    kb::F.persist = true;
    kb::identity = "WIN";
    kb::regions_flush();
    const std::set<std::array<int32_t, 3>> kept{{0, 0, 0}, {9, 0, 0}, {40, 0, 40}};
    kb::g_chunks = kept;
    kb::g_dirty = kb::g_boxes_stale = true;
    const kb::CallSites sites{0x1000, 0x2000, 0x3000};
    const uint8_t ok[8] = {0};
    const uint64_t all = kb::boxes_for_chunks(kept).size();

    {   // the entries fault when read: every kept chunk gets a box, the game's own entry is not touched
        kb::GameArray a{g_noaccess, 1, 1, &grow_into_fresh};
        kb::after_save_regions(0x1000, ok, &a, sites);
        check(a.count == 1 + all, "unreadable entries: Kiln boxes for every kept chunk, appended after the game's");
        bool ids = true;
        for (uint64_t i = 1; i < a.count; ++i) ids = ids && regions_test::is_kiln(a.data + i * 0x30);
        check(ids, "unreadable entries: the appended entries are whole KILNBILD boxes");
    }
    {   // grow faults while Kiln appends: the list keeps the game's count, nothing half-written reaches the save
        std::vector<uint8_t> store;
        regions_test::FakeList l = regions_test::make_list(store);
        const float lo[3] = {0.f, 0.f, 0.f}, hi[3] = {120.f, 120.f, 120.f};
        regions_test::add_entry(l, "REAL-ALTAR-BASE1", lo, hi);
        l.a.grow = &grow_faults;
        const int errors = kb::errors;
        kb::after_save_regions(0x1000, ok, &l.a, sites);
        check(l.a.count == 1 && std::memcmp(l.a.data, "REAL-ALTAR-BASE1", 16) == 0,
              "a faulting grow: the game's list is back to its own entry");
        check(kb::errors == errors + 1, "a faulting grow is reported once");
    }
    {   // the list header itself faults: the hook leaves it alone and does not fault
        const int errors = kb::errors;
        kb::after_save_regions(0x1000, ok, reinterpret_cast<kb::GameArray*>(g_noaccess), sites);
        check(true, "an unreadable list header does not fault the game's save");
        check(kb::errors == errors + 1, "an unreadable list header is reported");
    }
    {   // the base zones the game's systems use are never read or changed, even when they would fault
        kb::GameArray a{g_noaccess, 1, 1, &grow_faults};
        const int errors = kb::errors, warnings = kb::warnings, grows = g_grow_calls;
        kb::after_save_regions(0x2000, ok, &a, sites);
        kb::after_save_regions(0x3000, ok, &a, sites);
        check(a.count == 1 && a.data == g_noaccess, "base zones / base query: the list is not read or changed");
        check(g_grow_calls == grows && kb::errors == errors && kb::warnings == warnings,
              "base zones / base query: no grow, no fault, nothing reported");
    }
    {   // the ordinary save, natively
        std::vector<uint8_t> store;
        regions_test::FakeList l = regions_test::make_list(store);
        const float lo[3] = {0.f, 0.f, 0.f}, hi[3] = {120.f, 120.f, 120.f};
        regions_test::add_entry(l, "REAL-ALTAR-BASE1", lo, hi);
        kb::after_save_regions(0x1000, ok, &l.a, sites);
        check(l.a.count == 3 && regions_test::every_chunk_saved(l.a, kept, true),
              "world save: boxes only for the two chunks the altar does not save");
    }
    {   // a foreign module between the world save's call and the hook: placed by the world save's site found deeper
        HMODULE crt = LoadLibraryW(L"msvcrt.dll");
        const auto sort = crt ? reinterpret_cast<QsortFn>(GetProcAddress(crt, "qsort")) : nullptr;
        check(sort != nullptr, "msvcrt.dll's qsort is there to stand in for another module");
        if (sort) {
            g_probe_sites = kb::CallSites{0x10, 0, 0};  // the world save's site resolved, elsewhere
            fake_world_save(sort);
            const uintptr_t save_site = site_in(g_probe_frames, reinterpret_cast<const void*>(&fake_world_save));
            bool crt_frame = false;
            for (const auto& f : g_probe_frames) crt_frame = crt_frame || (f.place == kb::Place::Other && named(f, "msvcrt.dll"));
            check(g_probe_frames.size() >= 3 && g_probe_frames[0].place == kb::Place::Game && crt_frame && save_site,
                  "the walk goes from the hook's return address through msvcrt.dll's frames to the caller's call site");
            check(g_probe.how == kb::Found::Foreign && g_probe.boxes && named(g_probe.via, "msvcrt.dll"),
                  "no known site on the stack, another module on the way: Foreign, named, boxes");
            g_probe_sites = kb::CallSites{save_site, 0x20, 0x30};
            fake_world_save(sort);
            check(g_probe.kind == kb::Caller::WorldSave && g_probe.how == kb::Found::Deeper && g_probe.boxes,
                  "the world save's site found below another module's frames: the world save, boxes");
            g_probe_sites = kb::CallSites{0x10, 0, 0};
            fake_base_zones(sort);
            const uintptr_t zones_site = site_in(g_probe_frames, reinterpret_cast<const void*>(&fake_base_zones));
            g_probe_sites = kb::CallSites{save_site, zones_site, 0x30};
            fake_base_zones(sort);
            check(zones_site && g_probe.kind == kb::Caller::BaseZones && g_probe.how == kb::Found::Deeper && !g_probe.boxes,
                  "the base-zone site found below another module's frames: the base zones, no boxes");
        }
        // the game's own frames only, deeper than kWalkFrames: a caller Kiln does not know, no boxes
        g_probe_sites = kb::CallSites{0x10, 0x20, 0x30};
        game_depth(12);
        bool all_game = g_probe_frames.size() == kb::kWalkFrames;
        for (const auto& f : g_probe_frames) all_game = all_game && f.place == kb::Place::Game;
        check(all_game && g_probe.how == kb::Found::GameOnly && !g_probe.boxes,
              "eight frames of the game's own code and no known site: GameOnly, no boxes");
        // a return address the walk never reaches: nothing past it, the boxes
        const auto lone = kb::walk_from(0x1234);
        const auto v = kb::judge_caller(0x1234, lone, g_probe_sites);
        check(lone.size() == 1 && lone[0].place == kb::Place::Other && v.boxes &&
                  (v.how == kb::Found::Foreign || v.how == kb::Found::Unwalked),
              "a return address that is not on the stack: only itself, the boxes");
        const auto none = kb::walk_from(0);
        check(none.empty() && kb::judge_caller(0, none, g_probe_sites).how == kb::Found::Unwalked &&
                  kb::judge_caller(0, none, g_probe_sites).boxes,
              "no return address at all: Unwalked, the boxes");
    }
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    VirtualFree(g_noaccess, 0, MEM_RELEASE);
    std::printf("regions-win: %d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
