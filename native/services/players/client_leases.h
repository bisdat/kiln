// SPDX-License-Identifier: MIT
// kiln.playerstate: which client actions are held, by whom (client_action_hold and client_action_interest), with
// nothing of the game or of Windows in it. playerstate.cpp asks it at the hold point; tests/client_leases_test.cpp
// runs it on the host.
//
// Two kinds of lease, both renewed every Pre by their caller and ended by themselves after `stale` hold points
// without a renewal:
//   - the legacy wildcard lease (client_action_hold): every holdable action of the listed players;
//   - selective leases (client_action_interest, since 0.2.0-dev.29), one per owner: the listed players x the listed
//     action indices (no index: every action). A typed equipment listener holds changeEquipmentAction and
//     swapEquipmentSetAction only, so no unrelated client action waits a step for it.
// An action is held when any lease covers (player, action). One held record per (player, action, version) stays the
// hold point's own business: the leases only say what to hold.
#pragma once
#include <algorithm>
#include <cstdint>
#include <utility>
#include <vector>

namespace kiln::playerstate {

class ClientLeases {
public:
    static constexpr uint32_t kMaxEntities = 1024, kMaxActions = 64, kMaxOwners = 32;

    explicit ClientLeases(uint32_t stale = 8) : stale_(stale) {}

    // client_action_hold: the wildcard lease. count 0 ends it.
    void set_wildcard(const uint32_t* entities, uint32_t count) {
        wildcard_.assign(entities, entities + count);
        normalize(wildcard_);
        wildcard_age_ = 0;
    }

    // client_action_interest: replaces `owner`'s selective lease. entity_count 0 removes it; action_count 0 means
    // every action. False for a bad argument (owner 0, too many ids, too many owners): nothing changed.
    bool set(uint64_t owner, const uint32_t* entities, uint32_t entity_count, const uint32_t* actions, uint32_t action_count) {
        if (owner == 0 || entity_count > kMaxEntities || action_count > kMaxActions || (entity_count && !entities) ||
            (action_count && !actions))
            return false;
        auto it = std::find_if(leases_.begin(), leases_.end(), [&](const Lease& l) { return l.owner == owner; });
        if (entity_count == 0) {
            if (it != leases_.end()) leases_.erase(it);
            return true;
        }
        if (it == leases_.end()) {
            if (leases_.size() >= kMaxOwners) return false;
            Lease fresh;
            fresh.owner = owner;
            leases_.push_back(std::move(fresh));
            it = leases_.end() - 1;
        }
        it->entities.assign(entities, entities + entity_count);
        normalize(it->entities);
        it->actions.assign(actions, actions + action_count);
        sort_once(it->actions);  // index 0 is an action like any other
        it->age = 0;
        return true;
    }

    // One hold point (the step's DeliverEvents): leases nobody renewed for `stale` hold points end. Returns how many
    // ended (the wildcard counts as one).
    uint32_t age() {
        uint32_t ended = 0;
        if (!wildcard_.empty() && ++wildcard_age_ > stale_) {
            wildcard_.clear();
            ++ended;
        }
        for (auto it = leases_.begin(); it != leases_.end();) {
            if (++it->age > stale_) {
                it = leases_.erase(it);
                ++ended;
            } else {
                ++it;
            }
        }
        return ended;
    }

    bool empty() const { return wildcard_.empty() && leases_.empty(); }
    bool wildcard_empty() const { return wildcard_.empty(); }

    // Whether (entity, action) is held by any lease.
    bool covers(uint32_t entity, uint32_t action) const {
        if (has(wildcard_, entity)) return true;
        for (const Lease& l : leases_)
            if (has(l.entities, entity) && (l.actions.empty() || has(l.actions, action))) return true;
        return false;
    }

    // Whether any lease names `entity` (for some action).
    bool covers_entity(uint32_t entity) const {
        if (has(wildcard_, entity)) return true;
        for (const Lease& l : leases_)
            if (has(l.entities, entity)) return true;
        return false;
    }

    // Every entity some lease names, ascending, once.
    std::vector<uint32_t> entities() const {
        std::vector<uint32_t> all = wildcard_;
        for (const Lease& l : leases_) all.insert(all.end(), l.entities.begin(), l.entities.end());
        normalize(all);
        return all;
    }

    void clear() {
        wildcard_.clear();
        leases_.clear();
    }

private:
    struct Lease {
        uint64_t owner = 0;
        std::vector<uint32_t> entities, actions;  // ascending, once
        uint32_t age = 0;
    };

    static void sort_once(std::vector<uint32_t>& v) {
        std::sort(v.begin(), v.end());
        v.erase(std::unique(v.begin(), v.end()), v.end());
    }
    // entity ids: ascending, once, never 0 (no entity)
    static void normalize(std::vector<uint32_t>& v) {
        sort_once(v);
        v.erase(std::remove(v.begin(), v.end(), 0u), v.end());
    }
    static bool has(const std::vector<uint32_t>& v, uint32_t x) { return std::binary_search(v.begin(), v.end(), x); }

    uint32_t stale_;
    std::vector<uint32_t> wildcard_;
    uint32_t wildcard_age_ = 0;
    std::vector<Lease> leases_;
};

}  // namespace kiln::playerstate
