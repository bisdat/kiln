// SPDX-License-Identifier: MIT
// kiln.survivors: the parts that need no game (request shape, the terminal-result cache, the event ring, outcome rules,
// zone geometry, the authorization decision). survivors.cpp is the glue to the game; tests/survivors_test.cpp runs this
// on the host.
#pragma once
#include <cmath>
#include <cstdint>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "services/kiln_survivors.h"

namespace kiln::survivors {

constexpr size_t kResultCache = 256;
constexpr uint64_t kResultKeepMs = 10ull * 60 * 1000;
constexpr size_t kEventRing = 4096;
constexpr uint32_t kWriteEffects = KILN_SURV_KNOWLEDGE_WRITTEN | KILN_SURV_TOOL_GIVEN | KILN_SURV_ENTITY_CREATED |
                                   KILN_SURV_HOME_MOVED | KILN_SURV_BED_CHANGED;

// FNV-1a-32 of a template's 16 GUID bytes: the game's NpcId.
inline uint32_t npc_id_of(const uint8_t guid[16]) {
    uint32_t h = 0x811c9dc5u;
    for (int i = 0; i < 16; ++i) h = (h ^ guid[i]) * 0x01000193u;
    return h;
}

inline bool zero16(const uint8_t b[16]) {
    for (int i = 0; i < 16; ++i)
        if (b[i]) return false;
    return true;
}

// The request's shape (submit, any thread): nullptr when it is well formed, else why not (INVALID_ARGUMENT).
inline const char* check_request(const KilnSurvivorRequest* r) {
    if (!r) return "no request";
    if (r->size != sizeof(KilnSurvivorRequest)) return "size is not sizeof(KilnSurvivorRequest)";
    if (r->npc_id == 0) return "npc_id is 0";
    if (r->reserved || r->reserved1 || r->reserved2) return "reserved fields must be 0";
    const bool bed_op = r->op == KILN_SURV_OP_ASSIGN_BED || r->op == KILN_SURV_OP_UNASSIGN_BED;
    switch (r->op) {
    case KILN_SURV_OP_RECRUIT:
        if (r->flags & ~uint32_t(KILN_SURV_GIVE_TOOL)) return "unknown flags";
        if ((r->flags & KILN_SURV_GIVE_TOOL) && r->recipient == 0) return "GIVE_TOOL needs a recipient";
        if (!(r->flags & KILN_SURV_GIVE_TOOL) && r->recipient != 0) return "a recipient needs GIVE_TOOL";
        break;
    case KILN_SURV_OP_SUMMON:
        if (r->flags || r->recipient) return "SUMMON takes no flags and no recipient";
        if (zero16(r->base_id)) return "SUMMON needs base_id";
        if (!std::isfinite(r->position.x) || !std::isfinite(r->position.y) || !std::isfinite(r->position.z) ||
            !std::isfinite(r->yaw_degrees))
            return "position and yaw must be finite";
        if (std::fabs(r->position.x) > 1048576.0 || std::fabs(r->position.y) > 1048576.0 || std::fabs(r->position.z) > 1048576.0)
            return "position is out of range";
        break;
    case KILN_SURV_OP_ASSIGN_BED:
    case KILN_SURV_OP_UNASSIGN_BED:
        if (r->flags || r->recipient) return "bed operations take no flags and no recipient";
        if (r->op == KILN_SURV_OP_ASSIGN_BED && r->bed == 0) return "ASSIGN_BED needs a bed";
        if (r->op == KILN_SURV_OP_UNASSIGN_BED && r->bed != 0) return "UNASSIGN_BED takes no bed";
        if (r->authorization == KILN_SURV_AUTH_PLAYER_PERMISSIONS) {
            if (r->actor == 0) return "PLAYER_PERMISSIONS needs an actor";
        } else if (r->authorization == KILN_SURV_AUTH_TRUSTED_SERVER) {
            if (r->actor != 0) return "TRUSTED_SERVER takes no actor";
        } else {
            return "unknown authorization";
        }
        break;
    default:
        return "unknown op";
    }
    if (!bed_op && (r->authorization || r->actor)) return "authorization and actor are for bed operations";
    if (r->op != KILN_SURV_OP_ASSIGN_BED && r->bed) return "bed is for ASSIGN_BED";
    if (r->op != KILN_SURV_OP_SUMMON && !zero16(r->base_id)) return "base_id is for SUMMON";
    return nullptr;
}

// A roster unlock Kiln can grant safely: none (always unlocked) or a plain world Bool compared with "true".
// type: KILN_KNOW_Q_* (1 = BOOL), compare: KILN_KNOW_CMP_* (0 GREATER, 2 EQUALS).
inline bool unlock_supported(uint32_t id, uint8_t type, uint8_t compare, uint32_t value, uint8_t player_only) {
    if (id == 0) return true;
    if (type != 1 || player_only) return false;
    return (compare == 2 && value == 1) || (compare == 0 && value == 0);
}

// How a request that cannot finish ends: PARTIAL when it wrote anything, else the given no-write status.
inline uint32_t failed_status(uint32_t effects, uint32_t no_write_status) {
    return (effects & kWriteEffects) ? uint32_t(KILN_SURV_PARTIAL) : no_write_status;
}

inline bool in_box(const KilnVec3& center, const KilnVec3& half, const KilnVec3& p) {
    return std::fabs(p.x - center.x) <= half.x && std::fabs(p.y - center.y) <= half.y && std::fabs(p.z - center.z) <= half.z;
}

inline double distance(const KilnVec3& a, const KilnVec3& b) {
    const double dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

// ---- authorization (D5)
struct ActorView {
    bool found = false;        // kiln.players knows the entity as a player
    bool in_game = false;      // state IN_GAME
    bool permissions_known = false;
    bool can_edit_base = false;
    uint32_t handle = 0;       // the connection handle now
};
struct BedView {
    bool in_base = false;      // inside a loaded base zone
    bool abandoned = false;    // that base is abandoned
    bool denied = false;       // kiln.gameplay's zone rules deny building there to the actor
};
// nullptr = allowed, else "Code: words". accepted_handle: the actor's handle when the request was taken (0 = take now).
inline const char* authorize(uint32_t mode, const ActorView& a, uint32_t accepted_handle, const std::vector<BedView>& beds) {
    if (mode == KILN_SURV_AUTH_TRUSTED_SERVER) return nullptr;
    if (!a.found || !a.in_game) return "ActorNotInGame: the actor is not a player in game";
    if (accepted_handle && a.handle != accepted_handle) return "ActorSessionChanged: the actor's slot belongs to another connection now";
    if (!a.permissions_known) return "PermissionsUnknown: the actor's permissions are not known";
    if (!a.can_edit_base) return "PermissionDenied: the actor may not edit bases";
    for (const BedView& b : beds) {
        if (!b.in_base || b.abandoned) return "BedOutsideBase: a bed it touches is not in a loaded, kept base";
        if (b.denied) return "PermissionDenied: the zone rules do not let the actor build there";
    }
    return nullptr;
}

// ---- terminal results: FIFO of 256, each for 10 minutes. Thread-safe.
class ResultCache {
public:
    void put(const KilnSurvivorResult& r, uint64_t now_ms) {
        std::lock_guard lock(mu_);
        expire(now_ms);
        if (order_.size() >= kResultCache) {
            map_.erase(order_.front().first);
            order_.pop_front();
        }
        map_[r.request] = r;
        order_.emplace_back(r.request, now_ms);
    }
    bool get(uint64_t id, uint64_t now_ms, KilnSurvivorResult* out) {
        std::lock_guard lock(mu_);
        expire(now_ms);
        auto it = map_.find(id);
        if (it == map_.end()) return false;
        *out = it->second;
        return true;
    }
    size_t size() {
        std::lock_guard lock(mu_);
        return map_.size();
    }

private:
    void expire(uint64_t now_ms) {
        while (!order_.empty() && now_ms - order_.front().second >= kResultKeepMs) {
            map_.erase(order_.front().first);
            order_.pop_front();
        }
    }
    std::mutex mu_;
    std::unordered_map<uint64_t, KilnSurvivorResult> map_;
    std::deque<std::pair<uint64_t, uint64_t>> order_;
};

// ---- events: the last 4096. Thread-safe.
class EventRing {
public:
    uint64_t push(KilnSurvivorEvent e) {
        std::lock_guard lock(mu_);
        e.size = sizeof e;
        e.sequence = ++seq_;
        ring_.push_back(e);
        if (ring_.size() > kEventRing) ring_.pop_front();
        return seq_;
    }
    uint32_t read(uint64_t after, KilnSurvivorEvent* out, uint32_t cap) {
        std::lock_guard lock(mu_);
        uint32_t n = 0;
        for (const KilnSurvivorEvent& e : ring_) {
            if (e.sequence <= after) continue;
            if (n >= cap) break;
            out[n++] = e;
        }
        return n;
    }
    uint64_t last() {
        std::lock_guard lock(mu_);
        return seq_;
    }

private:
    std::mutex mu_;
    std::deque<KilnSurvivorEvent> ring_;
    uint64_t seq_ = 0;
};

inline void set_text(char* dst, size_t cap, const std::string& s) {
    const size_t n = s.size() < cap - 1 ? s.size() : cap - 1;
    std::memcpy(dst, s.data(), n);
    dst[n] = 0;
}

}  // namespace kiln::survivors
