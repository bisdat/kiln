// SPDX-License-Identifier: MIT
// Kiln save regions. The game saves voxel edits only inside player-base build zones. build/SaveRegions builds the list of
// the active (not abandoned) base zones from BuildZoneSystem: entries {BaseId[16], float3 min, pad, float3 max, pad},
// 0x30 bytes, in a game array {data, count, capacity, grow}. Three places in the game call it (docs/re/build.md §12):
//  - the world save, which writes one base record per entry: its BaseId, its bounds, the stored 32-cell chunks whose 16 m
//    box touches the bounds, of each only the 4 m blocks (8 cells a side) that touch them too (the rest written empty:
//    it regrows), and the water inside them. The loader applies every record it finds, base or not; the world list
//    counts the records as Flame Altars;
//  - the step that publishes the base zones to the ECS when BuildZoneSystem changed: the base areas the game's systems
//    group stations, storage and beds by, and every client receives;
//  - an ECS callback that asks which entities stand inside a base.
// Kiln remembers the 16 m chunks its own writes touched (kiln/data/kiln.build/regions/<world GUID>.json, written
// atomically when it changes). Into the world save's list only, after the game's own entries, it appends boxes for the
// kept chunks the entries of that same list do not save whole (every 4 m block), so the save keeps Kiln's edits outside
// bases too, without a box (and a Flame Altar in the world list) for chunks a base saves already. The other two callers
// get the game's list as its builder wrote it. When a game update breaks the world save's call-site pattern, every
// caller gets the boxes, as up to 0.2.0-dev.30.8: a lost edit is worse than extra altars. The game's voxel writer keeps
// at most 4,096 stored chunks per record, so a base that touches more chunks does not count as saving any, and no Kiln
// box covers more than 4,096 chunks. Thanks bisdat (kiln#136) for the covered-chunk filter and its bounds checks.
#ifdef _WIN32
#include <windows.h>
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <vector>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>

#ifndef KILN_REGIONS_HOST_TEST
#include <cstddef>

#include "internal.h"
#include "kiln_hook.hpp"
#include "services/kiln_caps.h"
#include "services/kiln_saves.h"
#endif

