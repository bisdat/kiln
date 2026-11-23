// SPDX-License-Identifier: MIT
#pragma once
#include "stations_logic.h"
#include <mutex>

namespace kitems {
// Immutable registrations. No managed callback runs in a game program's payment hook.
struct CraftSource {
    uint64_t owner = 0, handle = 0;
    uint32_t player = 0, container = 0;
    bool after_outputs = false, deny = true;
    std::vector<uint32_t> items;
    uint32_t player_session = 0;
    bool allows(uint32_t item) const {
        const bool found = std::binary_search(items.begin(), items.end(), item);
        return item && (deny ? !found : found);
    }
};
inline bool source_identity_ok(uint32_t player, uint32_t container, bool character, bool inventory) {
    // Characters (including somebody else's bag) are not linked storage containers.
    return player && player <= 1023 && container > 1023 && !character && inventory;
}
class CraftSourceRegistry {
    mutable std::mutex mutex_;
    uint64_t next_ = 0;
    std::vector<CraftSource> sources_;
public:
    uint64_t add(uint64_t owner, uint32_t player, uint32_t container, bool after, bool deny, std::vector<uint32_t> items, uint32_t session = 0) {
        std::lock_guard lock(mutex_);
        // This player's registrations from an earlier login are dead (every snapshot drops them): free their slots, so
        // a mod that re-registers on each join without disposing cannot exhaust the shared 4096 limit for everyone.
        sources_.erase(std::remove_if(sources_.begin(),sources_.end(),[&](const CraftSource& s) { return s.player == player && s.player_session != session; }),sources_.end());
        if (sources_.size() >= 4096) return 0;
        std::sort(items.begin(), items.end());
        items.erase(std::unique(items.begin(), items.end()), items.end());
        const auto id = ++next_;
        sources_.push_back({owner,id,player,container,after,deny,std::move(items),session});
        return id;
    }
    bool remove(uint64_t owner, uint64_t handle) {
        std::lock_guard lock(mutex_);
        const auto it = std::find_if(sources_.begin(),sources_.end(),[&](const CraftSource& s) { return s.owner == owner && s.handle == handle; });
        if (it == sources_.end()) return false;
        sources_.erase(it); return true;
    }
    void cancel_owner(uint64_t owner) {
        std::lock_guard lock(mutex_);
        sources_.erase(std::remove_if(sources_.begin(),sources_.end(),[&](const CraftSource& s) { return s.owner == owner; }),sources_.end());
    }
    bool empty() const { std::lock_guard lock(mutex_); return sources_.empty(); }
    void clear() { std::lock_guard lock(mutex_); sources_.clear(); }
    std::vector<CraftSource> copy(uint32_t player) const {
        std::lock_guard lock(mutex_);
        std::vector<CraftSource> out;
        for (const auto& s : sources_) if (s.player == player) out.push_back(s);
        return out;
    }
};

// Per-item order: normal bag/base sources, then the registrations around station outputs. A container is counted once
// per item, even if several registrations or an opted-in station name it. Registration order breaks ties.
template<class Access, class Count>
void merge_craft_sources(const std::vector<CraftSource>& sources, uint32_t player, uint32_t exclude,
                         const std::vector<uint32_t>& order, const std::vector<uint32_t>& items,
                         Access access, Count count, stations::CraftLent* lent) {
    for (uint32_t item : items) {
        std::unordered_set<uint32_t> seen(order.begin(),order.end());
        seen.insert(exclude);
        std::vector<uint32_t> out;
        uint64_t total = 0;
        auto push = [&](uint32_t e) { if (seen.insert(e).second) { const uint64_t n = count(e,item); if(n) {out.push_back(e); total += n;} } };
        auto registered = [&](bool after) {
            for (const auto& s : sources)
                if (s.player == player && s.after_outputs == after && s.allows(item) && !seen.count(s.container) && access(player,s.container))
                    push(s.container);
        };
        registered(false);
        for (uint32_t e : lent->by[item]) push(e);
        registered(true);
        lent->by[item] = std::move(out);
        lent->count[item] = total;
    }
}
} // namespace kitems
