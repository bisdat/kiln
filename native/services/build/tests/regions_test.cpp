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
int voxel_closes = 0, transient_closes = 0;
void vox_world_closed() { ++voxel_closes; }
void transient_world_closed() { ++transient_closes; }

struct { bool persist = true; } F;
std::string identity;
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
    std::filesystem::remove_all(dir);
    std::printf("regions: %d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
