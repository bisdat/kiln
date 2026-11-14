// SPDX-License-Identifier: MIT
// Transient edits (KILN_BUILD_F_TRANSIENT) are gone after a restart.
//
// Skipping Kiln's own save region is not enough for that: the game's save writes every stored 32-cell chunk that
// overlaps a region (docs/re/build.md section 5), so a transient cell inside a player base, or in a chunk another (kept)
// edit made Kiln keep, is saved with the world. Undoing the cells before a save would show every player the terrain
// flickering, and the save itself has no hook that could leave them out. So the save may write them, and Kiln puts the
// earlier content back at the next start, before anyone needs it:
//  - Every transient write journals the cells it changed: per 8x8x8 block, the cells' value before the first transient
//    write of this run of transient writes ("before") and after the newest ("after"). A kept write (any write without
//    the flag) takes the cells it sets out of the journal: they are meant to stay.
//  - The journal is kiln/data/kiln.build/transient/<world GUID>.bin, written atomically (temp file, then rename) when it
//    changed: at the save-region hook, which the game calls right before its voxel serializer reads the store, so a
//    save that can hold a transient cell finds it on record first; every 24 steps; and at once when the hook is not in
//    place.
//  - At the next start (the world runs, its save is loaded into the store), every journaled cell that still holds its
//    "after" value gets its "before" value back, block by block as the tiles load, through the same store commit as any
//    edit (so a joined client sees it too). A cell that holds anything else was changed since (a player dug it, or the
//    save never kept it) and is left as it is.
//  - Put-back cells stay on record until kiln.saves has seen two world files written after the put-back, so a crash
//    before the next save does not lose them; then they leave the journal.
// Cells a transient write sets again in the new session become that session's (an earlier copy is not put back over
// them). A brand-new world has no GUID before its first save: its journal stays in memory until kiln.saves knows it.
// Tick thread, except transient_before_save (the game's save thread); one mutex.
#include <windows.h>

#include <array>
#include <bit>
#include <filesystem>
#include <fstream>
#include <map>

#include "internal.h"
#include "services/kiln_saves.h"

