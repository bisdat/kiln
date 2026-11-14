// SPDX-License-Identifier: MIT
// Transient edits (KILN_BUILD_F_TRANSIENT) are gone after a restart.
//
// Skipping Kiln's own save region is not enough for that: the game's save writes every stored 32-cell chunk that
// overlaps a region (docs/re/build.md section 5), so a transient cell inside a player base, or in a chunk another (kept)
// edit made Kiln keep, is saved with the world. Undoing the cells before a save would show every player the terrain
// flickering, and the save itself has no hook that could leave them out. So the save may write them, and Kiln puts the
// earlier content back at the next start, before anyone needs it:
//  - Every transient write journals the cells it changed, per 8x8x8 block: the value before the first transient write
//    of this run of transient writes ("before") and after the newest ("after"). It is on disk BEFORE the write commits
//    (appended to kiln/data/kiln.build/transient/<world GUID>.log); when that cannot be done (a full disk, no world
//    identity, the journal full) the write is refused and nothing is committed. So no transient cell is ever in the
//    store, or in any save, without a record on disk. The bookkeeping is transient_journal.cpp (host-tested with a
//    store that fails writes, renames and appends: tests/transient_journal_test.cpp).
//  - A kept write (any write without the flag) takes the journaled cells it sets out of the journal, on disk BEFORE it
//    commits: they are meant to stay, and a record left behind would make the next start put back a kept edit of the
//    very value the transient write left. If that cannot be written the kept write is refused; a kept write that sets
//    no journaled cell costs a map lookup per block. A faulted commit gives its uncommitted blocks their records back.
//  - The log is compacted into <world GUID>.bin (a snapshot with a generation) at the save-region hook, every 24 steps
//    and when it grows; a failed compaction leaves the earlier files valid.
//  - At the next start (the world runs, its save is loaded into the store), every journaled cell that still holds its
//    "after" value gets its "before" value back, block by block as the tiles load, through the same store commit as any
//    edit (so a joined client sees it too). A cell that holds anything else was changed since (a player dug it, or the
//    save never kept it) and is left as it is. A block whose commit fails stays pending, in memory and on disk, for a
//    later start (a later read decides cell by cell).
//  - Put-back cells stay on record until kiln.saves has seen two world files written after the put-back, so a crash
//    before the next save does not lose them; then they leave the journal.
// Cells a transient write sets again in the new session become that session's (an earlier copy is not put back over
// them). Tick thread, except transient_before_save (the game's save thread); one mutex. The engine's reads and commits
// run outside it.
#include <windows.h>

#include <filesystem>
#include <fstream>

#include "internal.h"
#include "services/kiln_saves.h"
#include "transient_journal.h"

namespace kb {
namespace {

constexpr uint32_t kBlocksPerStep = 32;  // put back at most this many blocks a step

// kiln/data/kiln.build/transient/<name>, through Win32 so each step reports its own failure.
struct WinStore final : tj::Store {
    std::filesystem::path dir;

