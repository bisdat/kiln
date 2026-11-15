// SPDX-License-Identifier: MIT
// Host test of the transient journal's bookkeeping (transient_journal.cpp) with a store that can fail a write, a rename
// (replace), an append, or tear an append half way, and with put-back commits that fail. Run: test-transient.sh.
#include "../transient_journal.h"

#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

using namespace kb::tj;

namespace {

int g_fail = 0, g_pass = 0;
void check(bool ok, const char* name) {
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", name);
    (ok ? g_pass : g_fail)++;
}

struct MemStore final : Store {
    std::map<std::string, std::string> files;
    bool fail_write = false, fail_replace = false, fail_append = false, tear_append = false;
    bool read(const std::string& n, std::string* out) override {
        auto it = files.find(n);
        if (it == files.end()) return false;
        *out = it->second;
        return true;
    }
    bool write_tmp(const std::string& n, const std::string& b) override {
        if (fail_write) return false;
        files[n + ".tmp"] = b;
        return true;
    }
    bool replace(const std::string& n) override {
        if (fail_replace) return false;
        auto it = files.find(n + ".tmp");
        if (it == files.end()) return false;
        files[n] = it->second;
        files.erase(it);
        return true;
    }
    bool append(const std::string& n, const std::string& b) override {
        auto it = files.find(n);
        if (it == files.end() || fail_append) return false;
        if (tear_append) {
            it->second += b.substr(0, b.size() / 2);  // half of it reached the disk
            return false;
        }
        it->second += b;
        return true;
    }
    bool rename_aside(const std::string& n, const std::string& to) override {
        auto it = files.find(n);
        if (it == files.end()) return false;
        files[to] = it->second;
        files.erase(it);
        return true;
    }
};

std::vector<std::string> g_log;
Logger logger() {
    return [](int level, const std::string& t) { g_log.push_back(std::to_string(level) + " " + t); };
}

const std::string W = "6f1c2a3b-0000-4000-8000-00000000000a";

// One block-aligned 8x8x8 write at `org`: cells `n` set from 1 to `to` (old 1 = stone, everything addressed).
struct Write {
    int32_t wo[3];
    uint32_t wd[3] = {8, 8, 8};
    std::vector<Cell> old = std::vector<Cell>(kCells, 1), cur = std::vector<Cell>(kCells, 1);
    std::vector<uint8_t> addressed = std::vector<uint8_t>(kCells, 0);
    Write(int32_t x, int32_t y, int32_t z, uint32_t n, Cell to) : wo{x, y, z} {
        for (uint32_t i = 0; i < n; ++i) cur[i] = to, addressed[i] = 1;
    }
};

bool transient(Journal& j, const Write& w, std::string* why = nullptr) {
    std::string tmp;
    return j.note_transient(w.wo, w.wd, w.old.data(), w.cur.data(), w.addressed.data(), why ? why : &tmp);
}

size_t journaled(MemStore& s, Key k, uint32_t* cells = nullptr, bool* pending = nullptr) {
    Journal r(s, logger());
    r.bind(W);
    auto it = r.entries().find(k);
    if (it == r.entries().end()) return 0;
    uint32_t n = 0;
    for (uint32_t i = 0; i < kCells; ++i) n += it->second.mask.get(i);
    if (cells) *cells = n;
    if (pending) *pending = it->second.pending.any();
    return 1;
}

void test_on_disk_before_commit() {
    MemStore s;
    Journal j(s, logger());
    j.bind(W);
    check(j.durable(), "a bound journal has its log");
    check(transient(j, Write(0, 0, 0, 10, 20)), "a transient write is recorded");
    uint32_t cells = 0;
    bool pending = false;
    check(journaled(s, {0, 0, 0}, &cells, &pending) && cells == 10 && pending,
          "the record is on disk before any compaction (replayed from the log), pending at the next start");
}

void test_failing_append_refuses() {
    MemStore s;
    Journal j(s, logger());
    j.bind(W);
    s.fail_append = true;
    std::string why;
    check(!transient(j, Write(0, 0, 0, 10, 20), &why) && why.find("could not be written") != std::string::npos,
          "a failing append refuses the transient write (it must not commit)");
    check(j.entries().empty(), "a refused write leaves nothing in memory");
    check(!j.durable(), "after a failed append nothing more is appended until a compaction");
    s.fail_append = false;
    check(transient(j, Write(0, 0, 0, 10, 20)), "once the disk takes writes again the next write compacts and goes through");
    check(journaled(s, {0, 0, 0}), "and its record is on disk");
}

void test_failing_write_refuses() {
    MemStore s;
    s.fail_write = true;
    Journal j(s, logger());
    j.bind(W);
    std::string why;
    check(!j.durable() && !transient(j, Write(0, 0, 0, 4, 20), &why) && why.find("cannot be written") != std::string::npos,
          "a failing file write (no snapshot, no log): transient writes are refused");
    s.fail_write = false;
    check(transient(j, Write(0, 0, 0, 4, 20)) && journaled(s, {0, 0, 0}), "recovers when the file can be written");
}

void test_failing_rename_refuses() {
    MemStore s;
    s.fail_replace = true;
    Journal j(s, logger());
    j.bind(W);
    check(!j.durable() && !transient(j, Write(0, 0, 0, 4, 20)), "a failing rename: transient writes are refused");
    s.fail_replace = false;
    check(transient(j, Write(0, 0, 0, 4, 20)) && journaled(s, {0, 0, 0}), "recovers when the rename works");
}

void test_live_cells_survive_a_failed_save_flush() {
    // cells already live when the save hook's flush (a compaction) fails: they are in the log already
    MemStore s;
    Journal j(s, logger());
    j.bind(W);
    check(transient(j, Write(0, 0, 0, 10, 20)) && transient(j, Write(8, 0, 0, 3, 21)), "two transient writes");
    s.fail_write = true;
    check(!j.compact(), "the save hook's compaction fails (disk full)");
    s.fail_write = false;
    s.fail_replace = true;
    check(!j.compact(), "and again, now its rename");
    s.fail_replace = false;
    uint32_t a = 0, b = 0;
    check(journaled(s, {0, 0, 0}, &a) && journaled(s, {8, 0, 0}, &b) && a == 10 && b == 3,
          "both blocks are still on disk (the earlier snapshot and its log stay valid)");
    check(transient(j, Write(16, 0, 0, 2, 22)) && journaled(s, {16, 0, 0}), "and appends go on after a failed snapshot");
}

void test_failing_log_restart_stops_appends() {
    MemStore s;
    Journal j(s, logger());
    j.bind(W);
    check(transient(j, Write(0, 0, 0, 5, 20)), "a transient write");
    s.fail_replace = false;
    // the snapshot is written, the new log is not: the old log is another generation now
    struct LogOnly final : Store {
        MemStore& m;
        explicit LogOnly(MemStore& s) : m(s) {}
        bool read(const std::string& n, std::string* o) override { return m.read(n, o); }
        bool write_tmp(const std::string& n, const std::string& b) override { return n.ends_with(".log") ? false : m.write_tmp(n, b); }
        bool replace(const std::string& n) override { return m.replace(n); }
        bool append(const std::string& n, const std::string& b) override { return m.append(n, b); }
        bool rename_aside(const std::string& n, const std::string& t) override { return m.rename_aside(n, t); }
    } lo(s);
    Journal k(lo, logger());
    k.bind(W);
    check(!k.durable() && !transient(k, Write(8, 0, 0, 2, 20)), "a snapshot without a new log: no appends, writes refused");
    check(journaled(s, {0, 0, 0}), "the snapshot holds the earlier block");
}

void test_torn_append() {
    MemStore s;
    Journal j(s, logger());
    j.bind(W);
    check(transient(j, Write(0, 0, 0, 5, 20)), "a transient write");
    s.tear_append = true;
    check(!transient(j, Write(8, 0, 0, 5, 20)), "a torn append refuses its write");
    s.tear_append = false;
    check(journaled(s, {0, 0, 0}) && !journaled(s, {8, 0, 0}), "the loader stops at the torn record and keeps what came before");
    check(transient(j, Write(16, 0, 0, 5, 20)) && journaled(s, {0, 0, 0}) && journaled(s, {16, 0, 0}) && !journaled(s, {8, 0, 0}),
          "the next write compacts first, so nothing is appended behind the torn record");
}

void test_failed_put_back_commit_stays_pending() {
    MemStore s;
    {
        Journal j(s, logger());
        j.bind(W);
        check(transient(j, Write(0, 0, 0, 10, 20)) && transient(j, Write(8, 0, 0, 10, 20)), "two transient blocks");
    }
    Journal j(s, logger());  // the next start
    j.bind(W);
    check(j.pending_blocks() == 2, "both blocks are pending at the next start");
    auto work = j.pending_work(32);
    std::vector<std::pair<Bits, Bits>> done(work.size());
    std::vector<Journal::Outcome> how(work.size());
    for (size_t n = 0; n < work.size(); ++n) {
        std::vector<Cell> cur(kCells, 1);
        for (uint32_t i = 0; i < 10; ++i) cur[i] = 20;  // the save kept the transient cells
        decide(work[n].second, cur.data(), &done[n].first, &done[n].second);
        how[n] = work[n].first == Key{0, 0, 0} ? Journal::Outcome::Failed : Journal::Outcome::Done;  // its commit faults
    }
    const auto [back, left] = j.finish(work, how, done);
    check(back == 10 && left == 0, "the block whose commit worked counts 10 cells put back");
    check(j.pending_blocks() == 1 && j.entries().count({0, 0, 0}) && j.entries().at({0, 0, 0}).pending.any(),
          "the block whose commit failed stays pending in memory");
    j.maintain();
    bool pending = false;
    uint32_t cells = 0;
    check(journaled(s, {0, 0, 0}, &cells, &pending) && cells == 10 && pending,
          "and on disk, for the next start (it was not taken for restored)");

    // the next start reads it: a cell the faulted commit did write holds "before", the rest still "after"
    Journal again(s, logger());
    again.bind(W);
    auto w2 = again.pending_work(32);
    std::vector<std::pair<Bits, Bits>> d2(w2.size());
    std::vector<Journal::Outcome> h2(w2.size(), Journal::Outcome::Done);
    for (size_t n = 0; n < w2.size(); ++n) {
        std::vector<Cell> cur(kCells, 1);
        if (w2[n].first == Key{0, 0, 0})
            for (uint32_t i = 4; i < 10; ++i) cur[i] = 20;  // partly committed: cells 0..3 are back already
        decide(w2[n].second, cur.data(), &d2[n].first, &d2[n].second);
    }
    const auto [b2, l2] = again.finish(w2, h2, d2);
    check(b2 == 6 && l2 == 0 && again.pending_blocks() == 0, "a later read settles a partly committed block cell by cell");
}

void test_decide_rule() {
    Entry e;
    for (uint32_t i = 0; i < 3; ++i) e.mask.set(i), e.pending.set(i), e.before[i] = 1, e.after[i] = 20;
    std::vector<Cell> cur(kCells, 0);
    cur[0] = 20, cur[1] = 1, cur[2] = 7;  // still "after", already "before", changed by a player
    Bits back, gone;
    decide(e, cur.data(), &back, &gone);
    check(back.get(0) && !back.get(1) && !back.get(2) && gone.get(2) && !gone.get(1) && cur[0] == 1 && cur[2] == 7,
          "put back only what still holds the transient value; a player's change is kept");
}

bool kept(Journal& j, const Write& w, std::vector<std::pair<Key, Entry>>* undo = nullptr) {
    std::vector<std::pair<Key, Entry>> tmp;
    std::string why;
    return j.note_kept_before(w.wo, w.wd, w.addressed.data(), undo ? undo : &tmp, &why);
}

// The next start after a crash (no compaction): which cells of block 0 the put-back changes, the store holding `cur`.
Bits put_back_after_crash(MemStore& s, const std::vector<Cell>& cur) {
    Journal r(s, logger());
    r.bind(W);
    Bits back, gone;
    for (auto& [k, e] : r.pending_work(32)) {
        if (k != Key{0, 0, 0}) continue;
        std::vector<Cell> c = cur;
        decide(e, c.data(), &back, &gone);
    }
    return back;
}

void test_kept_write_of_the_transient_value_survives_a_crash() {
    MemStore s;
    Journal j(s, logger());
    j.bind(W);
    check(transient(j, Write(0, 0, 0, 10, 20)), "a transient write: cells 0..9 become 20");
    std::vector<Cell> store(kCells, 1);
    for (uint32_t i = 0; i < 10; ++i) store[i] = 20;

    // a kept write of exactly the transient value on cells 0..3 while the disk refuses the journal's append
    s.fail_append = true;
    const bool accepted = kept(j, Write(0, 0, 0, 4, 20));
    s.fail_append = false;
    check(!accepted, "a kept write over journaled cells is refused when their removal cannot be on disk first");
    Bits back = put_back_after_crash(s, store);  // the process dies before any compaction
    bool kept_cells_reverted = false;
    for (uint32_t i = 0; i < 4; ++i) kept_cells_reverted |= accepted && back.get(i);
    check(!kept_cells_reverted, "so a kept edit of the transient value is never put back by the next start");

    // the disk works again: the kept write goes through, the process dies before a compaction
    Journal j2(s, logger());
    j2.bind(W);
    check(kept(j2, Write(0, 0, 0, 4, 20)), "with a working disk the kept write's removal is appended before its commit");
    back = put_back_after_crash(s, store);
    check(!back.get(0) && !back.get(3) && back.get(4) && back.get(9),
          "after a crash the put-back keeps the kept cells (0..3) and puts back the transient rest (4..9)");
}

void test_ordinary_kept_writes_pay_nothing() {
    MemStore s;
    Journal j(s, logger());
    j.bind(W);
    check(transient(j, Write(0, 0, 0, 4, 20)), "a transient block");
    const size_t log_before = s.files[W + ".log"].size();
    s.fail_append = true;
    check(kept(j, Write(64, 0, 0, 8, 30)), "a kept write elsewhere goes through even while the disk refuses (nothing journaled there)");
    Write miss(0, 0, 0, 0, 30);
    miss.addressed[100] = 1;  // the transient block, but a cell it never wrote
    check(kept(j, miss), "and so does one in a journaled block that sets no journaled cell");
    s.fail_append = false;
    check(s.files[W + ".log"].size() == log_before, "neither wrote anything");
}

void test_faulted_kept_commit_restores_records() {
    MemStore s;
    Journal j(s, logger());
    j.bind(W);
    check(transient(j, Write(0, 0, 0, 6, 20)) && transient(j, Write(8, 0, 0, 6, 20)), "two transient blocks");
    Write both(0, 0, 0, 0, 30);
    both.wd[0] = 16;
    both.old.assign(16 * 64, 1), both.cur.assign(16 * 64, 1), both.addressed.assign(16 * 64, 0);
    for (uint32_t i = 0; i < 16 * 64; ++i) both.addressed[i] = 1;
    std::vector<std::pair<Key, Entry>> undo;
    check(kept(j, both, &undo) && undo.size() == 2 && j.entries().empty(), "a kept write over both takes them out (on disk first)");
    j.undo_kept(undo, {Key{8, 0, 0}});  // its commit faulted after block 0: block 8 still holds its transient cells
    uint32_t cells = 0;
    check(!journaled(s, {0, 0, 0}) && journaled(s, {8, 0, 0}, &cells) && cells == 6,
          "the uncommitted block's records come back on disk; the committed block's stay out");
}

void test_bad_snapshot_aside() {
    MemStore s;
    {
        Journal j(s, logger());
        j.bind(W);
        check(transient(j, Write(0, 0, 0, 3, 20)), "a transient write");
    }
    s.files[W + ".bin"] = "garbage";
    Journal j(s, logger());
    j.bind(W);
    check(s.files.count(W + ".bin.bad") == 1, "an unreadable snapshot is kept aside");
    check(j.entries().count({0, 0, 0}) == 1, "and its log is still replayed");
}

// A game client hosts one world after another. When the game closes world A, kiln.saves has no identity until the next
// world's is known, and a brand-new world's is known only after its first save: the journal must not go on recording
// under A meanwhile (the new world's cells would go into A's journal, and A's next start would look for them in A).
void test_a_world_without_identity_unbinds() {
    MemStore s;
    Journal j(s, logger());
    j.bind(W);
    check(transient(j, Write(0, 0, 0, 10, 20)), "world A: a transient write is recorded");
    const size_t log_before = s.files[W + ".log"].size();
    j.bind("");  // the game closed world A; the next world has no identity yet
    check(j.world().empty() && j.entries().empty(), "a world without an identity unbinds the journal");
    std::string why;
    check(!transient(j, Write(64, 0, 0, 5, 30), &why) && why.find("identity") != std::string::npos,
          "a transient write in a world without an identity is refused");
    check(kept(j, Write(0, 0, 0, 10, 20)), "a kept write there goes through");
    check(s.files[W + ".log"].size() == log_before, "neither wrote to world A's log");
    uint32_t cells = 0;
    bool pending = false;
    check(!journaled(s, {64, 0, 0}) && journaled(s, {0, 0, 0}, &cells, &pending) && cells == 10 && pending,
          "world A's journal holds its own cells only, pending when A is loaded again");
    j.bind(W);
    check(j.pending_blocks() == 1, "rehosting the same world reloads the previous session's cells as pending");
}

}  // namespace

int main() {
    test_on_disk_before_commit();
    test_failing_append_refuses();
    test_failing_write_refuses();
    test_failing_rename_refuses();
    test_live_cells_survive_a_failed_save_flush();
    test_failing_log_restart_stops_appends();
    test_torn_append();
    test_failed_put_back_commit_stays_pending();
    test_decide_rule();
    test_kept_write_of_the_transient_value_survives_a_crash();
    test_ordinary_kept_writes_pay_nothing();
    test_faulted_kept_commit_restores_records();
    test_bad_snapshot_aside();
    test_a_world_without_identity_unbinds();
    std::printf("transient journal: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