namespace kb {
namespace {

constexpr uint32_t kCells = kBlock * kBlock * kBlock;  // 512 cells per block
constexpr size_t kMaxBlocks = 8192;                    // ~17 MB of journal, 4 M cells
constexpr uint32_t kBlocksPerStep = 32;                // put back at most this many blocks a step
constexpr char kMagic[8] = {'K', 'L', 'N', 'T', 'R', 'J', '0', '1'};

using Key = std::array<int32_t, 3>;  // block origin, in cells
struct Bits {
    uint64_t w[8] = {};
    bool get(uint32_t i) const { return (w[i >> 6] >> (i & 63)) & 1; }
    void set(uint32_t i) { w[i >> 6] |= 1ull << (i & 63); }
    void clear(uint32_t i) { w[i >> 6] &= ~(1ull << (i & 63)); }
    bool any() const {
        for (uint64_t x : w)
            if (x) return true;
        return false;
    }
    void minus(const Bits& o) {
        for (int i = 0; i < 8; ++i) w[i] &= ~o.w[i];
    }
};
struct Entry {
    Bits mask;      // journaled cells
    Bits pending;   // from an earlier session, not checked yet (part of mask)
    Bits restored;  // put back in this session (part of mask), until two saves were written
    KilnVoxel before[kCells];
    KilnVoxel after[kCells];
};

std::mutex g_mu;
std::map<Key, Entry> g_j;
std::string g_jworld;                 // GUID text the journal belongs to ("" = not known yet)
std::filesystem::path g_dir, g_file;
bool g_dirty = false;
size_t g_pending_blocks = 0;         // entries with pending cells
Key g_cursor{INT32_MIN, INT32_MIN, INT32_MIN};
uint64_t g_restored_at = UINT64_MAX;  // kiln.saves' written count when the put-back finished
uint64_t g_put_back = 0, g_left = 0;  // cells put back / left (changed since) in this session's put-back
bool g_logged_start = false;

uint32_t index_of(uint32_t x, uint32_t y, uint32_t z) { return (z * kBlock + y) * kBlock + x; }

uint64_t fnv64(const void* p, size_t n, uint64_t h = 1469598103934665603ull) {
    const auto* b = static_cast<const uint8_t*>(p);
    for (size_t i = 0; i < n; ++i) h = (h ^ b[i]) * 1099511628211ull;
    return h;
}

void count_pending_locked() {
    g_pending_blocks = 0;
    for (const auto& [k, e] : g_j)
        if (e.pending.any()) ++g_pending_blocks;
}

// File: magic[8], u32 format 1, u32 count, count x {i32 x, y, z; u64 mask[8]; u16 before[512]; u16 after[512]},
// u64 FNV-1a of everything before it. Only masked cells mean anything.
bool read_file_locked(std::map<Key, Entry>& out) {
    std::error_code ec;
    if (g_file.empty() || !std::filesystem::exists(g_file, ec)) return true;
    std::ifstream f(g_file, std::ios::binary);
    std::string s((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    constexpr size_t rec = 12 + 64 + 2 * 2 * kCells;
    uint32_t fmt = 0, count = 0;
    if (s.size() < 24 || std::memcmp(s.data(), kMagic, 8) != 0) return false;
    std::memcpy(&fmt, s.data() + 8, 4);
    std::memcpy(&count, s.data() + 12, 4);
    if (fmt != 1 || count > kMaxBlocks || s.size() != 16 + static_cast<size_t>(count) * rec + 8) return false;
    uint64_t sum = 0;
    std::memcpy(&sum, s.data() + s.size() - 8, 8);
    if (sum != fnv64(s.data(), s.size() - 8)) return false;
    const char* p = s.data() + 16;
    for (uint32_t i = 0; i < count; ++i, p += rec) {
        Key k;
        std::memcpy(k.data(), p, 12);
        if (k[0] % static_cast<int32_t>(kBlock) || k[1] % static_cast<int32_t>(kBlock) || k[2] % static_cast<int32_t>(kBlock))
            return false;
        Entry& e = out[k];
        std::memcpy(e.mask.w, p + 12, 64);
        std::memcpy(e.before, p + 76, 2 * kCells);
        std::memcpy(e.after, p + 76 + 2 * kCells, 2 * kCells);
        e.pending = e.mask;  // every cell of an earlier session is checked once
    }
    return true;
}

void save_locked() {
    if (!g_dirty || g_file.empty()) return;
    std::error_code ec;
    if (g_j.empty()) {
        std::filesystem::remove(g_file, ec);
        g_dirty = false;
        return;
    }
    std::string s;
    s.reserve(16 + g_j.size() * (76 + 4 * kCells) + 8);
    s.append(kMagic, 8);
    const uint32_t fmt = 1, count = static_cast<uint32_t>(g_j.size());
    s.append(reinterpret_cast<const char*>(&fmt), 4);
    s.append(reinterpret_cast<const char*>(&count), 4);
    for (const auto& [k, e] : g_j) {
        s.append(reinterpret_cast<const char*>(k.data()), 12);
        s.append(reinterpret_cast<const char*>(e.mask.w), 64);
        s.append(reinterpret_cast<const char*>(e.before), 2 * kCells);
        s.append(reinterpret_cast<const char*>(e.after), 2 * kCells);
    }
    const uint64_t sum = fnv64(s.data(), s.size());
    s.append(reinterpret_cast<const char*>(&sum), 8);
    std::filesystem::create_directories(g_file.parent_path(), ec);
    std::filesystem::path tmp = g_file;
    tmp += L".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        f.write(s.data(), static_cast<std::streamsize>(s.size()));
        f.flush();
        if (!f) {
            KB_E("transient edits: the journal could not be written (%s)", reinterpret_cast<const char*>(tmp.u8string().c_str()));
            return;
        }
    }
    if (!MoveFileExW(tmp.c_str(), g_file.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        KB_E("transient edits: the journal could not be renamed into place (error %lu)", GetLastError());
        return;
    }
    g_dirty = false;
}

// Binds the journal to the running world (kiln.saves' GUID). What was journaled before the GUID was known (a new
// world before its first save) is this world's; another world's journal is saved first and dropped.
void bind_locked() {
    const std::string w = regions_world();
    if (w.empty() || w == g_jworld) return;
    std::map<Key, Entry> live;
    if (g_jworld.empty()) live.swap(g_j);
    else save_locked();
    g_j.clear();
    g_jworld = w;
    if (g_dir.empty()) {
        char buf[1024];
        size_t len = 0;
        if (g_api->path(KILN_PATH_DATA, buf, sizeof buf, &len) == KILN_OK)
            g_dir = std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(buf), len)) / "kiln.build" / "transient";
    }
    g_file = g_dir.empty() ? std::filesystem::path() : g_dir / (w + ".bin");
    if (!read_file_locked(g_j)) {
        g_j.clear();
        std::error_code ec;
        std::filesystem::path bad = g_file;
        bad += L".bad";
        std::filesystem::rename(g_file, bad, ec);
        KB_E("transient edits: the journal of world %s is unreadable; kept as %s, and transient edits of an earlier session "
             "are NOT put back", w.c_str(), reinterpret_cast<const char*>(bad.u8string().c_str()));
    }
    // this session's own cells (a new world's) go on top: they are not put back now
    for (auto& [k, e] : live) {
        auto [it, fresh] = g_j.try_emplace(k);
        Entry& d = it->second;
        if (fresh) {
            d = e;
            continue;
        }
        for (uint32_t i = 0; i < kCells; ++i) {
            if (!e.mask.get(i)) continue;
            if (!d.mask.get(i)) d.before[i] = e.before[i];
            d.after[i] = e.after[i];
            d.mask.set(i);
            d.pending.clear(i);
        }
    }
    g_dirty = !live.empty();
    count_pending_locked();
    g_restored_at = UINT64_MAX;
    g_put_back = g_left = 0;
    g_logged_start = false;
    g_cursor = {INT32_MIN, INT32_MIN, INT32_MIN};
    size_t cells = 0;
    for (const auto& [k, e] : g_j)
        for (uint64_t x : e.pending.w) cells += static_cast<size_t>(std::popcount(x));
    KB_I("transient edits: world %s, %zu journaled block(s), %zu cell(s) of an earlier session to check", w.c_str(),
         g_j.size(), cells);
}

// One block of an earlier session, on a copy of its entry and WITHOUT the journal's lock (the game's save thread takes
// it in the save-region hook, and the engine's reads and store commit must never wait behind that): cells that still
// hold what the transient write left get their earlier value back. `back` gets the cells put back, `gone` the cells
// that were changed since (or never saved). False when the block's tiles are not loaded yet (asked for; tried later).
bool put_back(const VoxelWorld& v, const Key& k, const Entry& e, Bits* back, Bits* gone) {
    const uint32_t dims[3] = {kBlock, kBlock, kBlock};
    if (!vox_ready_box(v, k.data(), dims, true)) return false;
    KilnVoxel cur[kCells];
    if (vox_read(k.data(), dims, cur, kCells) != KILN_OK) return false;
    for (uint32_t i = 0; i < kCells; ++i) {
        if (!e.pending.get(i)) continue;
        if (cur[i] == e.after[i] && cur[i] != e.before[i]) {
            cur[i] = e.before[i];
            back->set(i);
        } else if (cur[i] != e.before[i]) {
            gone->set(i);
        }
    }
    if (back->any() && !vox_commit_block(v, k.data(), cur)) {
        KB_E("transient edits: putting block %d,%d,%d back faulted; voxel writes are off", k[0], k[1], k[2]);
        *back = Bits{};
        return true;
    }
    if (back->any()) S.blocks_written.fetch_add(1, std::memory_order_relaxed);
    return true;
}

}  // namespace

KilnResult transient_admit(const int32_t wo[3], const uint32_t wd[3], const uint8_t* dirty_blocks) {
    std::lock_guard lock(g_mu);
    bind_locked();
    if (regions_world().empty() && !g_api->service(kstr(KILN_SAVES_SERVICE), 1)) {
        set_detail("transient edits need kiln.saves (the world's identity for their journal)");
        return KILN_ERR_UNAVAILABLE;
    }
    size_t fresh = 0;
    const uint32_t bx = wd[0] / kBlock, by = wd[1] / kBlock, bz = wd[2] / kBlock;
    for (uint32_t gz = 0; gz < bz; ++gz)
        for (uint32_t gy = 0; gy < by; ++gy)
            for (uint32_t gx = 0; gx < bx; ++gx) {
                if (!dirty_blocks[(static_cast<size_t>(gz) * by + gy) * bx + gx]) continue;
                const Key k{wo[0] + static_cast<int32_t>(gx * kBlock), wo[1] + static_cast<int32_t>(gy * kBlock),
                            wo[2] + static_cast<int32_t>(gz * kBlock)};
                if (!g_j.count(k)) ++fresh;
            }
    if (g_j.size() + fresh > kMaxBlocks) {
        set_detail("the transient journal is full (%zu blocks): transient edits are refused until a restart puts them back",
                   kMaxBlocks);
        return KILN_ERR_UNAVAILABLE;
    }
    return KILN_OK;
}

void transient_note(const int32_t wo[3], const uint32_t wd[3], const KilnVoxel* old, const KilnVoxel* cur,
                    const uint8_t* addressed, bool transient) {
    std::lock_guard lock(g_mu);
    if (!transient && g_j.empty()) return;
    bind_locked();
    const uint32_t bx = wd[0] / kBlock, by = wd[1] / kBlock, bz = wd[2] / kBlock;
    bool changed = false;
    for (uint32_t gz = 0; gz < bz; ++gz)
        for (uint32_t gy = 0; gy < by; ++gy)
            for (uint32_t gx = 0; gx < bx; ++gx) {
                const Key k{wo[0] + static_cast<int32_t>(gx * kBlock), wo[1] + static_cast<int32_t>(gy * kBlock),
                            wo[2] + static_cast<int32_t>(gz * kBlock)};
                auto it = g_j.find(k);
                if (it == g_j.end() && !transient) continue;
                Entry* e = it == g_j.end() ? nullptr : &it->second;
                const bool had_pending = e && e->pending.any();
                for (uint32_t z = 0; z < kBlock; ++z)
                    for (uint32_t y = 0; y < kBlock; ++y)
                        for (uint32_t x = 0; x < kBlock; ++x) {
                            const size_t at = ((static_cast<size_t>(gz) * kBlock + z) * wd[1] + gy * kBlock + y) * wd[0] +
                                              gx * kBlock + x;
                            if (!addressed[at]) continue;
                            const uint32_t i = index_of(x, y, z);
                            if (!transient) {
                                if (e && e->mask.get(i)) {
                                    e->mask.clear(i), e->pending.clear(i), e->restored.clear(i);
                                    changed = true;
                                }
                                continue;
                            }
                            const bool known = e && e->mask.get(i);
                            if (!known && old[at] == cur[at]) continue;  // nothing changed, nothing to put back
                            if (!e) e = &g_j[k];
                            if (!known) e->before[i] = old[at];
                            e->after[i] = cur[at];
                            e->mask.set(i);
                            e->pending.clear(i), e->restored.clear(i);
                            if (e->after[i] == e->before[i]) e->mask.clear(i);  // written back to what it was
                            changed = true;
                        }
                if (e && had_pending && !e->pending.any()) --g_pending_blocks;
                if (e && !e->mask.any()) g_j.erase(k);
            }
    if (!changed) return;
    g_dirty = true;
    if (!regions_hooked()) save_locked();  // no save-region hook to flush before a save: now
}

void transient_pre(uint64_t step) {
    // 1. under the lock: the blocks to look at this step (copies)
    std::vector<std::pair<Key, Entry>> work;
    {
        std::lock_guard lock(g_mu);
        bind_locked();
        if (g_jworld.empty()) return;
        if (g_pending_blocks && world_running() && F.write) {
            if (!g_logged_start) {
                g_logged_start = true;
                KB_I("transient edits: putting back an earlier session's transient cells in %zu block(s)", g_pending_blocks);
            }
            auto it = g_j.upper_bound(g_cursor);
            for (size_t seen = 0; seen < g_j.size() && work.size() < kBlocksPerStep; ++seen, ++it) {
                if (it == g_j.end()) it = g_j.begin();
                g_cursor = it->first;
                if (it->second.pending.any()) work.emplace_back(it->first, it->second);
            }
        }
    }
    // 2. without it: read, put back, commit
    std::vector<std::pair<Bits, Bits>> done(work.size());
    std::vector<bool> ok(work.size(), false);
    const VoxelWorld* v = work.empty() ? nullptr : vox();
    for (size_t n = 0; v && n < work.size() && F.write; ++n)
        ok[n] = put_back(*v, work[n].first, work[n].second, &done[n].first, &done[n].second);
    // 3. under the lock again: the outcome (only the tick thread changes entries, so the copies are still current)
    std::lock_guard lock(g_mu);
    for (size_t n = 0; n < work.size(); ++n) {
        if (!ok[n]) continue;
        auto it = g_j.find(work[n].first);
        if (it == g_j.end()) continue;
        Entry& e = it->second;
        const bool had = e.pending.any();
        for (uint32_t i = 0; i < kCells; ++i) {
            if (!e.pending.get(i)) continue;
            e.pending.clear(i);
            if (done[n].first.get(i)) {
                e.restored.set(i);
                ++g_put_back;
            } else {
                // changed since, or never saved: nothing to put back, and nothing to keep on record
                e.mask.clear(i);
                if (done[n].second.get(i)) ++g_left;
            }
        }
        if (had) --g_pending_blocks;
        if (!e.mask.any()) g_j.erase(it);
        g_dirty = true;
    }
    if (!work.empty() && !g_pending_blocks) {
        uint64_t written = 0;
        g_restored_at = regions_saves_written(&written) ? written : 0;
        KB_I("transient edits: put back %llu cell(s) that an earlier session's transient edits left in the save; %llu "
             "cell(s) were changed since and kept as they are",
             static_cast<unsigned long long>(g_put_back), static_cast<unsigned long long>(g_left));
        save_locked();
    }
    // put-back cells leave the journal once two world files were written after the put-back
    if (g_restored_at != UINT64_MAX && !g_pending_blocks) {
        uint64_t written = 0;
        if (regions_saves_written(&written) && written >= g_restored_at + 2) {
            for (auto it = g_j.begin(); it != g_j.end();) {
                it->second.mask.minus(it->second.restored);
                it->second.restored = Bits{};
                it = it->second.mask.any() ? std::next(it) : g_j.erase(it);
            }
            g_restored_at = UINT64_MAX;
            g_dirty = true;
            KB_I("transient edits: the put-back cells are in two saves now; %zu block(s) stay journaled", g_j.size());
        }
    }
    if (step % 24 == 0) save_locked();
}

void transient_before_save() {
    std::lock_guard lock(g_mu);
    bind_locked();
    save_locked();
}

}  // namespace kb
