// SPDX-License-Identifier: MIT
// Native Windows test of the save-region hook's guarded paths (regions.cpp, after the game's builder returned): a list
// whose entries fault when read, whose grow faults while Kiln appends, and whose header itself faults. Pages come from
// VirtualAlloc, a fault is a real access violation caught by the structured-exception guards the service uses.
// Built explicitly (CMake target build-regions-win, into bin/tests/); never a game target or a bundle file.
#include <windows.h>

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
bool grow_into_fresh(kb::GameArray* a, int64_t neg_required, uint8_t) {  // a new buffer; the old entries are not copied
    static std::vector<uint8_t> fresh;
    fresh.assign(static_cast<size_t>(-neg_required) * 0x30, 0);
    a->data = fresh.data();
    a->capacity = static_cast<uint64_t>(-neg_required);
    return true;
}
bool grow_faults(kb::GameArray*, int64_t, uint8_t) {
    *reinterpret_cast<volatile uint8_t*>(g_noaccess) = 1;  // the game's grow faulting while Kiln appends
    return true;
}
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
        kb::after_save_regions(0x2000, ok, &a, sites);
        kb::after_save_regions(0x3000, ok, &a, sites);
        check(a.count == 1 && a.data == g_noaccess, "base zones / base query: the list is not read or changed");
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
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    VirtualFree(g_noaccess, 0, MEM_RELEASE);
    std::printf("regions-win: %d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
