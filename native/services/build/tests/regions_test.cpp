// SPDX-License-Identifier: MIT
// Compile the actual region bookkeeping, replacing only the game adapter and atomic file replacement.
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>
#define KILN_CALL
struct KilnWorldEvent { int kind, event; };
constexpr int KILN_WORLD_SERVER = 1, KILN_WORLD_STOPPING = 2;
namespace kb {
int voxel_closes = 0, transient_closes = 0, block_closes = 0;
void block_world_closed() { ++block_closes; }
void terrain_budget_reset() {}
void vox_world_closed() { ++voxel_closes; }
void transient_world_closed() { ++transient_closes; }

struct { bool persist = true; } F;
std::string identity;
std::string detail;
void set_detail(const char* text) { detail = text; }
std::string current_world() { return identity; }
}
#define KILN_REGIONS_HOST_TEST
#define KB_I(...) ((void)0)
#include "../regions.cpp"

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
    // any private world-save data. The serializer saves whole 16 m chunks on intersection.
    const kb::RegionBounds altar{{0.f, 0.f, 0.f}, {120.f, 120.f, 120.f}};
    const std::set<std::array<int32_t, 3>> sample{
        {-1, 0, 0}, {0, 0, 0}, {7, 0, 0}, {7, 7, 7},
        {8, 0, 0}, {0, 8, 0}, {8, 8, 8}
    };
    const auto cover = kb::classify_chunks(sample, {altar});
    check(cover.overlaps_base == 4 && cover.outside_bases == 3,
          "120 m altar: partially overlapping 16 m chunks are saved by the base; off-base chunks are not");
    check(kb::classify_chunks(sample, {}).outside_bases == sample.size(),
          "without any genuine base, all kept chunks require external coverage");
    const kb::RegionBounds edge{{0.f, 0.f, 0.f}, {128.f, 128.f, 128.f}};
    check(kb::chunk_touches_region({8, 0, 0}, edge),
          "a chunk touching a base region at exactly 128 m counts as overlapping");
    const kb::RegionBounds second{{130.f, 0.f, 0.f}, {160.f, 160.f, 160.f}};
    check(kb::classify_chunks({{8, 0, 0}}, {altar, second}).overlaps_base == 1,
          "a chunk saved by either of two bases is not classified as off-base");
    const kb::RegionBounds invalid{{12.f, 0.f, 0.f}, {0.f, 16.f, 16.f}};
    check(!kb::chunk_touches_region({0, 0, 0}, invalid),
          "inverted base bounds are never treated as verified coverage");

    // The 40 m start-zone and 120 m final-zone from the controlled reproduction must
    // not generate synthetic save regions during the brief transition.
    kb::NativeRegionStability stability;
    const auto t0 = std::chrono::steady_clock::time_point{};
    const kb::RegionBounds startup{{40.f, 40.f, 40.f}, {80.f, 80.f, 80.f}};
    const kb::RegionBounds settled{{0.f, 0.f, 0.f}, {120.f, 120.f, 120.f}};
    check(!stability.observe({startup}, t0), "first startup bounds observation is unsettled");
    check(!stability.observe({startup}, t0 + std::chrono::milliseconds(50)),
          "startup 40 m bounds remain unsettled before the guard expires");
    check(!stability.observe({settled}, t0 + std::chrono::milliseconds(60)),
          "40 to 120 m upgrade resets stabilization");
    check(!stability.observe({settled}, t0 + std::chrono::milliseconds(1490)),
          "a stable 120 m region still respects the guard interval");
    check(stability.observe({settled}, t0 + std::chrono::milliseconds(1570)),
          "120 m region becomes eligible only once stable");
    check(!stability.observe({startup}, t0 + std::chrono::milliseconds(2000)),
          "an altar shrink or reset immediately invalidates stable coverage");

    const kb::ChunkSet protected_chunks{{0, 0, 0}, {7, 7, 7}};
    check(kb::chunks_outside_regions(protected_chunks, {settled}).empty(),
          "all native-covered kept chunks can be omitted from synthetic injection");
    check(kb::boxes_for_chunks(kb::chunks_outside_regions(protected_chunks, {settled})).empty(),
          "fully covered world serializes zero synthetic boxes");

    const kb::ChunkSet boundary_mix{{0, 0, 0}, {7, 0, 0}, {8, 0, 0}, {9, 0, 0}};
    const auto uncovered = kb::chunks_outside_regions(boundary_mix, {settled});
    check(uncovered.size() == 2 && uncovered.count({8, 0, 0}) && uncovered.count({9, 0, 0}),
          "only entirely off-base store chunks remain in extra-save coverage");
    check(kb::boxes_for_chunks(uncovered).size() == 1,
          "contiguous off-base chunks are serialized as a single synthetic box");
    check(kb::chunks_outside_regions(boundary_mix, {}).size() == boundary_mix.size(),
          "without a native altar all kept chunks retain extra coverage");
    check(kb::chunks_outside_regions(boundary_mix, {startup}).size() >=
              kb::chunks_outside_regions(boundary_mix, {settled}).size(),
          "shrinking an altar can increase the off-base coverage requirement");

    kb::g_chunks = boundary_mix;
    kb::g_dirty = true;
    kb::regions_flush();
    const auto journal_before = bytes("A");
    (void)kb::chunks_outside_regions(kb::g_chunks, {settled});
    kb::regions_flush();
    check(bytes("A") == journal_before,
          "save-time filtering does not modify the persistent kept-chunk JSON");
    kb::g_chunks.clear();
    kb::g_dirty = true;
    std::filesystem::remove_all(dir);
    std::printf("regions: %d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
