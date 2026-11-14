// SPDX-License-Identifier: MIT
// The transient journal's bookkeeping (transient_journal.h). Files, under kiln/data/kiln.build/transient/:
//   <world>.bin  snapshot: "KLNTRJ02", u32 format 2, u32 count, u64 generation, count x state, u64 FNV-1a of all before
//   <world>.log  log:      "KLNTRL02", u64 generation, u64 FNV-1a of those 16 bytes, then records
//                          {u32 'TTJR', state, u64 FNV-1a of the record before it}
//   state: i32 x, y, z (block origin); u64 mask[8]; u16 before[512]; u16 after[512] (only masked cells mean anything;
//          an empty mask means the block left the journal)
#include "transient_journal.h"

#include <bit>
#include <cstring>

namespace kb::tj {
namespace {

constexpr char kSnapMagic[8] = {'K', 'L', 'N', 'T', 'R', 'J', '0', '2'};
constexpr char kLogMagic[8] = {'K', 'L', 'N', 'T', 'R', 'L', '0', '2'};
constexpr uint32_t kRecMagic = 0x524A5454u;  // "TTJR"
constexpr size_t kState = 12 + 64 + 2 * 2 * kCells;
constexpr size_t kRecord = 4 + kState + 8;

uint64_t fnv64(const void* p, size_t n, uint64_t h = 1469598103934665603ull) {
    const auto* b = static_cast<const uint8_t*>(p);
    for (size_t i = 0; i < n; ++i) h = (h ^ b[i]) * 1099511628211ull;
    return h;
}

template <class T>
void put(std::string& s, const T& v) {
    s.append(reinterpret_cast<const char*>(&v), sizeof v);
}

void put_state(std::string& s, const Key& k, const Entry* e) {
    s.append(reinterpret_cast<const char*>(k.data()), 12);
    if (!e) {
        s.append(64 + 4 * kCells, '\0');
        return;
    }
    s.append(reinterpret_cast<const char*>(e->mask.w), 64);
    s.append(reinterpret_cast<const char*>(e->before), 2 * kCells);
    s.append(reinterpret_cast<const char*>(e->after), 2 * kCells);
}

bool aligned(const Key& k) {
    const auto b = static_cast<int32_t>(kBlock);
    return k[0] % b == 0 && k[1] % b == 0 && k[2] % b == 0;
}

// One state at p (kState bytes) into `j`: a block with an empty mask leaves.
bool take_state(const char* p, std::map<Key, Entry>& j) {
    Key k;
    std::memcpy(k.data(), p, 12);
    if (!aligned(k)) return false;
    Entry e;
    std::memcpy(e.mask.w, p + 12, 64);
    std::memcpy(e.before, p + 76, 2 * kCells);
    std::memcpy(e.after, p + 76 + 2 * kCells, 2 * kCells);
    if (e.mask.any()) j[k] = e;
    else j.erase(k);
    return true;
}

bool parse_snapshot(const std::string& s, std::map<Key, Entry>& j, uint64_t* gen) {
    uint32_t fmt = 0, count = 0;
    if (s.size() < 32 || std::memcmp(s.data(), kSnapMagic, 8) != 0) return false;
    std::memcpy(&fmt, s.data() + 8, 4);
    std::memcpy(&count, s.data() + 12, 4);
    std::memcpy(gen, s.data() + 16, 8);
    if (fmt != 2 || count > kMaxBlocks || s.size() != 24 + static_cast<size_t>(count) * kState + 8) return false;
    uint64_t sum = 0;
    std::memcpy(&sum, s.data() + s.size() - 8, 8);
    if (sum != fnv64(s.data(), s.size() - 8)) return false;
    for (uint32_t i = 0; i < count; ++i)
        if (!take_state(s.data() + 24 + static_cast<size_t>(i) * kState, j)) return false;
    return true;
}

std::string log_header(uint64_t gen) {
    std::string h(kLogMagic, 8);
    put(h, gen);
    put(h, fnv64(h.data(), h.size()));
    return h;
}

}  // namespace

void decide(const Entry& e, Cell* cur, Bits* back, Bits* gone) {
    for (uint32_t i = 0; i < kCells; ++i) {
        if (!e.pending.get(i)) continue;
        if (cur[i] == e.after[i] && cur[i] != e.before[i]) {
            cur[i] = e.before[i];
            back->set(i);
        } else if (cur[i] != e.before[i]) {
            gone->set(i);
        }
    }
}

std::pair<uint64_t, uint64_t> Journal::finish(const std::vector<std::pair<Key, Entry>>& work, const std::vector<Outcome>& how,
                                              const std::vector<std::pair<Bits, Bits>>& done) {
    uint64_t back = 0, left = 0;
    for (size_t n = 0; n < work.size() && n < how.size() && n < done.size(); ++n) {
        // NotReady: the tiles are not loaded yet. Failed: the commit faulted, so whether any cell was written is not
        // known; the block stays pending (memory and disk), and a later read decides cell by cell.
        if (how[n] != Outcome::Done) continue;
        for (uint32_t i = 0; i < kCells; ++i) {
            if (!work[n].second.pending.get(i)) continue;
            if (done[n].first.get(i)) ++back;
            else if (done[n].second.get(i)) ++left;
        }
        settle(work[n].first, done[n].first, done[n].second);
    }
    return {back, left};
}

void Journal::count_pending() {
    pending_blocks_ = 0;
    for (const auto& [k, e] : j_)
        if (e.pending.any()) ++pending_blocks_;
}

void Journal::load() {
    std::string s;
    bool snap = false;
    uint64_t g = 0;
    if (store_.read(world_ + ".bin", &s)) {
        if (parse_snapshot(s, j_, &g)) {
            snap = true;
        } else {
            j_.clear();
            g = 0;
            store_.rename_aside(world_ + ".bin", world_ + ".bin.bad");
            log_(2, "transient edits: the journal snapshot of world " + world_ + " is unreadable; kept as " + world_ +
                        ".bin.bad (its log, if any, is still replayed)");
        }
    }
    std::string l;
    if (store_.read(world_ + ".log", &l) && l.size() >= 24 && std::memcmp(l.data(), kLogMagic, 8) == 0) {
        uint64_t lg = 0, hs = 0;
        std::memcpy(&lg, l.data() + 8, 8);
        std::memcpy(&hs, l.data() + 16, 8);
        // the log of this snapshot's generation; with no usable snapshot every log is replayed (whole-block states:
        // replaying an older one yields a superset, which the put-back's per-cell rule makes harmless)
        if (hs == fnv64(l.data(), 16) && (!snap || lg == g)) {
            size_t n = 0;
            for (size_t at = 24; at + kRecord <= l.size(); at += kRecord, ++n) {
                uint32_t m = 0;
                uint64_t sum = 0;
                std::memcpy(&m, l.data() + at, 4);
                std::memcpy(&sum, l.data() + at + 4 + kState, 8);
                if (m != kRecMagic || sum != fnv64(l.data() + at, 4 + kState) || !take_state(l.data() + at + 4, j_)) break;  // torn tail
            }
            if (n) log_(0, "transient edits: replayed " + std::to_string(n) + " journal record(s) of world " + world_);
        }
    }
    gen_ = g;
    for (auto& [k, e] : j_) {
        e.pending = e.mask;  // every cell of an earlier session is checked once
        e.restored = Bits{};
    }
}

void Journal::bind(const std::string& world) {
    if (world.empty() || world == world_) return;
    if (!world_.empty()) maintain();
    j_.clear();
    world_ = world;
    gen_ = 0;
    log_bytes_ = 0;
    durable_ = dirty_ = false;
    cursor_ = {INT32_MIN, INT32_MIN, INT32_MIN};
    load();
    count_pending();
    size_t cells = 0;
    for (const auto& [k, e] : j_)
        for (uint64_t x : e.pending.w) cells += static_cast<size_t>(std::popcount(x));
    log_(0, "transient edits: world " + world + ", " + std::to_string(j_.size()) + " journaled block(s), " +
                std::to_string(cells) + " cell(s) of an earlier session to check");
    if (!compact())
        log_(2, "transient edits: the journal of world " + world + " cannot be written; transient edits are refused until it can");
}

bool Journal::compact() {
    if (world_.empty()) return false;
    const uint64_t g = gen_ + 1;
    std::string snap(kSnapMagic, 8);
    put(snap, uint32_t{2});
    put(snap, static_cast<uint32_t>(j_.size()));
    put(snap, g);
    for (const auto& [k, e] : j_) put_state(snap, k, &e);
    put(snap, fnv64(snap.data(), snap.size()));
    if (!store_.write_tmp(world_ + ".bin", snap) || !store_.replace(world_ + ".bin")) {
        // the earlier snapshot and its log stay valid; appends to that log go on if they did before
        log_(2, "transient edits: the journal snapshot of world " + world_ + " could not be written");
        return false;
    }
    gen_ = g;
    dirty_ = false;
    const std::string h = log_header(g);
    if (!store_.write_tmp(world_ + ".log", h) || !store_.replace(world_ + ".log")) {
        // the snapshot holds everything; the earlier log is of another generation now: no appends until a new log exists
        durable_ = false;
        log_(2, "transient edits: the journal log of world " + world_ + " could not be started; transient edits are refused");
        return false;
    }
    durable_ = true;
    log_bytes_ = h.size();
    return true;
}

bool Journal::maintain() {
    if (world_.empty()) return true;
    if (dirty_ || !durable_ || log_bytes_ > kCompactLogBytes) return compact();
    return true;
}

bool Journal::append_states(const std::vector<Key>& keys, const std::map<Key, Entry>& from) {
    if (keys.empty()) return true;
    if (!durable_) return false;
    std::string buf;
    buf.reserve(keys.size() * kRecord);
    for (const Key& k : keys) {
        const size_t at = buf.size();
        put(buf, kRecMagic);
        auto it = from.find(k);
        put_state(buf, k, it == from.end() || !it->second.mask.any() ? nullptr : &it->second);
        put(buf, fnv64(buf.data() + at, 4 + kState));
    }
    if (!store_.append(world_ + ".log", buf)) {
        // a part of the buffer may be on disk: nothing more may follow it until a compaction starts a new log
        durable_ = false;
        log_(2, "transient edits: the journal log of world " + world_ + " could not be appended to");
        return false;
    }
    log_bytes_ += buf.size();
    return true;
}

bool Journal::note_transient(const int32_t wo[3], const uint32_t wd[3], const Cell* old, const Cell* cur,
                             const uint8_t* addressed, std::string* why) {
    if (world_.empty()) {
        *why = "the world's identity is not known";
        return false;
    }
    const uint32_t bx = wd[0] / kBlock, by = wd[1] / kBlock, bz = wd[2] / kBlock;
    std::map<Key, Entry> next;
    std::vector<Key> keys;
    size_t fresh = 0;
    for (uint32_t gz = 0; gz < bz; ++gz)
        for (uint32_t gy = 0; gy < by; ++gy)
            for (uint32_t gx = 0; gx < bx; ++gx) {
                const Key k{wo[0] + static_cast<int32_t>(gx * kBlock), wo[1] + static_cast<int32_t>(gy * kBlock),
                            wo[2] + static_cast<int32_t>(gz * kBlock)};
                auto it = j_.find(k);
                Entry e = it == j_.end() ? Entry{} : it->second;
                bool changed = false;
                for (uint32_t z = 0; z < kBlock; ++z)
                    for (uint32_t y = 0; y < kBlock; ++y)
                        for (uint32_t x = 0; x < kBlock; ++x) {
                            const size_t at = ((static_cast<size_t>(gz) * kBlock + z) * wd[1] + gy * kBlock + y) * wd[0] +
                                              gx * kBlock + x;
                            if (!addressed[at]) continue;
                            const uint32_t i = index_of(x, y, z);
                            const bool known = e.mask.get(i);
                            if (!known && old[at] == cur[at]) continue;  // nothing changed, nothing to put back
                            if (!known) e.before[i] = old[at];
                            e.after[i] = cur[at];
                            e.mask.set(i);
                            e.pending.clear(i), e.restored.clear(i);
                            if (e.after[i] == e.before[i]) e.mask.clear(i);  // written back to what it was
                            changed = true;
                        }
                if (!changed) continue;
                if (it == j_.end() && e.mask.any()) ++fresh;
                next[k] = e;
                keys.push_back(k);
            }
    if (keys.empty()) return true;
    if (j_.size() + fresh > kMaxBlocks) {
        *why = "the transient journal is full (" + std::to_string(kMaxBlocks) + " blocks) until a restart puts them back";
        return false;
    }
    if (!durable_ && !compact()) {
        *why = "the transient journal cannot be written (see the error above)";
        return false;
    }
    if (!append_states(keys, next)) {
        *why = "the transient journal could not be written (is the disk full?)";
        return false;
    }
    for (const Key& k : keys) {
        if (next[k].mask.any()) j_[k] = next[k];
        else j_.erase(k);
    }
    count_pending();
    return true;
}

bool Journal::note_kept_before(const int32_t wo[3], const uint32_t wd[3], const uint8_t* addressed,
                               std::vector<std::pair<Key, Entry>>* undo, std::string* why) {
    undo->clear();
    if (j_.empty()) return true;
    const uint32_t bx = wd[0] / kBlock, by = wd[1] / kBlock, bz = wd[2] / kBlock;
    std::map<Key, Entry> next;
    std::vector<Key> keys;
    for (uint32_t gz = 0; gz < bz; ++gz)
        for (uint32_t gy = 0; gy < by; ++gy)
            for (uint32_t gx = 0; gx < bx; ++gx) {
                const Key k{wo[0] + static_cast<int32_t>(gx * kBlock), wo[1] + static_cast<int32_t>(gy * kBlock),
                            wo[2] + static_cast<int32_t>(gz * kBlock)};
                auto it = j_.find(k);
                if (it == j_.end()) continue;  // the ordinary kept write: nothing journaled here
                Entry e = it->second;
                bool changed = false;
                for (uint32_t z = 0; z < kBlock; ++z)
                    for (uint32_t y = 0; y < kBlock; ++y)
                        for (uint32_t x = 0; x < kBlock; ++x) {
                            const size_t at = ((static_cast<size_t>(gz) * kBlock + z) * wd[1] + gy * kBlock + y) * wd[0] +
                                              gx * kBlock + x;
                            const uint32_t i = index_of(x, y, z);
                            if (!addressed[at] || !e.mask.get(i)) continue;
                            e.mask.clear(i), e.pending.clear(i), e.restored.clear(i);
                            changed = true;
                        }
                if (!changed) continue;
                next[k] = e;
                keys.push_back(k);
            }
    if (keys.empty()) return true;
    if (!durable_ && !compact()) {
        *why = "the transient journal cannot be written (see the error above), and this edit sets journaled cells";
        return false;
    }
    if (!append_states(keys, next)) {
        *why = "the transient journal could not be written (is the disk full?), and this edit sets journaled cells";
        return false;
    }
    for (const Key& k : keys) {
        undo->emplace_back(k, j_.at(k));
        if (next[k].mask.any()) j_[k] = next[k];
        else j_.erase(k);
    }
    count_pending();
    return true;
}

void Journal::undo_kept(const std::vector<std::pair<Key, Entry>>& undo, const std::vector<Key>& keys) {
    std::vector<Key> back;
    std::map<Key, Entry> from;
    for (const auto& [k, e] : undo) {
        bool want = false;
        for (const Key& w : keys) want |= w == k;
        if (!want) continue;
        j_[k] = e;
        from[k] = e;
        back.push_back(k);
    }
    if (back.empty()) return;
    if (!append_states(back, from)) {
        // memory has the records again and the next compaction writes them; until then those cells (still transient,
        // since their commit never happened) have no record on disk, and appends stay off until that compaction
        dirty_ = true;
        log_(2, "transient edits: a faulted kept write's blocks could not be put back on record; the next compaction writes them");
    }
    count_pending();
}

std::vector<std::pair<Key, Entry>> Journal::pending_work(size_t max) {
    std::vector<std::pair<Key, Entry>> work;
    if (!pending_blocks_) return work;
    auto it = j_.upper_bound(cursor_);
    for (size_t seen = 0; seen < j_.size() && work.size() < max; ++seen, ++it) {
        if (it == j_.end()) it = j_.begin();
        cursor_ = it->first;
        if (it->second.pending.any()) work.emplace_back(it->first, it->second);
    }
    return work;
}

void Journal::settle(const Key& k, const Bits& back, const Bits& gone) {
    (void)gone;
    auto it = j_.find(k);
    if (it == j_.end()) return;
    Entry& e = it->second;
    for (uint32_t i = 0; i < kCells; ++i) {
        if (!e.pending.get(i)) continue;
        e.pending.clear(i);
        if (back.get(i)) e.restored.set(i);
        else e.mask.clear(i);  // changed since, or never saved: nothing to put back, and nothing to keep on record
    }
    if (!append_states({k}, j_)) dirty_ = true;
    if (!e.mask.any()) j_.erase(it);
    count_pending();
}

void Journal::retire_restored() {
    std::vector<Key> keys;
    for (auto& [k, e] : j_) {
        if (!e.restored.any()) continue;
        e.mask.minus(e.restored);
        e.restored = Bits{};
        keys.push_back(k);
    }
    if (keys.empty()) return;
    if (!append_states(keys, j_)) dirty_ = true;
    for (const Key& k : keys)
        if (auto it = j_.find(k); it != j_.end() && !it->second.mask.any()) j_.erase(it);
}

}  // namespace kb::tj