    std::filesystem::path at(const std::string& name) const { return dir / std::filesystem::path(name); }
    bool ready() {
        if (!dir.empty()) return true;
        char buf[1024];
        size_t len = 0;
        if (g_api->path(KILN_PATH_DATA, buf, sizeof buf, &len) != KILN_OK) return false;
        dir = std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(buf), len)) / "kiln.build" / "transient";
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        return true;
    }
    static bool write_all(HANDLE h, const std::string& bytes) {
        size_t done = 0;
        while (done < bytes.size()) {
            DWORD n = 0;
            const DWORD want = static_cast<DWORD>(std::min<size_t>(bytes.size() - done, 1u << 30));
            if (!WriteFile(h, bytes.data() + done, want, &n, nullptr) || n == 0) return false;
            done += n;
        }
        return FlushFileBuffers(h) != 0;
    }
    bool read(const std::string& name, std::string* out) override {
        if (!ready()) return false;
        std::error_code ec;
        if (!std::filesystem::exists(at(name), ec)) return false;
        std::ifstream f(at(name), std::ios::binary);
        if (!f) return false;
        out->assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
        return true;
    }
    bool write_tmp(const std::string& name, const std::string& bytes) override {
        if (!ready()) return false;
        std::filesystem::path tmp = at(name);
        tmp += L".tmp";
        HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) return false;
        const bool ok = write_all(h, bytes);
        CloseHandle(h);
        return ok;
    }
    bool replace(const std::string& name) override {
        if (!ready()) return false;
        std::filesystem::path tmp = at(name);
        tmp += L".tmp";
        return MoveFileExW(tmp.c_str(), at(name).c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
    }
    bool append(const std::string& name, const std::string& bytes) override {
        if (!ready()) return false;
        HANDLE h = CreateFileW(at(name).c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                               FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) return false;
        const bool ok = write_all(h, bytes);
        CloseHandle(h);
        return ok;
    }
    bool rename_aside(const std::string& name, const std::string& to) override {
        if (!ready()) return false;
        return MoveFileExW(at(name).c_str(), at(to).c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
    }
};

std::mutex g_mu;
WinStore g_store;
tj::Journal g_j(g_store, [](int level, const std::string& text) {
    logf(level == 0 ? KILN_LOG_INFO : level == 1 ? KILN_LOG_WARN : KILN_LOG_ERROR, "%s", text.c_str());
});
uint64_t g_restored_at = UINT64_MAX;  // kiln.saves' written count when the put-back finished
uint64_t g_put_back = 0, g_left = 0;  // cells put back / left (changed since) in this session's put-back
bool g_logged_start = false, g_logged_fault = false;

// Binds the journal to the running world (kiln.saves' GUID); a new world resets this session's put-back counters.
void bind_locked() {
    const std::string before = g_j.world();
    g_j.bind(regions_world());
    if (g_j.world() == before) return;
    g_restored_at = UINT64_MAX;
    g_put_back = g_left = 0;
    g_logged_start = g_logged_fault = false;
}

using PutBack = tj::Journal::Outcome;

// One block of an earlier session, on a copy of its entry and WITHOUT the journal's lock (the game's save thread takes
// it in the save-region hook, and the engine's reads and store commit must never wait behind that): cells that still
// hold what the transient write left get their earlier value back. `back` gets the cells put back, `gone` the cells
// changed since. Failed: the commit faulted; nothing about the block may be settled (it stays pending).
PutBack put_back(const VoxelWorld& v, const tj::Key& k, const tj::Entry& e, tj::Bits* back, tj::Bits* gone) {
    const uint32_t dims[3] = {kBlock, kBlock, kBlock};
    if (!vox_ready_box(v, k.data(), dims, true)) return PutBack::NotReady;
    KilnVoxel cur[tj::kCells];
    if (vox_read(k.data(), dims, cur, tj::kCells) != KILN_OK) return PutBack::NotReady;
    tj::decide(e, cur, back, gone);
    if (back->any()) {
        if (!vox_commit_block(v, k.data(), cur)) return PutBack::Failed;
        S.blocks_written.fetch_add(1, std::memory_order_relaxed);
    }
    return PutBack::Done;
}

}  // namespace

KilnResult transient_note_before(const int32_t wo[3], const uint32_t wd[3], const KilnVoxel* old, const KilnVoxel* cur,
                                 const uint8_t* addressed) {
    std::lock_guard lock(g_mu);
    bind_locked();
    std::string why;
    if (g_j.note_transient(wo, wd, old, cur, addressed, &why)) return KILN_OK;
    if (g_j.world().empty())
        why = g_api->service(kstr(KILN_SAVES_SERVICE), 1)
                  ? "transient edits need the world's identity, which kiln.saves does not know yet (a new world whose id "
                    "only its first save gives): refused so that the first save cannot keep them"
                  : "transient edits need kiln.saves (the world's identity for their journal)";
    set_detail("transient edit refused: %s", why.c_str());
    return KILN_ERR_UNAVAILABLE;
}

namespace {
std::vector<std::pair<tj::Key, tj::Entry>> g_kept_undo;  // the kept write in flight (tick thread, one at a time)
}

