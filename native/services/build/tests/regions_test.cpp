// SPDX-License-Identifier: MIT
// Compile the actual region bookkeeping, replacing only the game adapter and atomic file replacement.
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>
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
std::string bytes(const std::string& world) {
    std::ifstream f(kb::g_dir / (world + ".json"), std::ios::binary);
    return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}
void bind(const std::string& world) { kb::identity = world; kb::regions_flush(); }
void keep(int32_t x) {
    const int32_t origin[3] = {x, 0, 0};
    const uint32_t dims[3] = {1, 1, 1};
    kb::regions_keep(origin, dims);
}
std::string expected(const std::string& world, const std::string& chunks) {
    return "{\n  \"format\": 1,\n  \"world\": \"" + world + "\",\n  \"chunk_cells\": 32,\n  \"chunks\": [" + chunks + "]\n}\n";
}
}
int main() {
    char dir[] = "/tmp/kiln-regions-XXXXXX";
    if (!mkdtemp(dir)) return 2;
    kb::g_dir = dir;
    bind("A"); keep(0); kb::regions_flush();
    const auto before = bytes("A");
    check(before == expected("A", "0,0,0"), "known identity start retains the existing file format");
    bind("");  // A closed; no identity for B yet
    keep(64); kb::regions_flush();
    check(bytes("A") == before, "B's pending kept chunk must not change A's file");
    check(kb::g_world.empty() && kb::g_file.empty(), "unknown identity unbinds both world and path");
    check(kb::g_chunks == std::set<std::array<int32_t, 3>>{{2,0,0}}, "only B's pending chunk remains in memory");
    bind("");  // repeated polling must retain pending chunks
    bind("B");  // B's first save learns its identity
    check(bytes("B") == expected("B", "2,0,0"), "B's first known-identity flush writes B's pending chunk");
    bind(""); bind("A");
    check(bytes("A") == before, "A's file is byte-identical after B's entire session");
    check(kb::g_chunks == std::set<std::array<int32_t, 3>>{{0,0,0}}, "reopened A loads only A's chunk");
    bind(""); bind("A");
    check(kb::g_chunks.count({0,0,0}) == 1, "closing and reopening the same world retains its chunks");
    // Direct known-identity switch must flush dirty A, then load B without importing A.
    keep(32); bind("B");
    check(bytes("A") == expected("A", "0,0,0,1,0,0"), "changed identity flushes dirty earlier world");
    check(kb::g_chunks == std::set<std::array<int32_t, 3>>{{2,0,0}}, "known-identity switch isolates chunk sets");
    check(kb::boxes_locked().size() == 1, "loaded chunk still supplies a save box");
    // A close event must flush even when kiln.saves still reports the old GUID.
    keep(96); kb::close_locked();
    check(bytes("B") == expected("B", "2,0,0,3,0,0"), "world close flushes dirty chunks before unbinding");
    check(kb::g_world.empty() && kb::g_file.empty() && kb::g_chunks.empty(), "world close clears the binding");
    bind(""); keep(128); kb::close_locked();  // a world closed before its first save
    bind("A");
    check(kb::g_chunks.count({4,0,0}) == 0, "an unidentified world's close discards only its pending chunks");
    KilnWorldEvent stop{KILN_WORLD_SERVER, KILN_WORLD_STOPPING};
    kb::on_world(&stop, nullptr);
    check(kb::voxel_closes == 1, "world stop invalidates cached voxel pointers even if the next world reuses its address");
    check(kb::transient_closes == 1, "world stop unbinds the journal even when no menu server tick occurs");
    check(kb::block_closes == 1, "world stop discards accumulated terrain damage even without a menu tick");
    kb::regions_lifecycle_ready(false, true);
    check(!kb::F.persist, "subscription failure disables persistent writes");
    kb::F.persist = true;
    kb::regions_lifecycle_ready(true, false);
    check(!kb::F.persist, "an installed subscriber without the core stop hook disables persistent writes");
    // Reproduce the important geometry from the user's 120 m-altar case without committing
    // any private world-save data. The serializer keeps, of each 16 m chunk a base's bounds touch, only the 4 m blocks
    // the bounds touch: a chunk on the base's edge that reaches outside it is saved only in part by the base.
    const kb::RegionBounds altar{{0.f, 0.f, 0.f}, {120.f, 120.f, 120.f}};
    const std::set<std::array<int32_t, 3>> sample{
        {-1, 0, 0}, {0, 0, 0}, {7, 0, 0}, {7, 7, 7},
        {8, 0, 0}, {0, 8, 0}, {8, 8, 8}
    };
    const auto cover = kb::classify_chunks(sample, {altar});
    check(cover.overlaps_base == 1 && cover.outside_bases == 6,
          "120 m altar: only the chunk wholly inside it is saved by the base; its edge chunks and the off-base ones are not");
    check(kb::classify_chunks(sample, {}).outside_bases == sample.size(),
          "without any genuine base, all kept chunks require external coverage");
    const kb::RegionBounds edge{{0.f, 0.f, 0.f}, {128.f, 128.f, 128.f}};
    check(kb::chunk_touches_region({8, 0, 0}, edge),
          "a chunk touching a base region at exactly 128 m counts as overlapping");
    check(kb::classify_chunks({{8, 0, 0}}, {edge}).outside_bases == 1,
          "a chunk whose first 4 m block only touches a base's edge is not saved by that base");
    check(kb::classify_chunks({{7, 7, 7}}, {kb::RegionBounds{{0.f, 0.f, 0.f}, {124.f, 124.f, 124.f}}}).overlaps_base == 1,
          "a chunk whose last 4 m block touches the base's edge is saved whole (the serializer counts touching edges)");
    const kb::RegionBounds second{{130.f, 0.f, 0.f}, {160.f, 160.f, 160.f}};
    check(kb::classify_chunks({{8, 0, 0}}, {altar, second}).overlaps_base == 1,
          "a chunk saved by either of two bases is not classified as off-base");
    const kb::RegionBounds left{{0.f, 0.f, 0.f}, {130.f, 120.f, 120.f}}, right{{134.f, 0.f, 0.f}, {300.f, 120.f, 120.f}};
    check(kb::classify_chunks({{8, 0, 0}}, {left, right}).overlaps_base == 1,
          "a chunk two bases save block by block between them needs no box");
    check(kb::classify_chunks({{8, 0, 0}}, {left}).outside_bases == 1 && kb::classify_chunks({{8, 0, 0}}, {right}).outside_bases == 1,
          "either of those two bases alone saves that chunk only in part");
    const kb::RegionBounds invalid{{12.f, 0.f, 0.f}, {0.f, 16.f, 16.f}};
    check(!kb::chunk_touches_region({0, 0, 0}, invalid),
          "inverted base bounds are never treated as verified coverage");

    // The controlled reproduction's altar: 40 m bounds right after the world loads, 120 m once its level applies.
    const kb::RegionBounds startup{{40.f, 40.f, 40.f}, {80.f, 80.f, 80.f}};
    const kb::RegionBounds settled{{0.f, 0.f, 0.f}, {120.f, 120.f, 120.f}};
    const kb::ChunkSet protected_chunks{{0, 0, 0}, {6, 6, 6}};
    check(kb::chunks_outside_regions(protected_chunks, {settled}).empty(),
          "all native-covered kept chunks can be omitted from synthetic injection");
    check(kb::boxes_for_chunks(kb::chunks_outside_regions(protected_chunks, {settled})).empty(),
          "fully covered world serializes zero synthetic boxes");

    const kb::ChunkSet boundary_mix{{0, 0, 0}, {7, 0, 0}, {8, 0, 0}, {9, 0, 0}};
    const auto uncovered = kb::chunks_outside_regions(boundary_mix, {settled});
    check(uncovered.size() == 3 && uncovered.count({7, 0, 0}) && uncovered.count({8, 0, 0}) && uncovered.count({9, 0, 0}),
          "the altar's edge chunk reaching outside it and the off-base chunks remain in extra-save coverage");
    check(kb::boxes_for_chunks(uncovered).size() == 1,
          "contiguous chunks that need coverage are serialized as a single synthetic box");
    check(kb::chunks_outside_regions(boundary_mix, {}).size() == boundary_mix.size(),
          "without a native altar all kept chunks retain extra coverage");
    check(kb::chunks_outside_regions(boundary_mix, {startup}).size() >=
              kb::chunks_outside_regions(boundary_mix, {settled}).size(),
          "shrinking an altar can increase the off-base coverage requirement");

    // Check actual persisted bytes in a bound test world, not an unbound in-memory set.
    kb::F.persist = true;
    bind("FILTER");
    kb::g_chunks = boundary_mix;
    kb::g_dirty = true;
    kb::regions_flush();
    const auto journal_before = bytes("FILTER");
    check(journal_before == expected("FILTER", "0,0,0,7,0,0,8,0,0,9,0,0"),
          "test world actually persists the mixed base/off-base chunk set");
    const auto filtered = kb::chunks_outside_regions(kb::g_chunks, {settled});
    check(filtered.size() == 3, "the bound world's in-memory filtered selection is correct");
    kb::regions_flush();
    check(bytes("FILTER") == journal_before,
          "save-time filtering does not modify the persistent kept-chunk JSON");

    // ---- which caller gets Kiln's boxes: the world save only (and, so no edit goes unsaved, a caller Kiln does not know)
    const kb::CallSites sites{0x1000, 0x2000, 0x3000};
    check(kb::classify_caller(0x1000, sites) == kb::Caller::WorldSave, "the save's call site is the world save");
    check(kb::classify_caller(0x2000, sites) == kb::Caller::BaseZones, "the ECS publish step's call site is base zones");
    check(kb::classify_caller(0x3000, sites) == kb::Caller::BaseQuery, "the ECS query callback's call site is base query");
    check(kb::classify_caller(0x4000, sites) == kb::Caller::Unknown, "any other return address is unknown");
    check(kb::classify_caller(0x1000, kb::CallSites{}) == kb::Caller::Unknown, "unresolved call sites make every caller unknown");
    check(kb::kiln_boxes_for(kb::Caller::WorldSave) && kb::kiln_boxes_for(kb::Caller::Unknown),
          "the world save and an unknown caller get Kiln's boxes");
    check(!kb::kiln_boxes_for(kb::Caller::BaseZones) && !kb::kiln_boxes_for(kb::Caller::BaseQuery),
          "the base zones the game's systems and clients use never get Kiln's boxes");

    // ---- the game's list after its builder returned, as the hook sees it
    using regions_test::FakeList;
    kb::regions_lifecycle_ready(true, true);
    kb::F.persist = true;
    bind("SAVE");
    // wholly inside the 120 m altar (0,0,0) (6,6,6), on its edges reaching outside it (-1,0,0) (7,7,7), next to it
    // (9..10,0,0), and far away (40,0,40)
    const std::set<std::array<int32_t, 3>> kept{{0, 0, 0}, {6, 6, 6}, {-1, 0, 0}, {7, 7, 7}, {9, 0, 0}, {10, 0, 0},
                                                {40, 0, 40}};
    kb::g_chunks = kept;
    kb::g_dirty = kb::g_boxes_stale = true;
    kb::regions_flush();
    const auto json_before = bytes("SAVE");
    const uint8_t ok[8] = {0};
    const float alo[3] = {0.f, 0.f, 0.f}, ahi[3] = {120.f, 120.f, 120.f};
    const char base_id[17] = "REAL-ALTAR-BASE1";
    auto run = [&](uintptr_t ret, std::vector<uint8_t>& store, const float* lo, const float* hi) {
        FakeList l = regions_test::make_list(store);
        if (lo) regions_test::add_entry(l, base_id, lo, hi);
        kb::after_save_regions(ret, ok, &l.a, sites);
        return l;
    };
    {
        std::vector<uint8_t> store;
        FakeList l = run(0x1000, store, alo, ahi);
        check(l.a.count == 5 && regions_test::kiln_entries(l.a) == 4,
              "world save: boxes for the altar's two edge chunks (-1,0,0) (7,7,7), the two next to it (9..10,0,0) and the "
              "far one (40,0,40); none for the two wholly inside it");
        check(std::memcmp(l.a.data, base_id, 16) == 0, "world save: the game's own entry stays first and unchanged");
        check(regions_test::every_chunk_saved(l.a, kept, true),
              "world save: every kept chunk is in some entry's blob, and no Kiln box repeats a chunk the altar saves");
    }
    for (uintptr_t ret : {uintptr_t{0x2000}, uintptr_t{0x3000}}) {
        std::vector<uint8_t> store;
        FakeList l = run(ret, store, alo, ahi);
        check(l.a.count == 1 && regions_test::kiln_entries(l.a) == 0,
              "base zones / base query: the game's list is left exactly as its builder wrote it");
    }
    {
        std::vector<uint8_t> store;
        FakeList l = run(0x4000, store, alo, ahi);
        check(regions_test::kiln_entries(l.a) == 4 && regions_test::every_chunk_saved(l.a, kept, true),
              "an unknown caller gets the same boxes as the world save (no edit goes unsaved)");
    }
    {
        // a save in the first steps after a load sees the altar's 40 m bounds: its chunks outside them get boxes
        std::vector<uint8_t> store;
        const float slo[3] = {40.f, 40.f, 40.f}, shi[3] = {80.f, 80.f, 80.f};
        FakeList l = run(0x1000, store, slo, shi);
        check(regions_test::every_chunk_saved(l.a, kept, true) && regions_test::kiln_entries(l.a) >= 2,
              "a save during the 40 m startup bounds still saves every kept chunk (no time window)");
        std::vector<uint8_t> store2;
        FakeList l2 = run(0x1000, store2, alo, ahi);
        check(l2.a.count == 5, "the next save, with the 120 m bounds, is back to the four boxes the altar does not cover");
    }
    {
        // two bases: the chunks next to the first one are inside the second; abandoned bases are not in the list at all
        std::vector<uint8_t> store;
        FakeList l = regions_test::make_list(store);
        const float blo[3] = {140.f, 0.f, 0.f}, bhi[3] = {400.f, 120.f, 120.f};
        regions_test::add_entry(l, base_id, alo, ahi);
        regions_test::add_entry(l, "REAL-ALTAR-BASE2", blo, bhi);
        kb::after_save_regions(0x1000, ok, &l.a, sites);
        check(regions_test::kiln_entries(l.a) == 3 && regions_test::every_chunk_saved(l.a, kept, true),
              "two bases: boxes only for the first one's edge chunks and the far chunk (40,0,40)");
    }
    {
        std::vector<uint8_t> store;
        FakeList l = run(0x1000, store, nullptr, nullptr);
        check(regions_test::every_chunk_saved(l.a, kept, false) && l.a.count == regions_test::kiln_entries(l.a),
              "no base in the list (none, abandoned or removed): every kept chunk gets a Kiln box again");
    }
    {
        // bounds the hook cannot trust: every kept chunk gets a box, as before the filter
        std::vector<uint8_t> store;
        const float bad_lo[3] = {120.f, 0.f, 0.f}, bad_hi[3] = {0.f, 120.f, 120.f};
        FakeList l = run(0x1000, store, bad_lo, bad_hi);
        const auto all = kb::boxes_for_chunks(kept).size();
        check(regions_test::kiln_entries(l.a) == all, "inverted bounds: Kiln boxes for every kept chunk");
        std::vector<uint8_t> store2;
        FakeList l2 = regions_test::make_list(store2);
        regions_test::add_entry(l2, base_id, alo, ahi);
        l2.a.count = 300;  // more entries than the game ever writes: not read at all
        l2.a.capacity = 300;
        store2.resize(300 * 0x30);
        l2.a.data = store2.data();
        kb::after_save_regions(0x1000, ok, &l2.a, sites);
        check(l2.a.count == 300 + all, "an implausible list is not read: Kiln boxes for every kept chunk");
    }
    {
        std::vector<uint8_t> store;
        FakeList l = regions_test::make_list(store);
        regions_test::add_entry(l, base_id, alo, ahi);
        l.fail_grow = true;
        const int errors = kb::errors, warnings = kb::warnings;
        kb::after_save_regions(0x1000, ok, &l.a, sites);
        check(l.a.count == 1 && std::memcmp(l.a.data, base_id, 16) == 0,
              "a list that cannot grow keeps the game's entries, whole");
        check(kb::warnings + kb::errors > warnings + errors, "a box the list could not take is reported");
    }
    {
        std::vector<uint8_t> store;
        FakeList l = regions_test::make_list(store);
        regions_test::add_entry(l, base_id, alo, ahi);
        const uint8_t failed_result[8] = {0x27};
        kb::after_save_regions(0x1000, failed_result, &l.a, sites);
        check(l.a.count == 1, "the game's builder failed: its list is left alone");
    }
    {
        // a new edit next to the altar changes the next save's boxes; a released chunk leaves them
        std::vector<uint8_t> store;
        const int32_t o[3] = {12 * 32, 0, 0};
        const uint32_t d[3] = {1, 1, 1};
        kb::regions_keep(o, d);
        FakeList l = run(0x1000, store, alo, ahi);
        auto with_new = kept;
        with_new.insert({12, 0, 0});
        check(regions_test::every_chunk_saved(l.a, with_new, true), "a chunk kept after the last save is in the next one");
        kb::regions_release(o, d);
        std::vector<uint8_t> store2;
        FakeList l2 = run(0x1000, store2, alo, ahi);
        check(l2.a.count == 5, "a released chunk is no longer saved by a Kiln box");
    }
    {
        // the controlled reproduction's altar as the game had it (kiln#136): 120 m, bounds off the 16 m chunk grid but
        // for one face. Chunks wholly inside it need no box, its edge chunks that reach outside it do.
        const float rlo[3] = {4350.5f, 718.f, 2752.f}, rhi[3] = {4470.5f, 838.f, 2872.f};
        const std::set<std::array<int32_t, 3>> edits{
            {273, 46, 173}, {275, 47, 176},  // inside
            {272, 45, 172},                  // inside: the z face is on the chunk grid (2752 = 16 * 172)
            {271, 46, 173},                  // x [4336, 4352]: only its last 4 m block reaches 4350.5
            {279, 46, 173},                  // x [4464, 4480]: two of its four blocks reach past 4470.5
            {275, 44, 173},                  // y [704, 720]: only its last block reaches 718
            {275, 46, 171},                  // z [2736, 2752]: only its last block touches 2752
            {290, 46, 173},                  // off the base
        };
        kb::g_chunks = edits;
        kb::g_boxes_stale = true;
        std::vector<uint8_t> store;
        FakeList l = run(0x1000, store, rlo, rhi);
        check(regions_test::every_chunk_saved(l.a, edits, true),
              "the reported altar: every 4 m block of every kept chunk is in some entry's blob, no box for a chunk it "
              "saves whole");
        const auto need = kb::chunks_outside_regions(edits, {kb::RegionBounds{{rlo[0], rlo[1], rlo[2]}, {rhi[0], rhi[1], rhi[2]}}});
        check(need.size() == 5 && !need.count({273, 46, 173}) && !need.count({275, 47, 176}) && !need.count({272, 45, 172}),
              "the reported altar: the four edge chunks and the off-base one need Kiln's boxes, the three inside do not");
        kb::g_chunks = kept;
        kb::g_boxes_stale = true;
    }
    kb::regions_flush();
    check(bytes("SAVE") == json_before, "the saves above leave the kept-chunk JSON byte for byte as it was");
    std::filesystem::remove_all(dir);
    std::printf("regions: %d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
