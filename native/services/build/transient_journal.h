// SPDX-License-Identifier: MIT
// The transient journal's bookkeeping, without the engine or Windows (transient.cpp is the adapter; the host test
// tests/transient_journal_test.cpp drives this with a fault-injecting store). What it guarantees:
//  - A transient write's cells are on disk before the write commits (`note_transient`): the change is appended to the
//    journal's log; if that cannot be done the write is refused and nothing changes. So every transient cell the store
//    holds, and any save can write, has a record on disk, whatever happens to a later flush.
//  - The log is a list of whole-block states (replaying it is idempotent); `compact` writes a snapshot of everything
//    with a new generation and starts a new log of that generation. Loading replays only the log of the snapshot's
//    generation. A failed compaction leaves the earlier snapshot and log valid; a failed append (a torn record) stops
//    all further appends until a compaction succeeds, so nothing is appended after a record the loader would stop at.
//  - A kept write that sets journaled cells takes them out on disk BEFORE it commits (`note_kept_before`); if that cannot
//    be done the kept write is refused. Otherwise a kept write of exactly the transient value, followed by a crash before
//    the next compaction, would leave a record that the next start's put-back reads as "still transient" and undoes. A
//    kept write that touches no journaled cell costs one map lookup per block. If its commit then faults, the blocks it
//    did not commit get their records back (`undo_kept`).
//  - Other changes (the put-back's settling, retiring put-back cells) are recorded the same way but best effort: if that
//    fails the journal on disk says more than memory (a superset of cells that hold their earlier value again), which the
//    put-back's per-cell rule makes harmless, and the next compaction writes the truth.
// Not thread-safe: the adapter holds one mutex around every call.
#pragma once
#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace kb::tj {

constexpr uint32_t kBlock = 8;
constexpr uint32_t kCells = kBlock * kBlock * kBlock;  // 512 cells per block
constexpr size_t kMaxBlocks = 8192;                    // ~17 MB of journal, 4 M cells
constexpr size_t kCompactLogBytes = 8u << 20;          // compact once the log is this long

using Cell = uint16_t;                // KilnVoxel
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
    Cell before[kCells] = {};
    Cell after[kCells] = {};
};

// The files, by name (the adapter maps a name to kiln/data/kiln.build/transient/<name>). Each call is one operation, so
// a test can make any of them fail.
struct Store {
    virtual ~Store() = default;
    virtual bool read(const std::string& name, std::string* out) = 0;      // false: no such file (or unreadable)
    virtual bool write_tmp(const std::string& name, const std::string& bytes) = 0;  // <name>.tmp, whole, flushed
    virtual bool replace(const std::string& name) = 0;                     // <name>.tmp over <name>, atomically
    virtual bool append(const std::string& name, const std::string& bytes) = 0;     // to an existing file, flushed
    virtual bool rename_aside(const std::string& name, const std::string& to) = 0;
};

using Logger = std::function<void(int level, const std::string& text)>;  // level: 0 info, 1 warn, 2 error

inline uint32_t index_of(uint32_t x, uint32_t y, uint32_t z) { return (z * kBlock + y) * kBlock + x; }

// The put-back rule for one block read back as `cur` (the block's cells, x fastest): a pending cell that still holds
// "after" (and "after" is not "before") gets "before" (`cur` is changed, the cell goes into `back`); one that holds
// something else than "before" was changed since (`gone`).
void decide(const Entry& e, Cell* cur, Bits* back, Bits* gone);

class Journal {
public:
    Journal(Store& store, Logger log) : store_(store), log_(std::move(log)) {}

    // Binds to world `world` (GUID text): the earlier world's journal is compacted, this one's loaded; every loaded cell
    // is pending (checked once by the put-back). Then a compaction starts this session's log. An empty `world` (the
    // game closed the world and the next one's identity is not known yet) compacts the earlier world's journal and
    // leaves the journal unbound: transient writes are refused and nothing is recorded under the earlier world.
    void bind(const std::string& world);
    const std::string& world() const { return world_; }
    bool durable() const { return durable_; }

    // A transient write, BEFORE it commits: the block-aligned box wo/wd, the cells before (`old`) and after (`cur`), the
    // cells it sets (`addressed`). True when the change is on disk (or there was nothing to record); false when it could
    // not be (no world, the journal is full, the disk refused): the write must then not commit. `why` says why.
    bool note_transient(const int32_t wo[3], const uint32_t wd[3], const Cell* old, const Cell* cur, const uint8_t* addressed,
                        std::string* why);
    // A kept write, BEFORE it commits: the journaled cells it sets leave the journal, on disk first. True when that is
    // done (or no journaled cell is touched: nothing is written); false when it could not be on disk: the kept write must
    // then not commit (`why`). `undo` gets the blocks' earlier states, for undo_kept.
    bool note_kept_before(const int32_t wo[3], const uint32_t wd[3], const uint8_t* addressed,
                          std::vector<std::pair<Key, Entry>>* undo, std::string* why);
    // The kept write's commit faulted: the blocks of `undo` in `keys` (dirty, not committed) get their states back.
    void undo_kept(const std::vector<std::pair<Key, Entry>>& undo, const std::vector<Key>& keys);

    // Up to `max` blocks with pending cells, round robin, as copies (the put-back reads and commits without the lock).
    std::vector<std::pair<Key, Entry>> pending_work(size_t max);
    // The put-back of one block was committed (or nothing needed committing): `back` cells now hold "before" again,
    // `gone` cells were changed since. All of the block's pending cells are settled. A block whose commit FAILED must not
    // come here: it stays pending (on disk and in memory) for a later start.
    void settle(const Key& k, const Bits& back, const Bits& gone);
    size_t pending_blocks() const { return pending_blocks_; }
    // Step 3 of a put-back round: the outcome of each block of `work` (from pending_work). Done blocks are settled with
    // their `done` (back, gone) bits; NotReady and Failed blocks stay pending. Returns the cells put back and left.
    enum class Outcome { NotReady, Done, Failed };
    std::pair<uint64_t, uint64_t> finish(const std::vector<std::pair<Key, Entry>>& work, const std::vector<Outcome>& how,
                                         const std::vector<std::pair<Bits, Bits>>& done);
    // Put-back cells leave the journal (two saves were written after them).
    void retire_restored();

    // Snapshot + new log. False when it could not be written (the earlier files stay valid).
    bool compact();
    // Compacts when something is not on disk yet, when appends are off, or when the log is long.
    bool maintain();

    const std::map<Key, Entry>& entries() const { return j_; }
    uint64_t generation() const { return gen_; }

private:
    bool append_states(const std::vector<Key>& keys, const std::map<Key, Entry>& from);
    void load();
    void count_pending();

    Store& store_;
    Logger log_;
    std::map<Key, Entry> j_;
    std::string world_;
    uint64_t gen_ = 0;
    size_t log_bytes_ = 0;
    bool durable_ = false;  // the log of generation gen_ exists and every append so far succeeded
    bool dirty_ = false;    // memory says less than the disk (a best-effort append failed): compact
    size_t pending_blocks_ = 0;
    Key cursor_{INT32_MIN, INT32_MIN, INT32_MIN};
};

}  // namespace kb::tj