KilnResult transient_kept_before(const int32_t wo[3], const uint32_t wd[3], const uint8_t* addressed) {
    std::lock_guard lock(g_mu);
    g_kept_undo.clear();
    if (g_j.entries().empty() && !g_j.world().empty()) return KILN_OK;  // the ordinary kept write: nothing journaled
    bind_locked();
    std::string why;
    if (g_j.note_kept_before(wo, wd, addressed, &g_kept_undo, &why)) return KILN_OK;
    // refused rather than committed: a kept edit the next start's put-back could undo must not exist
    set_detail("edit refused: %s", why.c_str());
    KB_E("kiln.build: an edit of journaled transient cells was refused: %s", why.c_str());
    return KILN_ERR_UNAVAILABLE;
}

void transient_kept_failed(const std::vector<std::array<int32_t, 3>>& uncommitted) {
    std::lock_guard lock(g_mu);
    if (g_kept_undo.empty()) return;
    std::vector<tj::Key> keys(uncommitted.begin(), uncommitted.end());
    g_j.undo_kept(g_kept_undo, keys);
    g_kept_undo.clear();
}

void transient_pre(uint64_t step) {
    // 1. under the lock: the blocks to look at this step (copies)
    std::vector<std::pair<tj::Key, tj::Entry>> work;
    {
        std::lock_guard lock(g_mu);
        bind_locked();
        if (g_j.world().empty()) return;
        if (g_j.pending_blocks() && world_running() && F.write) {
            if (!g_logged_start) {
                g_logged_start = true;
                KB_I("transient edits: putting back an earlier session's transient cells in %zu block(s)", g_j.pending_blocks());
            }
            work = g_j.pending_work(kBlocksPerStep);
        }
    }
    // 2. without it: read, put back, commit
    std::vector<std::pair<tj::Bits, tj::Bits>> done(work.size());
    std::vector<PutBack> how(work.size(), PutBack::NotReady);
    const VoxelWorld* v = work.empty() ? nullptr : vox();
    for (size_t n = 0; v && n < work.size() && F.write; ++n)
        how[n] = put_back(*v, work[n].first, work[n].second, &done[n].first, &done[n].second);
    // 3. under the lock again: settle what was done (only the tick thread changes entries, so the copies are current)
    std::lock_guard lock(g_mu);
    bool settled = false;
    for (size_t n = 0; n < work.size(); ++n) {
        settled |= how[n] == PutBack::Done;
        if (how[n] == PutBack::Failed && !g_logged_fault) {
            // the commit faulted (voxel writes are off now): the block stays pending, in memory and on disk; a later
            // start reads it again and decides cell by cell (a cell the commit did write holds "before" by then)
            g_logged_fault = true;
            KB_E("transient edits: putting block %d,%d,%d back faulted; it stays journaled for the next start",
                 work[n].first[0], work[n].first[1], work[n].first[2]);
        }
    }
    const auto [b, l] = g_j.finish(work, how, done);
    g_put_back += b, g_left += l;
    if (settled && !g_j.pending_blocks()) {
        uint64_t written = 0;
        g_restored_at = regions_saves_written(&written) ? written : 0;
        KB_I("transient edits: put back %llu cell(s) that an earlier session's transient edits left in the save; %llu "
             "cell(s) were changed since and kept as they are",
             static_cast<unsigned long long>(g_put_back), static_cast<unsigned long long>(g_left));
        g_j.maintain();
    }
    // put-back cells leave the journal once two world files were written after the put-back
    if (g_restored_at != UINT64_MAX && !g_j.pending_blocks()) {
        uint64_t written = 0;
        if (regions_saves_written(&written) && written >= g_restored_at + 2) {
            g_j.retire_restored();
            g_restored_at = UINT64_MAX;
            KB_I("transient edits: the put-back cells are in two saves now; %zu block(s) stay journaled", g_j.entries().size());
        }
    }
    if (step % 24 == 0) g_j.maintain();
}

void transient_before_save() {
    // Every transient cell in the store is on disk already (appended before its commit); this only compacts the log.
    std::lock_guard lock(g_mu);
    bind_locked();
    g_j.maintain();
}

}  // namespace kb
