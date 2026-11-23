// SPDX-License-Identifier: MIT
// Compile the actual region bookkeeping, replacing only the game adapter and atomic file replacement.
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <unistd.h>
#define KILN_REGIONS_SUPPORT_STUBS
#include "regions_test_support.h"
#undef KILN_REGIONS_SUPPORT_STUBS
#include "../regions.cpp"
#include "regions_test_support.h"

namespace {
int passed = 0, failed = 0;
// A fully walked stack of the game's own frames from `ret` down (kb::kWalkFrames of them), and a frame elsewhere.
kb::Frame in_game(uintptr_t at) {
    kb::Frame f;
    f.at = at;
    return f;
}
std::vector<kb::Frame> game_stack(uintptr_t ret) {
    std::vector<kb::Frame> f{in_game(ret)};
    for (uintptr_t i = 1; i < kb::kWalkFrames; ++i) f.push_back(in_game(0x9000 + i * 0x10));
    return f;
}
kb::Frame frame_in(uintptr_t at, kb::Place place, const char* module, uint64_t offset) {
    kb::Frame f;
    f.at = at;
    f.place = place;
    f.module = module;
    f.offset = offset;
    return f;
}
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

    // ---- which caller gets Kiln's boxes: the world save only, when its call site resolved
    const kb::CallSites sites{0x1000, 0x2000, 0x3000};
    check(kb::classify_caller(0x1000, sites) == kb::Caller::WorldSave, "the save's call site is the world save");
    check(kb::classify_caller(0x2000, sites) == kb::Caller::BaseZones, "the ECS publish step's call site is base zones");
    check(kb::classify_caller(0x3000, sites) == kb::Caller::BaseQuery, "the ECS query callback's call site is base query");
    check(kb::classify_caller(0x4000, sites) == kb::Caller::Unknown, "any other return address is unknown");
    check(kb::classify_caller(0x1000, kb::CallSites{}) == kb::Caller::Unknown, "unresolved call sites make every caller unknown");
    check(kb::kiln_boxes_for(kb::Caller::WorldSave, sites) && !kb::kiln_boxes_for(kb::Caller::Unknown, sites),
          "world save call site resolved: the world save gets Kiln's boxes, a caller Kiln does not know none");
    check(!kb::kiln_boxes_for(kb::Caller::BaseZones, sites) && !kb::kiln_boxes_for(kb::Caller::BaseQuery, sites),
          "world save call site resolved: the base zones the game's systems and clients use get no Kiln boxes");
    for (kb::Caller c : {kb::Caller::WorldSave, kb::Caller::BaseZones, kb::Caller::BaseQuery, kb::Caller::Unknown})
        check(kb::kiln_boxes_for(c, kb::CallSites{0, 0x2000, 0x3000}),
              "world save call site unresolved: every caller gets Kiln's boxes, as up to 30.8");

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
        const int warnings = kb::warnings;
        const auto stack = game_stack(0x4000);
        FakeList l = regions_test::make_list(store);
        regions_test::add_entry(l, base_id, alo, ahi);
        kb::after_save_regions(0x4000, ok, &l.a, sites, &stack);
        check(l.a.count == 1 && regions_test::kiln_entries(l.a) == 0,
              "world save call site resolved: an unknown caller whose frames are all the game's gets no Kiln boxes");
        check(kb::warnings == warnings + 1 && kb::last_warning.find("none of its three known callers") != std::string::npos &&
                  kb::last_warning.find("adds no boxes") != std::string::npos,
              "an unknown caller is reported once, saying it gets no boxes");
        std::vector<uint8_t> store2;
        FakeList l2 = regions_test::make_list(store2);
        kb::after_save_regions(0x4000, ok, &l2.a, sites, &stack);
        check(kb::warnings == warnings + 1, "an unknown caller is reported only once");
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
    {
        // Whatever the bases and the kept chunks, the world save's list keeps every 4 m block of every kept chunk (the
        // game's float rule, regions_test_support.h), and Kiln's exact boxes never touch a chunk the bases save whole.
        // Bases on and off the 4 m and 16 m grids, negative coordinates, rounds scattered enough for the coarse boxes.
        uint64_t seed = 0x9e3779b97f4a7c15ull;
        auto rnd = [&seed] {
            seed ^= seed << 13;
            seed ^= seed >> 7;
            seed ^= seed << 17;
            return seed;
        };
        bool saved = true, exact_no_redundant = true, appended = true;
        int coarse = 0, filtered = 0;
        for (int round = 0; round < 400; ++round) {
            const int32_t ox = static_cast<int32_t>(rnd() % 600) - 300, oz = static_cast<int32_t>(rnd() % 600) - 300;
            std::set<std::array<int32_t, 3>> chunks;
            const int n = 1 + static_cast<int>(rnd() % (round % 4 == 0 ? 600 : 40));
            for (int i = 0; i < n; ++i)
                chunks.insert({ox + static_cast<int32_t>(rnd() % 40) - 20, static_cast<int32_t>(rnd() % 8) - 2,
                               oz + static_cast<int32_t>(rnd() % 40) - 20});
            std::vector<uint8_t> store;
            FakeList l = regions_test::make_list(store);
            std::vector<kb::RegionBounds> native;
            const int bases = static_cast<int>(rnd() % 4);
            for (int b = 0; b < bases; ++b) {
                float lo[3], hi[3];
                for (int k = 0; k < 3; ++k) {
                    const float centre = (k == 0 ? ox * 16.f : k == 2 ? oz * 16.f : 32.f) +
                                         static_cast<float>(static_cast<int>(rnd() % 1280) - 640) * 0.25f;
                    const float half = 4.f + static_cast<float>(rnd() % 320) * 0.25f;
                    lo[k] = centre - half;
                    hi[k] = centre + half;
                }
                regions_test::add_entry(l, base_id, lo, hi);
                native.push_back(kb::RegionBounds{{lo[0], lo[1], lo[2]}, {hi[0], hi[1], hi[2]}});
            }
            const auto outside = kb::chunks_outside_regions(chunks, native);
            const auto boxes = kb::boxes_for_chunks(outside);
            uint64_t volume = 0;
            for (const auto& b : boxes)
                volume += uint64_t(b.hi[0] - b.lo[0] + 1) * uint64_t(b.hi[1] - b.lo[1] + 1) * uint64_t(b.hi[2] - b.lo[2] + 1);
            const bool exact = volume == outside.size();  // the greedy boxes cover exactly the chunks; coarse ones more
            coarse += !exact;
            filtered += outside.size() < chunks.size();
            kb::g_chunks = chunks;
            kb::g_boxes_stale = true;
            kb::after_save_regions(0x1000, ok, &l.a, sites);
            saved = saved && regions_test::every_chunk_saved(l.a, chunks, false);
            if (exact) exact_no_redundant = exact_no_redundant && regions_test::every_chunk_saved(l.a, chunks, true);
            appended = appended && regions_test::kiln_entries(l.a) == boxes.size() &&
                       l.a.count == static_cast<uint64_t>(bases) + boxes.size();
        }
        check(saved, "any bases, any kept chunks: every 4 m block of every kept chunk is in some entry's blob");
        check(exact_no_redundant, "any bases: Kiln's exact boxes never repeat a chunk the bases save whole");
        check(appended, "any bases: the list holds the game's entries and every box Kiln planned");
        check(coarse > 0 && filtered > 0, "the rounds include coarse boxes and chunks the bases save whole");

        // bounds that are not numbers: the entries are not trusted, every kept chunk gets a box
        kb::g_chunks = kept;
        kb::g_boxes_stale = true;
        std::vector<uint8_t> store;
        const float nan = std::numeric_limits<float>::quiet_NaN();
        const float nlo[3] = {0.f, nan, 0.f};
        FakeList l = run(0x1000, store, nlo, ahi);
        check(regions_test::kiln_entries(l.a) == kb::boxes_for_chunks(kept).size(),
              "a base whose bounds are not numbers: Kiln boxes for every kept chunk");

        // call sites that resolved only in part, or to one address: the world save is never taken for a base-zone caller
        const kb::CallSites no_save{0, 0x2000, 0x3000}, same{0x1000, 0x1000, 0x1000};
        std::vector<uint8_t> s1, s2, s3;
        FakeList a1 = regions_test::make_list(s1), a2 = regions_test::make_list(s2), a3 = regions_test::make_list(s3);
        regions_test::add_entry(a1, base_id, alo, ahi);
        regions_test::add_entry(a2, base_id, alo, ahi);
        regions_test::add_entry(a3, base_id, alo, ahi);
        kb::after_save_regions(0x1000, ok, &a1.a, no_save);
        kb::after_save_regions(0x2000, ok, &a2.a, no_save);
        kb::after_save_regions(0x1000, ok, &a3.a, same);
        check(regions_test::kiln_entries(a1.a) == 4 && regions_test::kiln_entries(a2.a) == 4,
              "world save site unresolved: the save's call gets the boxes, and so do the base zones (as up to 30.8)");
        check(kb::classify_caller(0x1000, same) == kb::Caller::WorldSave && regions_test::kiln_entries(a3.a) == 4,
              "three names on one address: the world save wins, so the save keeps its boxes");
    }
    {
        // A game update that breaks the call-site patterns (the sigdb names build/RetSaveRegions*): who gets Kiln's boxes,
        // the warning in kiln.log when kiln.build starts, and the text of its kiln.caps row. The game still calls from its
        // three sites (0x1000 world save, 0x2000 base zones, 0x3000 base query); 0x4000 is a caller Kiln never knew.
        const std::string save_name = "build/RetSaveRegionsWorldSave", zones_name = "build/RetSaveRegionsBaseZones",
                          query_name = "build/RetSaveRegionsBaseQuery";
        struct Case {
            const char* what;
            kb::CallSites sites;
            bool boxes[4];  // world save, base zones, base query, another caller
            std::vector<std::string> missing;
        };
        const Case cases[] = {
            {"all resolved", {0x1000, 0x2000, 0x3000}, {true, false, false, false}, {}},
            {"base zones missing", {0x1000, 0, 0x3000}, {true, false, false, false}, {zones_name}},
            {"base query missing", {0x1000, 0x2000, 0}, {true, false, false, false}, {query_name}},
            {"world save missing", {0, 0x2000, 0x3000}, {true, true, true, true}, {save_name}},
            {"all missing", {0, 0, 0}, {true, true, true, true}, {save_name, zones_name, query_name}},
        };
        for (const Case& c : cases) {
            const std::string what = c.what;
            auto says = [](const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; };
            const int warnings = kb::warnings, reports = kb::cap_reports;
            kb::last_info.clear();
            kb::last_warning.clear();
            kb::announce_sites(c.sites);
            const bool all = c.missing.empty();
            check(kb::warnings == warnings + (all ? 0 : 1),
                  (what + ": a warning in kiln.log exactly when a name is missing").c_str());
            check(kb::cap_reports == reports + 1, (what + ": kiln.build reports its kiln.caps row").c_str());
            check(kb::cap_text.size() <= 512, (what + ": the row's text fits kiln.caps' 512 bytes").c_str());
            if (c.sites.world_save)
                check(says(kb::cap_text, "it may be the world save"),
                      (what + ": the row says a call Kiln cannot place gets the boxes too").c_str());
            const std::string& line = all ? kb::last_info : kb::last_warning;
            for (const std::string& name : {save_name, zones_name, query_name}) {
                const bool listed = std::find(c.missing.begin(), c.missing.end(), name) != c.missing.end();
                check(says(line, name) == listed,
                      (what + ": kiln.log names " + name + " exactly when it is missing").c_str());
                check(says(kb::cap_text, name) == listed,
                      (what + ": the row names " + name + " exactly when it is missing").c_str());
            }
            if (!all) {
                check(says(line, "not resolved") && says(kb::cap_text, "not resolved"),
                      (what + ": the warning and the row say the names did not resolve").c_str());
            }
            if (c.sites.world_save) {
                check(kb::cap_available && says(kb::cap_text, "world save's region list only") &&
                          says(line, "world save's region list only"),
                      (what + ": row available; Kiln's boxes go into the world save's list only").c_str());
            } else {
                check(!kb::cap_available && says(kb::cap_text, "every caller") && says(kb::cap_text, "0.2.0-dev.30.8") &&
                          says(line, "every caller") && says(line, "Flame Altar"),
                      (what + ": row unavailable; every caller gets Kiln's boxes, as up to 0.2.0-dev.30.8").c_str());
            }
            const uintptr_t rets[4] = {0x1000, 0x2000, 0x3000, 0x4000};
            const char* who[4] = {"the world save", "the base zones", "the base query", "another caller"};
            for (int i = 0; i < 4; ++i) {
                std::vector<uint8_t> store;
                FakeList l = regions_test::make_list(store);
                regions_test::add_entry(l, base_id, alo, ahi);
                const auto stack = game_stack(rets[i]);  // the game's own frames: no other module's hook on the way
                kb::after_save_regions(rets[i], ok, &l.a, c.sites, &stack);
                const bool got = c.boxes[i]
                                     ? regions_test::kiln_entries(l.a) == 4 && regions_test::every_chunk_saved(l.a, kept, true)
                                     : l.a.count == 1 && regions_test::kiln_entries(l.a) == 0;
                check(got, (what + ": " + who[i] + (c.boxes[i] ? " gets Kiln's boxes" : " gets none")).c_str());
            }
        }
    }
    {
        // Another mod's detour in front of Kiln's (on the builder, or on one of its callers) changes the return address
        // Kiln's hook sees: the call is placed by the frames below it, up to kWalkFrames. A call it cannot place gets the
        // boxes when the stack shows another module's code on the way, or cannot be walked: it may be the world save.
        using kb::Place;
        const kb::CallSites all{0x1000, 0x2000, 0x3000};
        auto kiln_boxes = [&](uintptr_t ret, const std::vector<kb::Frame>* frames, const kb::CallSites& s) {
            std::vector<uint8_t> store;
            FakeList l = regions_test::make_list(store);
            regions_test::add_entry(l, base_id, alo, ahi);
            kb::after_save_regions(ret, ok, &l.a, s, frames);
            const uint64_t n = regions_test::kiln_entries(l.a);
            return n == 4 && regions_test::every_chunk_saved(l.a, kept, true) ? 4 : n;
        };
        const uintptr_t mfa = 0x7ff812340100;  // a return address in another mod's DLL, mfa.dll+0x100
        const kb::Frame mfa_frame = frame_in(mfa, Place::Other, "mfa.dll", 0x100);
        {
            const std::vector<kb::Frame> st{mfa_frame, in_game(0x1000), in_game(0x9100), in_game(0x9200)};
            const auto v = kb::judge_caller(mfa, st, all);
            check(v.kind == kb::Caller::WorldSave && v.how == kb::Found::Deeper && v.boxes && v.via.at == 0x1000,
                  "a foreign detour in front of the world save: placed by the world save's site one frame deeper");
            check(kiln_boxes(mfa, &st, all) == 4, "a foreign detour in front of the world save: the save gets the boxes");
        }
        {
            const std::vector<kb::Frame> st{mfa_frame, in_game(0x2000), in_game(0x9100), in_game(0x9200)};
            const auto v = kb::judge_caller(mfa, st, all);
            check(v.kind == kb::Caller::BaseZones && v.how == kb::Found::Deeper && !v.boxes,
                  "a foreign detour in front of the base-zone step: placed by the base-zone site found deeper");
            check(kiln_boxes(mfa, &st, all) == 0, "a foreign detour in front of the base-zone step: no boxes");
        }
        {
            // another Kiln link on the builder (a kiln-*.dll frame) is Kiln's, not another mod's
            const kb::Frame kiln_link = frame_in(0x7ff900001000, Place::Kiln, "kiln-items.dll", 0x1000);
            const std::vector<kb::Frame> st{kiln_link, in_game(0x3000), in_game(0x9100)};
            check(kb::judge_caller(0x7ff900001000, st, all).kind == kb::Caller::BaseQuery && kiln_boxes(0x7ff900001000, &st, all) == 0,
                  "another Kiln link in front of the base query: placed by the deeper site, no boxes");
            auto own = game_stack(0x4000);
            own[1] = kiln_link;
            const auto v = kb::judge_caller(0x4000, own, all);
            check(v.how == kb::Found::GameOnly && !v.boxes, "the game's frames and a Kiln module's only: no boxes");
        }
        {
            // the world save's site past kWalkFrames is not looked at
            auto st = game_stack(0x4000);
            st.push_back(in_game(0x1000));
            check(kb::judge_caller(0x4000, st, all).how == kb::Found::GameOnly, "a known site past 8 frames is not looked at");
        }
        {
            // no known site within 8 frames, another module on the way (a mod that calls the builder from its own code,
            // or the world save's site moved): it may be the world save, so it gets the boxes; told once, naming the module
            std::vector<kb::Frame> st = game_stack(0x4000);
            st[0] = mfa_frame;
            const auto v = kb::judge_caller(mfa, st, all);
            check(v.kind == kb::Caller::Unknown && v.how == kb::Found::Foreign && v.boxes && v.via.module == "mfa.dll",
                  "no known site and another module's frame: Foreign, boxes");
            const int warnings = kb::warnings;
            check(kiln_boxes(mfa, &st, all) == 4, "no known site and another module's frame: the list gets the boxes");
            check(kb::warnings == warnings + 1 && kb::last_warning.find("mfa.dll+0x100") != std::string::npos &&
                      kb::last_warning.find("may be the world save") != std::string::npos,
                  "a call through another module is reported once, naming the module");
            kiln_boxes(mfa, &st, all);
            check(kb::warnings == warnings + 1, "a call through another module is reported only once");
            // a relay in no module (a hook's stub in private memory) is another module's code too
            std::vector<kb::Frame> relay = game_stack(0x4000);
            relay[2] = frame_in(0x500000, Place::Other, "", 0);
            const auto r = kb::judge_caller(0x4000, relay, all);
            check(r.how == kb::Found::Foreign && r.boxes && kb::frame_text(r.via).find("in no module") != std::string::npos,
                  "a frame in no module (a relay): Foreign, boxes");
        }
        {
            // a stack that cannot be walked past the return address: the boxes, as it may be the world save
            const std::vector<kb::Frame> only{in_game(0x5000)};
            check(kb::judge_caller(0x5000, only, all).how == kb::Found::Unwalked && kb::judge_caller(0x5000, only, all).boxes,
                  "only the return address: Unwalked, boxes");
            check(kb::judge_caller(0x5000, {}, all).how == kb::Found::Unwalked, "no frames at all: Unwalked");
            const int warnings = kb::warnings;
            check(kiln_boxes(0x5000, &only, all) == 4 && kiln_boxes(0x5000, nullptr, all) == 4,
                  "an unwalkable stack, or none walked: the list gets the boxes");
            check(kb::warnings == warnings + 1 && kb::last_warning.find("could not be walked") != std::string::npos,
                  "an unwalkable stack is reported once");
        }
        {
            // the world save's site unresolved: every call gets the boxes whatever its frames
            const kb::CallSites no_save{0, 0x2000, 0x3000};
            const auto game = game_stack(0x4000);
            const std::vector<kb::Frame> zones{mfa_frame, in_game(0x2000)};
            check(kb::judge_caller(0x4000, game, no_save).boxes && kb::judge_caller(mfa, zones, no_save).boxes &&
                      kiln_boxes(0x4000, &game, no_save) == 4,
                  "world save site unresolved: game-only and base-zone stacks get the boxes too");
        }
    }
    {
        // The game's voxel writer collects at most 4,096 stored chunks per record and skips the rest without a word, in
        // hash order (docs/re/build.md §12): a base entry whose bounds touch more chunk positions than that may lose any
        // chunk, so Kiln's kept chunks inside it keep their own boxes.
        const kb::RegionBounds at_limit{{0.5f, 0.5f, 0.5f}, {255.5f, 255.5f, 255.5f}};  // 16 x 16 x 16 positions
        const kb::RegionBounds over_limit{{0.5f, 1.f, 0.5f}, {271.5f, 15.f, 3855.5f}};  // 17 x 1 x 241 = 4,097
        check(kb::chunk_positions(at_limit) == 4096 && kb::chunk_positions(over_limit) == 4097,
              "chunk positions an entry touches: 4,096 and 4,097");
        const kb::ChunkSet inside{{5, 0, 5}, {8, 0, 7}};
        check(kb::chunks_outside_regions(inside, {at_limit}).empty(),
              "an entry at 4,096 chunk positions saves the kept chunks wholly inside it: no box");
        check(kb::chunks_outside_regions(inside, {over_limit}) == inside,
              "an entry at 4,097 chunk positions may lose any chunk: every kept chunk in it needs a box");
        const kb::RegionBounds grid_240{{5888.f, 1000.5f, 1000.5f}, {6128.f, 1240.5f, 1240.5f}};  // x faces on the 16 m grid
        const kb::RegionBounds grid_239{{5888.f, 1000.5f, 1000.5f}, {6127.f, 1239.5f, 1239.5f}};
        check(kb::chunk_positions(grid_240) == 17 * 16 * 16 && kb::chunk_positions(grid_239) <= 4096,
              "a 240 m base with faces on the chunk grid touches 4,352 chunk positions, a 239 m one at most 4,096");
        check(kb::chunks_outside_regions({{375, 70, 70}}, {grid_240}).size() == 1 &&
                  kb::chunks_outside_regions({{375, 70, 70}}, {grid_239}).empty(),
              "a kept chunk inside the grid-aligned 240 m base gets a box, inside the 239 m one none");
        auto save_with = [&](const kb::ChunkSet& chunks, const float* lo, const float* hi, std::vector<uint8_t>& store) {
            kb::g_chunks = chunks;
            kb::g_boxes_stale = true;
            FakeList l = regions_test::make_list(store);
            if (lo) regions_test::add_entry(l, base_id, lo, hi);
            kb::after_save_regions(0x1000, ok, &l.a, sites);
            return l;
        };
        {
            std::vector<uint8_t> s1, s2;
            const float l1[3] = {0.5f, 0.5f, 0.5f}, h1[3] = {255.5f, 255.5f, 255.5f};
            const float l2[3] = {0.5f, 1.f, 0.5f}, h2[3] = {271.5f, 15.f, 3855.5f};
            FakeList a = save_with(inside, l1, h1, s1), b = save_with(inside, l2, h2, s2);
            check(regions_test::kiln_entries(a.a) == 0 && regions_test::every_chunk_saved(a.a, inside, true),
                  "world save, a base at 4,096 chunk positions: no box, the base's record keeps the kept chunks");
            check(regions_test::kiln_entries(b.a) == 2 && regions_test::every_chunk_saved(b.a, inside, true),
                  "world save, a base at 4,097 chunk positions: a box for each kept chunk in it");
        }
        {
            // the lab's 288 m altar (6,498 stored chunks, 2,402 skipped by the writer): Kiln's kept chunks in it, even at
            // its centre, keep their boxes
            std::vector<uint8_t> store;
            const float lo[3] = {5856.25f, 1056.f, 5856.25f}, hi[3] = {6144.25f, 1344.f, 6144.25f};
            const kb::ChunkSet in_288{{375, 75, 375}, {370, 70, 380}, {381, 80, 367}};
            FakeList l = save_with(in_288, lo, hi, store);
            check(kb::chunk_positions(kb::RegionBounds{{lo[0], lo[1], lo[2]}, {hi[0], hi[1], hi[2]}}) == 19 * 20 * 19,
                  "the 288 m altar touches 7,220 chunk positions");
            check(regions_test::kiln_entries(l.a) == 3 && regions_test::every_chunk_saved(l.a, in_288, false),
                  "a 288 m base: every kept chunk in it, the centre too, is in a Kiln box the writer keeps whole");
        }
        {
            // Kiln's own boxes are records too: none may cover more than 4,096 chunk positions
            kb::ChunkSet solid;
            for (int32_t x = 0; x < 17; ++x)
                for (int32_t y = 0; y < 16; ++y)
                    for (int32_t z = 0; z < 16; ++z) solid.insert({1000 + x, y, 1000 + z});
            const auto boxes = kb::boxes_for_chunks(solid);
            uint64_t volume = 0, most = 0;
            bool within = true;
            for (const auto& b : boxes) {
                const uint64_t v =
                    uint64_t(b.hi[0] - b.lo[0] + 1) * uint64_t(b.hi[1] - b.lo[1] + 1) * uint64_t(b.hi[2] - b.lo[2] + 1);
                volume += v;
                most = std::max(most, v);
                const int32_t lo[3] = {1000, 0, 1000}, hi[3] = {1016, 15, 1015};
                for (int k = 0; k < 3; ++k) within = within && b.lo[k] >= lo[k] && b.hi[k] <= hi[k];
            }
            check(boxes.size() == 2 && most <= 4096 && volume == solid.size() && within,
                  "a solid block of 17 x 16 x 16 kept chunks: two boxes of at most 4,096 chunks, together exactly the block");
            std::vector<uint8_t> store;
            FakeList l = save_with(solid, nullptr, nullptr, store);
            check(regions_test::max_kiln_positions(l.a) <= 4096 && regions_test::every_chunk_saved(l.a, solid, false),
                  "world save: no Kiln box over 4,096 chunk positions, every chunk of the block saved");
            // the coarse boxes too (over 64 runs: one box per 128 m column), here 8 x 131 x 8 chunks high
            kb::ChunkSet scattered;
            for (int32_t k = 0; k < 66; ++k) scattered.insert({k % 8, 2 * k, (k / 8) % 8});
            std::vector<uint8_t> store2;
            FakeList l2 = save_with(scattered, nullptr, nullptr, store2);
            check(regions_test::kiln_entries(l2.a) >= 3 && regions_test::max_kiln_positions(l2.a) <= 4096 &&
                      regions_test::every_chunk_saved(l2.a, scattered, false),
                  "scattered edits in one 128 m column: its coarse box is cut at 4,096 chunk positions, every chunk saved");
        }
        kb::g_chunks = kept;
        kb::g_boxes_stale = true;
    }
    kb::g_chunks = kept;
    kb::g_boxes_stale = true;
    kb::regions_flush();
    check(bytes("SAVE") == json_before, "the saves above leave the kept-chunk JSON byte for byte as it was");
    std::filesystem::remove_all(dir);
    std::printf("regions: %d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
