// SPDX-License-Identifier: MIT
// kiln.decisions: the immutable entity metadata table and the life-serial cache, with nothing of the game in it.
//
// Hooks inside game programs may not read the world (other entities' components, player records): they run in the
// sim step, often on workers. What they need to name an entity (which life of this id is it, where was it, whose
// session) is sampled once per step at window_begin, when no world job is live, and published as one immutable
// table. A hook takes the table (a shared_ptr copy) and looks its entity up; nothing in a published table changes.
//
// The game side is a Sampler (decisions.cpp reads kiln.world and kiln.players through it; tests fake it).
#pragma once
#include <algorithm>
#include <atomic>
#include <cstring>
#include <iterator>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "services/kiln_decisions.h"

namespace kiln::decisions {

// What one world-safe look at an entity gives.
struct Sample {
    bool exists = false;
    bool has_spawn_time = false;
    uint64_t spawn_time = 0;
    bool has_position = false;
    double position[3] = {};
    bool player_in_game = false;  // ids 1..1023 only: a current InGame player record
    uint64_t player_handle = 0, joined_step = 0;
};
using Sampler = bool (*)(uint32_t entity, Sample* out, void* user);  // false: could not look (treated as absent)

struct Row {
    KilnEventEntityKey key{};
    uint32_t has_position = 0;
    double position[3] = {};
    uint64_t sampled_step = 0;
};

struct Table {
    uint64_t world_epoch = 0, step = 0;
    std::vector<Row> rows;  // sorted by key.entity

    const Row* find(uint32_t entity) const {
        auto it = std::lower_bound(rows.begin(), rows.end(), entity, [](const Row& r, uint32_t e) { return r.key.entity < e; });
        return it != rows.end() && it->key.entity == entity ? &*it : nullptr;
    }
};

inline bool is_player_id(uint32_t entity) { return entity >= 1 && entity <= 1023; }

class Metadata {
public:
    void set_sampler(Sampler fn, void* user) {
        sampler_ = fn;
        user_ = user;
    }

    // The table of the current step (never null). Any thread.
    std::shared_ptr<const Table> table() const { return published_.load(); }

    // One row, sampled now (world-safe phase, world thread: the caller checked). False: no entity or no usable
    // identity; *miss says which (true = the entity is there but cannot be told from another life of its id).
    bool resolve(uint64_t epoch, uint64_t step, uint32_t entity, Row* out, bool* miss = nullptr) {
        if (miss) *miss = false;
        std::lock_guard lock(m_);
        return sample_locked(epoch, step, entity, out, miss);
    }

    // window_begin: samples `wanted` (entity ids, any order, duplicates allowed) and publishes the table. Returns
    // false and publishes an EMPTY table when more than KILN_DECISIONS_MAX_METADATA distinct ids are wanted: the
    // caller turns the producers off and says so; nothing is covered silently in part. *misses counts wanted
    // entities that exist without a usable identity.
    bool rebuild(uint64_t epoch, uint64_t step, std::vector<uint32_t>& wanted, uint32_t* misses) {
        if (misses) *misses = 0;
        std::sort(wanted.begin(), wanted.end());
        wanted.erase(std::unique(wanted.begin(), wanted.end()), wanted.end());
        auto t = std::make_shared<Table>();
        t->world_epoch = epoch;
        t->step = step;
        std::lock_guard lock(m_);
        if (wanted.size() > KILN_DECISIONS_MAX_METADATA) {
            published_.store(std::shared_ptr<const Table>(std::move(t)));
            return false;
        }
        t->rows.reserve(wanted.size());
        for (uint32_t e : wanted) {
            if (e == 0) continue;
            Row r;
            bool miss = false;
            if (sample_locked(epoch, step, e, &r, &miss)) t->rows.push_back(r);
            else if (miss && misses) ++*misses;
        }
        // A life keeps its serial for as long as it is remembered, also while nobody asks about it. Only when the
        // cache grows past four tables' worth are the lives not seen for the longest time dropped; such an entity
        // gets a new serial when it is seen again (a pending request that names the old one ends StaleEntity).
        if (lives_.size() > kMaxLives) {
            std::vector<uint64_t> seen;
            seen.reserve(lives_.size());
            for (const auto& [e, l] : lives_) seen.push_back(l.seen_step);
            std::nth_element(seen.begin(), seen.begin() + static_cast<long>(lives_.size() - kMaxLives / 2), seen.end());
            const uint64_t cut = seen[lives_.size() - kMaxLives / 2];
            for (auto it = lives_.begin(); it != lives_.end();) it = it->second.seen_step < cut ? lives_.erase(it) : std::next(it);
        }
        published_.store(std::shared_ptr<const Table>(std::move(t)));
        return true;
    }

    // World stop, or the item instance counter went back (a rolled-back world): every row and every life is void.
    void clear() {
        std::lock_guard lock(m_);
        lives_.clear();
        published_.store(std::shared_ptr<const Table>(std::make_shared<Table>()));
    }

private:
    static constexpr size_t kMaxLives = 4 * KILN_DECISIONS_MAX_METADATA;
    struct Life {
        uint64_t epoch = 0, spawn_time = 0, player_handle = 0, joined_step = 0, serial = 0, seen_step = 0;
        uint32_t flags = 0;
    };

    bool sample_locked(uint64_t epoch, uint64_t step, uint32_t entity, Row* out, bool* miss) {
        Sample s;
        if (entity == 0 || !sampler_ || !sampler_(entity, &s, user_) || !s.exists) return false;
        uint32_t flags = 0;
        if (is_player_id(entity)) {
            // A player id needs a current in-game session; an absent record is missing metadata, never a made-up one.
            if (!s.player_in_game) {
                if (miss) *miss = true;
                return false;
            }
            flags |= KILN_EVENT_KEY_PLAYER_SESSION;
        }
        if (s.has_spawn_time) flags |= KILN_EVENT_KEY_SPAWN_TIME;
        if (flags == 0) {  // not a player and no SpawnTime: this life cannot be told from the next one
            if (miss) *miss = true;
            return false;
        }
        const uint64_t spawn = s.has_spawn_time ? s.spawn_time : 0;
        const uint64_t handle = (flags & KILN_EVENT_KEY_PLAYER_SESSION) ? s.player_handle : 0;
        const uint64_t joined = (flags & KILN_EVENT_KEY_PLAYER_SESSION) ? s.joined_step : 0;
        Life& l = lives_[entity];
        if (l.serial == 0 || l.epoch != epoch || l.flags != flags || l.spawn_time != spawn || l.player_handle != handle ||
            l.joined_step != joined) {
            l = Life{epoch, spawn, handle, joined, ++next_serial_, step, flags};
        }
        l.seen_step = step;
        Row r;
        r.key.entity = entity;
        r.key.flags = flags;
        r.key.spawn_time = spawn;
        r.key.life_serial = l.serial;
        r.key.player_handle = handle;
        r.key.joined_step = joined;
        r.has_position = s.has_position ? 1 : 0;
        if (s.has_position) std::memcpy(r.position, s.position, sizeof r.position);
        r.sampled_step = step;
        *out = r;
        return true;
    }

    mutable std::mutex m_;
    Sampler sampler_ = nullptr;
    void* user_ = nullptr;
    std::unordered_map<uint32_t, Life> lives_;
    uint64_t next_serial_ = 0;  // process-wide: never reset, also not by clear()
    std::atomic<std::shared_ptr<const Table>> published_{std::make_shared<Table>()};
};

}  // namespace kiln::decisions