namespace kb {
#ifdef KILN_REGIONS_HOST_TEST
bool regions_lifecycle_safe();
#endif
namespace {

constexpr int kChunk = 32;              // cells per store chunk edge (16 m)
constexpr int kBlock = 8;               // cells per edge of the blocks the save keeps or drops (4 m)
constexpr int kBlocks = kChunk / kBlock;  // blocks per chunk edge: 4, 64 blocks per chunk
constexpr uint64_t kWholeChunk = ~uint64_t{0};
constexpr size_t kEntry = 0x30;         // one entry of the game's region list
constexpr uint64_t kMaxGameEntries = 256;  // far more than the game's base limit: a larger list is not read
// The game's voxel writer collects a record's stored chunks into a fixed scratch array of 4,096 keys on its stack and
// skips every chunk past it without a word, in hash order, so the loss is scattered over the whole record (server
// 0x83d200, the array set up at 0x83d2f8, grow callback 0x832de0; client 0x10b89b0, 0x10b8aa8, 0x10ae690;
// docs/re/build.md §12).
constexpr double kWriterChunks = 4096;

struct Box {
    int32_t lo[3], hi[3];  // chunk coordinates, inclusive
};

// An entry's bounds, as the voxel serializer gets them.
struct RegionBounds {
    float lo[3], hi[3];
};
struct RegionCoverage {
    size_t overlaps_base = 0;  // kept chunks the entries save whole
    size_t outside_bases = 0;  // kept chunks they save in part or not at all: Kiln's boxes
};
bool valid_region(const RegionBounds& b) {
    for (int axis = 0; axis < 3; ++axis)
        if (!std::isfinite(b.lo[axis]) || !std::isfinite(b.hi[axis]) || b.lo[axis] > b.hi[axis])
            return false;
    return true;
}
// The serializer's own rule (docs/re/build.md §12), first step: a stored chunk goes into the blob of every entry its 16 m
// box [16c, 16c + 16] touches, edges included.
bool chunk_touches_region(const std::array<int32_t, 3>& chunk, const RegionBounds& region) {
    if (!valid_region(region)) return false;
    constexpr double metres_per_chunk = 0.5 * kChunk;
    for (int axis = 0; axis < 3; ++axis) {
        const double lo = static_cast<double>(chunk[axis]) * metres_per_chunk;
        const double hi = lo + metres_per_chunk;
        if (hi < region.lo[axis] || lo > region.hi[axis]) return false;
    }
    return true;
}
// Second step: of such a chunk, only the 4 m blocks whose own box touches the entry's bounds (edges included) keep their
// cells in that blob; the others are written empty, and the loader leaves an empty block as the world generates it. Bit
// x + 4y + 16z for block (x, y, z), as the serializer walks them. The values are whole metres and the entry's floats,
// so these comparisons in double give the game's float results.
uint64_t blocks_saved_by(const std::array<int32_t, 3>& chunk, const RegionBounds& region) {
    if (!chunk_touches_region(chunk, region)) return 0;
    constexpr double metres_per_block = 0.5 * kBlock;
    unsigned touched[3] = {};  // per axis: the block rows that touch the bounds
    for (int axis = 0; axis < 3; ++axis) {
        const double first = static_cast<double>(chunk[axis]) * (0.5 * kChunk);
        for (int i = 0; i < kBlocks; ++i) {
            const double lo = first + i * metres_per_block, hi = lo + metres_per_block;
            if (lo <= region.hi[axis] && region.lo[axis] <= hi) touched[axis] |= 1u << i;
        }
    }
    uint64_t saved = 0;
    for (int z = 0; z < kBlocks; ++z)
        for (int y = 0; y < kBlocks; ++y)
            for (int x = 0; x < kBlocks; ++x)
                if ((touched[0] >> x & 1) && (touched[1] >> y & 1) && (touched[2] >> z & 1))
                    saved |= uint64_t{1} << (x + kBlocks * y + kBlocks * kBlocks * z);
    return saved;
}
// The 16 m chunk positions an entry's bounds touch, edges included (the serializer's first step): chunk c of an axis
// touches [lo, hi] when 16c <= hi and lo <= 16c + 16. The most stored chunks the voxel writer can collect for it.
double chunk_positions(const RegionBounds& b) {
    double n = 1;
    for (int axis = 0; axis < 3; ++axis) n *= std::floor(b.hi[axis] / 16.0) - std::ceil(b.lo[axis] / 16.0) + 2;
    return n;
}
// Every block of the chunk is in the blob of some entry: the loader applies each blob's blocks and skips its empty ones,
// so blocks two bases save between them count too. An entry whose bounds touch more chunk positions than the writer
// collects may lose any of its chunks (a zone a mod made larger than 240 m, or exactly 240 m with faces on the 16 m
// grid): it counts as saving none, and Kiln's kept chunks in it keep their boxes.
bool chunk_saved_whole(const std::array<int32_t, 3>& chunk, const std::vector<RegionBounds>& native) {
    uint64_t saved = 0;
    for (const RegionBounds& b : native)
        if (chunk_positions(b) <= kWriterChunks && (saved |= blocks_saved_by(chunk, b)) == kWholeChunk) return true;
    return false;
}
// The kept chunks the game's entries do not save whole: those outside every base, and those on a base's edge that reach
// outside it (Kiln knows only which 16 m chunks its edits touched, not where in them). They stay in g_chunks and the
// JSON either way: a base can move, shrink, be abandoned or go, and the next save then needs Kiln's boxes for them again.
using ChunkSet = std::set<std::array<int32_t, 3>>;
ChunkSet chunks_outside_regions(const ChunkSet& chunks, const std::vector<RegionBounds>& native) {
    ChunkSet outside;
    for (const auto& chunk : chunks)
        if (!chunk_saved_whole(chunk, native)) outside.insert(chunk);
    return outside;
}
RegionCoverage classify_chunks(const ChunkSet& chunks, const std::vector<RegionBounds>& native) {
    const auto outside = chunks_outside_regions(chunks, native);
    return {chunks.size() - outside.size(), outside.size()};
}

// Who called the builder: its return address against the three call sites sigdb resolves (0 = not resolved).
enum class Caller { WorldSave, BaseZones, BaseQuery, Unknown };
struct CallSites {
    uintptr_t world_save = 0, base_zones = 0, base_query = 0;
};
Caller classify_caller(uintptr_t ret, const CallSites& s) {
    if (ret && ret == s.world_save) return Caller::WorldSave;
    if (ret && ret == s.base_zones) return Caller::BaseZones;
    if (ret && ret == s.base_query) return Caller::BaseQuery;
    return Caller::Unknown;
}
// Kiln's boxes go where the game saves terrain. With the world save's call site resolved, only the world save gets
// them: the base zones, the base query and a caller Kiln does not know (another mod's hook in front of Kiln's) get the
// list as the game's builder wrote it, whether their own call sites resolved or not. Without it Kiln cannot tell the
// save from the other callers, so every caller gets them, as up to 0.2.0-dev.30.8: extra Flame Altars and base zones
// (the game's base grouping goes wrong past 16 zones) are better than edits the save drops.
bool kiln_boxes_for(Caller c, const CallSites& s) { return !s.world_save || c == Caller::WorldSave; }

// What kiln.build says when it starts about the three call sites: a line in kiln.log (a warning when a name did not
// resolve) and the text of its kiln.caps row, which is unavailable when Kiln's boxes reach every caller.
constexpr char kCallersRow[] = "build.persist_callers";
struct SitesNotice {
    bool warn = false, available = true;
    std::string text;
};
SitesNotice sites_notice(const CallSites& s) {
    const std::pair<const char*, uintptr_t> names[] = {{"build/RetSaveRegionsWorldSave", s.world_save},
                                                       {"build/RetSaveRegionsBaseZones", s.base_zones},
                                                       {"build/RetSaveRegionsBaseQuery", s.base_query}};
    std::string missing;
    for (const auto& [name, at] : names)
        if (!at) missing += (missing.empty() ? "" : ", ") + std::string(name);
    if (missing.empty())
        return {false, true,
                "the save-region builder's three call sites resolved: Kiln's save boxes go into the world save's region "
                "list only"};
    if (s.world_save)
        return {true, true,
                "sigdb names not resolved on this game build: " + missing + ". Kiln's save boxes still go into the world "
                "save's region list only; every other caller of the save-region builder gets none"};
    return {true, false,
            "sigdb names not resolved on this game build: " + missing + ". Kiln cannot tell the world save from the "
            "other callers of the save-region builder, so every caller gets Kiln's save boxes, as up to "
            "0.2.0-dev.30.8: each box is one more Flame Altar in the world list and a base zone the game's systems "
            "group stations, storage and beds by"};
}
#ifndef KILN_REGIONS_HOST_TEST
void report_callers_row(bool available, const std::string& text) {
    const auto* caps = static_cast<const KilnCapsApi*>(g_api->service(kstr(KILN_CAPS_SERVICE), KILN_CAPS_VERSION));
    if (!caps || caps->header.size < offsetof(KilnCapsApi, report) + sizeof(void*) || !caps->report) return;
    caps->report(g_owner, kstr(kCallersRow), available ? KILN_CAP_AVAILABLE : KILN_CAP_UNAVAILABLE,
                 available ? KILN_CAP_REASON_NONE : KILN_CAP_REASON_SYMBOL_MISSING, kstr(text));
}
#endif
void announce_sites(const CallSites& s) {
    const SitesNotice n = sites_notice(s);
    if (n.warn) KB_W("save regions: %s", n.text.c_str());
    else KB_I("save regions: %s", n.text.c_str());
    report_callers_row(n.available, n.text);
}

#ifndef KILN_REGIONS_HOST_TEST
kiln::RawHook g_hook;
#endif
std::mutex g_mu;
std::set<std::array<int32_t, 3>> g_chunks;  // Kiln-kept chunks (cell coordinates / 32)
std::string g_world;                         // world GUID text the set belongs to ("" = unknown yet)
bool g_dirty = false;
bool g_lifecycle_ok = false;
std::filesystem::path g_dir;                 // kiln/data/kiln.build/regions
bool g_boxes_stale = true;                   // g_chunks changed since the last plan
std::filesystem::path g_file;
#ifndef KILN_REGIONS_HOST_TEST
const KilnSavesApi* g_saves = nullptr;
#endif
CallSites g_sites;  // set once by regions_install, before the hook can run
std::atomic<uint64_t> g_saves_seen{0}, g_boxes_added{0};
std::atomic<bool> g_unknown_told{false}, g_header_told{false};

// What one save needs from Kiln, and the last one logged (under g_mu).
struct SavePlan {
    std::vector<Box> boxes;
    size_t kept = 0, covered = 0;  // kept chunks; those the game's entries of the same list save whole already
    bool bounds = false;           // false: the game's entries could not be read, every kept chunk has a box
};
struct LoggedPlan {
    uint64_t added = ~uint64_t{0}, wanted = 0, kept = 0, covered = 0, entries = 0;
    bool bounds = false;
    bool operator==(const LoggedPlan&) const = default;
};
struct PlanCache {
    bool valid = false, bounds = false;
    std::vector<RegionBounds> native;
    SavePlan plan;
};
PlanCache g_plan;
LoggedPlan g_last_log;

#ifndef KILN_REGIONS_HOST_TEST
std::string guid_text(const uint8_t g[16]) {
    char b[40];
    // Windows GUID text: first three groups little-endian
    std::snprintf(b, sizeof b, "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x", g[3], g[2], g[1],
                  g[0], g[5], g[4], g[7], g[6], g[8], g[9], g[10], g[11], g[12], g[13], g[14], g[15]);
    return b;
}

std::string current_world() {
    if (!g_saves) return "";
    KilnSavesStatus st{};
    st.size = sizeof st;
    if (g_saves->status(&st) != KILN_OK || !st.has_guid) return "";
    return guid_text(st.world_guid);
}

#endif

// One file per world: kiln/data/kiln.build/regions/<world GUID>.json (a game client hosts several worlds).
void read_file_locked() {
    std::error_code ec;
    if (g_file.empty() || !std::filesystem::exists(g_file, ec)) return;
    std::ifstream f(g_file);
    std::stringstream ss;
    ss << f.rdbuf();
    const std::string s = ss.str();
    // tiny hand parser: {"format":1,"world":"<guid>","chunks":[x,y,z,x,y,z,...]}
    const size_t p = s.find("\"chunks\"");
    if (p == std::string::npos) return;
    const size_t a = s.find('[', p), e = a == std::string::npos ? a : s.find(']', a);
    if (e == std::string::npos) return;
    std::vector<int32_t> v;
    std::stringstream in(s.substr(a + 1, e - a - 1));
    for (std::string tok; std::getline(in, tok, ',');) {
        char* end = nullptr;
        const long n = std::strtol(tok.c_str(), &end, 10);
        if (end != tok.c_str()) v.push_back(static_cast<int32_t>(n));
    }
    for (size_t i = 0; i + 2 < v.size(); i += 3) g_chunks.insert({v[i], v[i + 1], v[i + 2]});
}

void save_locked() {
    if (!g_dirty || g_file.empty()) return;
    std::string s = "{\n  \"format\": 1,\n  \"world\": \"" + g_world + "\",\n  \"chunk_cells\": 32,\n  \"chunks\": [";
    bool first = true;
    for (const auto& c : g_chunks) {
        s += first ? "" : ",";
        s += std::to_string(c[0]) + "," + std::to_string(c[1]) + "," + std::to_string(c[2]);
        first = false;
    }
    s += "]\n}\n";
    std::error_code ec;
    std::filesystem::create_directories(g_file.parent_path(), ec);
    std::filesystem::path tmp = g_file;
    tmp += L".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        f << s;
        f.flush();
        if (!f) return;
    }
#ifdef KILN_REGIONS_HOST_TEST
    std::filesystem::rename(tmp, g_file, ec);
    if (ec) return;
#else
    if (!MoveFileExW(tmp.c_str(), g_file.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) return;
#endif
    g_dirty = false;
}

// End a session even if it never acquired an identity. Never carry its pending chunks into another world.
void close_locked() {
    save_locked();  // same best-effort atomic flush used on a known-identity switch
    g_chunks.clear();
    g_world.clear();
    g_file.clear();
    g_dirty = false;
    g_boxes_stale = true;
    g_last_log = {};
}

void KILN_CALL on_world(const KilnWorldEvent* ev, void*) {
    if (ev->kind != KILN_WORLD_SERVER || ev->event != KILN_WORLD_STOPPING) return;
    terrain_budget_reset();
    vox_world_closed();
    transient_world_closed();
    block_world_closed();
    std::lock_guard lock(g_mu);
    close_locked();  // independent of the order kiln.saves receives the same event
}

// Binds the set to the running world (kiln.saves' GUID): a different world saves the old set and loads its own.
// Chunks kept before the GUID is known (a brand-new world before its first save) move into the world's set.
void load_locked() {
    const std::string w = current_world();
    if (w == g_world) return;
    if (w.empty()) {
        close_locked();
        return;
    }
    std::set<std::array<int32_t, 3>> pending;
    if (g_world.empty()) pending.swap(g_chunks);
    else save_locked();
    g_chunks.clear();
    g_world = w;
    g_file = g_dir / (w + ".json");
    read_file_locked();
    const size_t loaded = g_chunks.size();
    for (const auto& c : pending) g_chunks.insert(c);
    g_dirty = g_chunks.size() != loaded;
    g_boxes_stale = true;
    g_last_log = {};
    KB_I("save regions: %zu chunk(s) kept for world %s (%s)", loaded, w.c_str(), reinterpret_cast<const char*>(g_file.u8string().c_str()));
}

// A Kiln box is a record of its own, and its bounds sit 0.05 m inside its chunks, so the chunks it covers are the
// positions it touches: a box over the voxel writer's 4,096 is cut into slabs across its longest axis, as thick as the
// other two axes allow (a slab still too large is cut again).
void split_for_writer(const Box& b, std::vector<Box>& out) {
    double extent[3], volume = 1;
    for (int k = 0; k < 3; ++k) volume *= extent[k] = static_cast<double>(b.hi[k]) - b.lo[k] + 1;
    if (volume <= kWriterChunks) {
        out.push_back(b);
        return;
    }
    const int axis = extent[0] >= extent[1] && extent[0] >= extent[2] ? 0 : extent[1] >= extent[2] ? 1 : 2;
    const double cross = volume / extent[axis];
    const int64_t step = cross >= kWriterChunks ? 1 : static_cast<int64_t>(kWriterChunks / cross);
    for (int64_t lo = b.lo[axis]; lo <= b.hi[axis]; lo += step) {
        Box slab = b;
        slab.lo[axis] = static_cast<int32_t>(lo);
        slab.hi[axis] = static_cast<int32_t>(std::min<int64_t>(lo + step - 1, b.hi[axis]));
        split_for_writer(slab, out);
    }
}

// Greedy boxes over the chunk set: runs along x, merged across z when equal, then across y; none over 4,096 chunks.
std::vector<Box> boxes_for_chunks(const ChunkSet& chunks) {
    std::map<std::pair<int32_t, int32_t>, std::vector<int32_t>> rows;  // (y, z) -> sorted x
    for (const auto& c : chunks) rows[{c[1], c[2]}].push_back(c[0]);
    std::vector<Box> runs;
    for (auto& [yz, xs] : rows) {
        std::sort(xs.begin(), xs.end());
        for (size_t i = 0; i < xs.size();) {
            size_t j = i;
            while (j + 1 < xs.size() && xs[j + 1] == xs[j] + 1) ++j;
            runs.push_back(Box{{xs[i], yz.first, yz.second}, {xs[j], yz.first, yz.second}});
            i = j + 1;
        }
    }
    auto merge_axis = [](std::vector<Box> in, int axis) {
        std::sort(in.begin(), in.end(), [&](const Box& a, const Box& b) {
            for (int k = 0; k < 3; ++k) {
                if (k == axis) continue;
                if (a.lo[k] != b.lo[k]) return a.lo[k] < b.lo[k];
                if (a.hi[k] != b.hi[k]) return a.hi[k] < b.hi[k];
            }
            return a.lo[axis] < b.lo[axis];
        });
        std::vector<Box> out;
        for (const Box& b : in) {
            if (!out.empty()) {
                Box& l = out.back();
                bool same = true;
                for (int k = 0; k < 3; ++k)
                    if (k != axis && (l.lo[k] != b.lo[k] || l.hi[k] != b.hi[k])) same = false;
                if (same && l.hi[axis] + 1 == b.lo[axis]) {
                    l.hi[axis] = b.hi[axis];
                    continue;
                }
            }
            out.push_back(b);
        }
        return out;
    };
    runs = merge_axis(std::move(runs), 2);
    runs = merge_axis(std::move(runs), 1);
    // far too many boxes (scattered single edits): fall back to one bounding box per 8x8 chunk area (128 m)
    if (runs.size() > 64) {
        std::map<std::pair<int32_t, int32_t>, Box> coarse;
        for (const Box& b : runs) {
            auto key = std::make_pair(b.lo[0] >> 3, b.lo[2] >> 3);
            auto [it, fresh] = coarse.emplace(key, b);
            if (!fresh)
                for (int k = 0; k < 3; ++k) it->second.lo[k] = std::min(it->second.lo[k], b.lo[k]),
                                            it->second.hi[k] = std::max(it->second.hi[k], b.hi[k]);
        }
        runs.clear();
        for (auto& [k, b] : coarse) runs.push_back(b);
    }
    std::vector<Box> boxes;
    for (const Box& b : runs) split_for_writer(b, boxes);
    return boxes;
}
std::vector<Box> boxes_locked() { return boxes_for_chunks(g_chunks); }

bool same_bounds(const std::vector<RegionBounds>& a, const std::vector<RegionBounds>& b) {
    return a.size() == b.size() && (a.empty() || std::memcmp(a.data(), b.data(), a.size() * sizeof(RegionBounds)) == 0);
}
// The boxes this save needs: over the kept chunks the game's entries do not save whole, or over every kept chunk when the
// entries could not be read (`native` null). Kept until the chunk set or the entries' bounds change.
const SavePlan& plan_locked(const std::vector<RegionBounds>* native) {
    const bool bounds = native != nullptr;
    if (g_boxes_stale || !g_plan.valid || g_plan.bounds != bounds || (bounds && !same_bounds(g_plan.native, *native))) {
        SavePlan p;
        p.kept = g_chunks.size();
        p.bounds = bounds;
        if (bounds) {
            const ChunkSet outside = chunks_outside_regions(g_chunks, *native);
            p.covered = p.kept - outside.size();
            p.boxes = boxes_for_chunks(outside);
            g_plan.native = *native;
        } else {
            p.boxes = boxes_locked();
            g_plan.native.clear();
        }
        g_plan.plan = std::move(p);
        g_plan.bounds = bounds;
        g_plan.valid = true;
        g_boxes_stale = false;
    }
    return g_plan.plan;
}

struct GameArray {
    uint8_t* data;
    uint64_t count;
    uint64_t capacity;
    bool (*grow)(GameArray*, int64_t, uint8_t);
};

// Appends one region; false when the array cannot grow.
bool append(GameArray* a, const Box& b, uint32_t index) {
    if (a->capacity < a->count + 1) {
        if (!a->grow || !a->grow(a, -static_cast<int64_t>(a->count + 1), 1)) return false;
    }
    uint8_t* e = a->data + a->count * kEntry;
    std::memset(e, 0, kEntry);
    const uint32_t id[4] = {0x4e4c494bu /* "KILN" */, 0x444c4942u /* "BILD" */, index, 0x6b69726eu};
    std::memcpy(e, id, 16);
    const double cs = 0.5 * kChunk;  // metres per chunk
    // a hair inside the chunk box: the serializer counts touching chunks as overlapping
    const float lo[3] = {static_cast<float>(b.lo[0] * cs + 0.05), static_cast<float>(b.lo[1] * cs + 0.05),
                         static_cast<float>(b.lo[2] * cs + 0.05)};
    const float hi[3] = {static_cast<float>((b.hi[0] + 1) * cs - 0.05), static_cast<float>((b.hi[1] + 1) * cs - 0.05),
                         static_cast<float>((b.hi[2] + 1) * cs - 0.05)};
    std::memcpy(e + 0x10, lo, sizeof lo);
    std::memcpy(e + 0x20, hi, sizeof hi);
    ++a->count;
    return true;
}

struct HookCtx {
    GameArray* list;
    const std::vector<Box>* boxes;
    uint32_t added;
};
void append_all(void* p) {
    auto* c = static_cast<HookCtx*>(p);
    for (uint32_t i = 0; i < c->boxes->size(); ++i) {
        if (!append(c->list, (*c->boxes)[i], i)) break;
        ++c->added;
    }
}

// Guards around game memory. No frame here holds a C++ object with a destructor, as __try requires.
#ifdef _WIN32
bool guarded_copy(void* dst, const void* src, size_t n) {
    __try {
        std::memcpy(dst, src, n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
bool guarded_append(HookCtx* c) {
    __try {
        append_all(c);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
#else  // the Linux host test: no structured exceptions there, and no game memory either
bool guarded_copy(void* dst, const void* src, size_t n) {
    std::memcpy(dst, src, n);
    return true;
}
bool guarded_append(HookCtx* c) {
    append_all(c);
    return true;
}
#endif

// The game's own entries, the ones its builder just wrote; false when the list is implausible or cannot be read.
bool read_game_entries(const GameArray& head, std::vector<RegionBounds>* out) {
    out->clear();
    if (head.count > kMaxGameEntries || head.count > head.capacity || (head.count && !head.data)) return false;
    std::vector<uint8_t> raw(static_cast<size_t>(head.count) * kEntry);
    if (!raw.empty() && !guarded_copy(raw.data(), head.data, raw.size())) return false;
    for (uint64_t i = 0; i < head.count; ++i) {
        RegionBounds b{};
        std::memcpy(b.lo, raw.data() + i * kEntry + 0x10, sizeof b.lo);
        std::memcpy(b.hi, raw.data() + i * kEntry + 0x20, sizeof b.hi);
        if (!valid_region(b)) return false;
        out->push_back(b);
    }
    return true;
}

// After the game's builder returned `r` and `list`: into the world save's list (into every caller's when the world
// save's call site did not resolve), after the game's entries, Kiln's boxes for the kept chunks the game's entries do
// not save whole. Lists for anyone else stay untouched.
void after_save_regions(uintptr_t ret, const uint8_t* r, GameArray* list, const CallSites& sites) {
    const Caller kind = classify_caller(ret, sites);
    if (kind == Caller::Unknown && sites.world_save && sites.base_zones && sites.base_query && F.persist &&
        !g_unknown_told.exchange(true))
        KB_W("save regions: the save-region builder was called from 0x%llx, none of its three known callers (another "
             "mod's hook in front of Kiln's?): Kiln adds no boxes to that list; if it was the world save, Kiln's edits "
             "outside bases are not in it", static_cast<unsigned long long>(ret));
    if (!kiln_boxes_for(kind, sites)) return;
    if (!regions_lifecycle_safe() || !F.persist) return;
    if (!r || r[0] != 0 || !list) return;  // the game failed itself: leave it alone
    GameArray head{};
    if (!guarded_copy(&head, list, sizeof head)) {
        if (!g_header_told.exchange(true))
            KB_E("save regions: the game's region list could not be read; this save keeps only the game's own regions");
        return;
    }
    std::vector<RegionBounds> native;
    const bool bounds = read_game_entries(head, &native);
    std::vector<Box> boxes;
    LoggedPlan now;
    {
        std::lock_guard lock(g_mu);
        load_locked();
        if (g_world.empty() || g_chunks.empty()) return;
        const SavePlan& p = plan_locked(bounds ? &native : nullptr);
        boxes = p.boxes;
        now.wanted = p.boxes.size();
        now.kept = p.kept;
        now.covered = p.covered;
        now.bounds = p.bounds;
        if (g_dirty) save_locked();
    }
    now.entries = head.count;
    HookCtx c{list, &boxes, 0};
    if (!boxes.empty() && !guarded_append(&c)) {
        list->count = head.count;  // never hand the game a half-written entry
        KB_E("save regions: appending faulted; this save keeps only the game's own regions");
        return;
    }
    g_boxes_added.fetch_add(c.added, std::memory_order_relaxed);
    now.added = c.added;
    bool tell = false;
    {
        std::lock_guard lock(g_mu);
        if (!(now == g_last_log)) {
            g_last_log = now;
            tell = true;
        }
    }
    if (!tell) return;
    if (c.added < boxes.size())
        KB_W("save regions: the game's list took only %u of %zu Kiln box(es); %llu kept chunk(s) its %llu base "
             "region(s) do not save whole may not be in this save", c.added, boxes.size(),
             static_cast<unsigned long long>(now.kept - now.covered), static_cast<unsigned long long>(head.count));
    else if (!now.bounds)
        KB_I("save regions: the game's save now writes %u Kiln box(es) for all %llu kept chunk(s) after its %llu base "
             "region(s), whose bounds could not be read", c.added, static_cast<unsigned long long>(now.kept),
             static_cast<unsigned long long>(head.count));
    else
        KB_I("save regions: the game's save now writes %u Kiln box(es) for %llu kept chunk(s) its %llu base region(s) "
             "do not save whole; the other %llu are saved with the bases", c.added,
             static_cast<unsigned long long>(now.kept - now.covered), static_cast<unsigned long long>(head.count),
             static_cast<unsigned long long>(now.covered));
}

#ifndef KILN_REGIONS_HOST_TEST
uint8_t* __fastcall hk_save_regions(uint8_t* result, GameArray* list, uintptr_t zones) {
    const uintptr_t ret = g_hook.caller();  // the game's call site: valid only before the original runs
    uint8_t* r = g_hook.call<uint8_t*>(result, list, zones);
    g_saves_seen.fetch_add(1, std::memory_order_relaxed);
    // for the world save the voxel serializer runs next, from this list; transient cells are on disk already (journaled
    // before their commit): this only compacts the journal, whoever called
    try {
        transient_before_save();
    } catch (...) {
    }
    try {
        after_save_regions(ret, r, list, g_sites);
    } catch (...) {
        // nothing may escape into the game's save
    }
    return r;
}
#endif

}  // namespace

#ifndef KILN_REGIONS_HOST_TEST
bool regions_install() {
    if (!F.persist) return false;
    char buf[1024];
    size_t len = 0;
    if (g_api->path(KILN_PATH_DATA, buf, sizeof buf, &len) == KILN_OK)
        g_dir = std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(buf), len)) / "kiln.build" / "regions";  // UTF-8
    uintptr_t a = 0;
    if (g_api->symbol(kstr("build/SaveRegions"), &a) != KILN_OK || !a) return F.persist = false;
    // The builder's three callers (docs/re/build.md §12): only the world save gets Kiln's boxes, or every caller when the
    // world save's site does not resolve. Read before the hook is installed.
    auto site = [](const char* name) {
        uintptr_t v = 0;
        return g_api->symbol(kstr(name), &v) == KILN_OK ? v : uintptr_t{0};
    };
    g_sites.world_save = site("build/RetSaveRegionsWorldSave");
    g_sites.base_zones = site("build/RetSaveRegionsBaseZones");
    g_sites.base_query = site("build/RetSaveRegionsBaseQuery");
    if (!g_hook.install(g_api, g_owner, a, reinterpret_cast<void*>(&hk_save_regions), "build/SaveRegions")) {
        KB_E("hooking the save-region builder failed: Kiln edits outside bases are not saved");
        return F.persist = false;
    }
    announce_sites(g_sites);
    return true;
}

void regions_lifecycle_ready(bool subscribed, bool observed);

void regions_ready() {
    g_saves = static_cast<const KilnSavesApi*>(g_api->service(kstr(KILN_SAVES_SERVICE), 1));
    KilnHandle h{};
    const bool subscribed = g_api->subscribe_world(g_owner, &on_world, nullptr, &h) == KILN_OK;
    KilnSessionInfo session{};
    session.size = sizeof session;
    regions_lifecycle_ready(subscribed, g_api->session && g_api->session(&session) == KILN_OK && session.lifecycle);
    if (!g_lifecycle_ok) KB_W("persistent terrain writes require world.lifecycle and a world-stop subscription");
    std::lock_guard lock(g_mu);
    load_locked();
}

#endif

void regions_lifecycle_ready(bool subscribed, bool observed) {
    g_lifecycle_ok = subscribed && observed;
    if (!g_lifecycle_ok) {
        F.persist = false;
        set_detail("persistent terrain writes require world.lifecycle and a world-stop subscription");
    }
}

bool regions_lifecycle_safe() {
    if (!g_lifecycle_ok) {
        set_detail("persistent terrain writes require world.lifecycle and a world-stop subscription");
        return false;
    }
#ifndef KILN_REGIONS_HOST_TEST
    KilnSessionInfo session{};
    session.size = sizeof session;
    if (g_lifecycle_ok && (!g_api->session || g_api->session(&session) != KILN_OK || !session.lifecycle)) {
        set_detail("persistent terrain writes require world.lifecycle and a world-stop subscription");
        return false;
    }
#endif
    return g_lifecycle_ok;
}

void regions_keep(const int32_t origin[3], const uint32_t dims[3]) {
    if (!F.persist) return;
    std::lock_guard lock(g_mu);
    load_locked();
    const size_t before = g_chunks.size();
    auto chunk = [](int64_t c) { return static_cast<int32_t>(c >= 0 ? c / kChunk : -((-c + kChunk - 1) / kChunk)); };
    for (int32_t z = chunk(origin[2]); z <= chunk(static_cast<int64_t>(origin[2]) + dims[2] - 1); ++z)
        for (int32_t y = chunk(origin[1]); y <= chunk(static_cast<int64_t>(origin[1]) + dims[1] - 1); ++y)
            for (int32_t x = chunk(origin[0]); x <= chunk(static_cast<int64_t>(origin[0]) + dims[0] - 1); ++x)
                g_chunks.insert({x, y, z});
    if (g_chunks.size() != before) g_dirty = g_boxes_stale = true;
}

size_t regions_release(const int32_t origin[3], const uint32_t dims[3]) {
    std::lock_guard lock(g_mu);
    load_locked();
    size_t n = 0;
    for (auto it = g_chunks.begin(); it != g_chunks.end();) {
        const auto& c = *it;
        bool in = true;
        for (int a = 0; a < 3; ++a) {
            const int64_t lo = static_cast<int64_t>(c[a]) * kChunk, hi = lo + kChunk;
            if (hi <= origin[a] || lo >= static_cast<int64_t>(origin[a]) + dims[a]) in = false;
        }
        if (in) {
            it = g_chunks.erase(it);
            ++n;
        } else {
            ++it;
        }
    }
    if (n) g_dirty = g_boxes_stale = true;
    return n;
}

void regions_flush() {
    std::lock_guard lock(g_mu);
    load_locked();
    if (g_dirty) save_locked();
}

#ifndef KILN_REGIONS_HOST_TEST
std::string regions_world() { return current_world(); }

bool regions_save_running() {
    if (!g_saves) return false;
    KilnSavesStatus st{};
    st.size = sizeof st;
    return g_saves->status(&st) == KILN_OK && (st.features & KILN_SAVES_CAN_LOAD) &&
        (st.state == KILN_SAVES_STATE_RUNNING || st.state == KILN_SAVES_STATE_SAVING);
}

bool regions_saves_written(uint64_t* written) {
    if (!g_saves) return false;
    KilnSavesStatus st{};
    st.size = sizeof st;
    if (g_saves->status(&st) != KILN_OK) return false;
    *written = st.written;
    return true;
}

#endif

void regions_status(uint64_t* chunks, uint64_t* saves, uint64_t* boxes) {
    std::lock_guard lock(g_mu);
    *chunks = g_chunks.size();
    *saves = g_saves_seen.load();
    *boxes = g_boxes_added.load();
}

}  // namespace kb
