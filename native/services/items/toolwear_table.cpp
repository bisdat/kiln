// SPDX-License-Identifier: MIT
// kiln.toolwear's table (kiln_toolwear.h): the owners' rules, the change records and the status. The hooks
// (toolwear.cpp) read one number per item, lock-free; everything here is above that, on the tick thread:
//   - a rule is a layer of one owner for one item. The latest defined live layer is in effect; removing it (or its
//     owner going) reveals the one before, or the game's own debit;
//   - the effective cost is written to the hooks' table whenever it changes, and the generation grows by one;
//   - every change of the generation is one rule-change record (whose rule, what cost, before and after, and what
//     made it), kept in a history of the last 1024 that readers read with a cursor of their own (rule_changes): a
//     native plugin's define/remove and an owner's retirement reach Kiln's managed events as a managed mod's do;
//   - status() and rule_changes() answer from a copy under a mutex, for any thread.
#include "toolwear.h"

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "engine.h"
#include "kiln_api.h"
#include "services/kiln_hooks.h"
#include "services/kiln_items.h"
#include "services/kiln_toolwear.h"
#include "xfer.h"

namespace kitems {
KilnResult items_item_info(uint32_t id, KilnItemInfo* out);
}

namespace kitems::toolwear {
namespace {

struct Layer {
    KilnHandle handle;
    KilnOwner owner;
    std::string mod;
    uint32_t item;
    int32_t cost;
    uint64_t order;  // the latest defined wins
};

// The rule in effect for one item, as a value (a Layer can go while a change is made).
struct Effective {
    int32_t cost = -1;  // -1: no rule
    KilnHandle handle = 0;
    KilnOwner owner = 0;
    std::string mod;
};

constexpr size_t kRuleHistory = 1024;

const KilnApi* g_api = nullptr;
std::mutex g_mu;  // the layers, the generation and the rule history: tick thread (and retirement) writes, status() reads
std::vector<Layer> g_layers;
std::unordered_map<KilnOwner, std::string> g_mod_ids;  // bound for the owner lifetime, even with no live layers
std::unordered_set<KilnOwner> g_retired;
uint64_t g_generation = 0, g_next_order = 1;
KilnHandle g_next_handle = 1;
// the record of generation n is at (n - 1) % kRuleHistory; generations newer than g_generation - kRuleHistory are held
KilnToolWearRuleChange g_history[kRuleHistory];

constexpr uint8_t kCategoryTools = 3, kCategoryBuildTools = 6;  // the game files its pickaxes under build tools (with an item entity)
constexpr uint32_t kFlagHasPide = 0x2;

const Layer* winner_locked(uint32_t item) {
    const Layer* best = nullptr;
    for (const Layer& l : g_layers)
        if (l.item == item && (!best || l.order > best->order)) best = &l;
    return best;
}

Effective effective_locked(uint32_t item) {
    const Layer* w = winner_locked(item);
    return w ? Effective{w->cost, w->handle, w->owner, w->mod} : Effective{};
}

// After a change of `item`'s layers: the hooks' number follows the winner. False: the hooks' table is full (nothing
// changed). A change of the cost in effect, or of whose rule it is, is one generation and one record.
bool publish_locked(uint32_t item, const Effective& before, uint32_t cause) {
    Effective now = effective_locked(item);
    if (!set_rule(item, now.cost)) return false;
    if (now.cost == before.cost && now.handle == before.handle) return true;
    KilnToolWearRuleChange& r = g_history[g_generation % kRuleHistory];
    r = KilnToolWearRuleChange{};
    r.size = sizeof r;
    r.item = item;
    r.generation = ++g_generation;
    r.cause = cause;
    r.before_cost = before.cost;
    r.after_cost = now.cost;
    r.before_owner = before.cost >= 0 ? before.owner : 0;
    r.after_owner = now.cost >= 0 ? now.owner : 0;
    r.before_rule = before.handle;
    r.after_rule = now.handle;
    std::snprintf(r.before_mod_id, sizeof r.before_mod_id, "%s", before.mod.c_str());
    std::snprintf(r.after_mod_id, sizeof r.after_mod_id, "%s", now.mod.c_str());
    return true;
}

bool ascii_id(KilnStr s) {
    if (!s.ptr || !s.len || s.len > 127) return false;
    for (size_t i = 0; i < s.len; ++i)
        if (static_cast<unsigned char>(s.ptr[i]) < 0x20 || static_cast<unsigned char>(s.ptr[i]) > 0x7e) return false;
    return true;
}

// 0: a rule can be set for the item; else the result code, with the reason in `why`.
KilnResult supported(uint32_t item, char* why, size_t cap) {
    KilnItemInfo info{};
    info.size = sizeof info;
    const KilnResult rc = items_item_info(item, &info);
    if (rc == KILN_ERR_UNAVAILABLE) {
        std::snprintf(why, cap, "PendingContent");
        return KILN_TOOLWEAR_PENDING;
    }
    if (rc != KILN_OK) {
        std::snprintf(why, cap, "UnknownItem: the game has no item 0x%08x", item);
        return KILN_ERR_NOT_FOUND;
    }
    // a tool of either category that has an item entity of its own (HasPIDE) and is gear (durability): axes, pickaxes. A
    // build tool without an item entity (a rake) and a torch (no item entity; it burns down by another path) are refused.
    if ((info.category != kCategoryTools && info.category != kCategoryBuildTools) || !(info.item_flags & kFlagHasPide) || !info.gear) {
        std::snprintf(why, cap, "UnsupportedTool: %.*s is not a tool with an item entity and durability of its own (a torch burns down by another "
                                "path; build tools without one and other categories are not debited per use)", static_cast<int>(info.name.len), info.name.ptr);
        return KILN_TOOLWEAR_UNSUPPORTED_TOOL;
    }
    return KILN_OK;
}

KilnResult KILN_CALL t_define(KilnOwner owner, KilnStr mod, uint32_t item, uint32_t per_debit, KilnHandle* out) {
    if (!g_api->on_tick_thread()) return KILN_ERR_WRONG_THREAD;
    if (!out || !item || per_debit > static_cast<uint32_t>(kMaxCost) || !ascii_id(mod)) return KILN_ERR_INVALID_ARGUMENT;
    *out = 0;
    if (!status().on) return KILN_ERR_UNAVAILABLE;
    if (!g_api->server_world()) return KILN_TOOLWEAR_NO_AUTHORITATIVE_WORLD;
    char why[192];
    if (const KilnResult rc = supported(item, why, sizeof why); rc != KILN_OK) return rc;
    std::lock_guard lock(g_mu);
    if (g_retired.count(owner)) return KILN_ERR_REVOKED;
    if (const auto id = g_mod_ids.find(owner); id != g_mod_ids.end() && id->second != std::string(mod.ptr, mod.len))
        return KILN_ERR_INVALID_ARGUMENT;
    // an equal cost under another owner's rule is a change too (publish_locked compares the handle as well)
    if (!activate()) return KILN_ERR_UNAVAILABLE;
    const Effective before = effective_locked(item);
    for (Layer& l : g_layers)
        if (l.owner == owner && l.item == item) {
            // the same owner again: its layer, updated in place and moved to the top (its handle stays)
            const Layer old = l;
            l.cost = static_cast<int32_t>(per_debit);
            l.mod.assign(mod.ptr, mod.len);
            l.order = g_next_order++;
            if (!publish_locked(item, before, KILN_TOOLWEAR_RULE_DEFINED)) {
                l = old;
                return KILN_TOOLWEAR_TOO_MANY_RULES;
            }
            *out = l.handle;
            return KILN_OK;
        }
    g_layers.push_back(Layer{g_next_handle, owner, std::string(mod.ptr, mod.len), item, static_cast<int32_t>(per_debit), g_next_order++});
    if (!publish_locked(item, before, KILN_TOOLWEAR_RULE_DEFINED)) {
        g_layers.pop_back();
        return KILN_TOOLWEAR_TOO_MANY_RULES;
    }
    g_mod_ids.emplace(owner, std::string(mod.ptr, mod.len));
    *out = g_next_handle++;
    return KILN_OK;
}

// Takes layers out by a predicate and publishes each item they touched (in the order of their first layer).
template <typename Pred>
size_t remove_locked(Pred&& gone, uint32_t cause) {
    std::vector<std::pair<uint32_t, Effective>> touched;  // item -> the rule in effect before
    for (const Layer& l : g_layers)
        if (gone(l) && std::none_of(touched.begin(), touched.end(), [&](const auto& t) { return t.first == l.item; }))
            touched.emplace_back(l.item, effective_locked(l.item));
    const size_t before = g_layers.size();
    g_layers.erase(std::remove_if(g_layers.begin(), g_layers.end(), gone), g_layers.end());
    for (const auto& [item, was] : touched) publish_locked(item, was, cause);  // taking a rule away always fits the hooks' table
    return before - g_layers.size();
}

KilnResult KILN_CALL t_remove(KilnOwner caller, KilnHandle rule) {
    if (!g_api->on_tick_thread()) return KILN_ERR_WRONG_THREAD;
    if (!rule) return KILN_ERR_INVALID_ARGUMENT;
    std::lock_guard lock(g_mu);
    for (const Layer& l : g_layers)
        if (l.handle == rule) {
            if (l.owner != caller) return KILN_TOOLWEAR_NOT_OWNER;
            remove_locked([rule](const Layer& x) { return x.handle == rule; }, KILN_TOOLWEAR_RULE_REMOVED);
            return KILN_OK;
        }
    return KILN_ERR_NOT_FOUND;
}

KilnResult KILN_CALL t_changes(KilnToolWearChange* out, uint32_t capacity, uint32_t stride, uint32_t* count) {
    if (!g_api->on_tick_thread()) return KILN_ERR_WRONG_THREAD;
    if (!count || (capacity && (!out || stride < offsetof(KilnToolWearChange, sequence_id)))) return KILN_ERR_INVALID_ARGUMENT;
    *count = 0;
    KilnSessionInfo session{};
    session.size = sizeof session;
    const bool have = g_api->session(&session) == KILN_OK;
    Change batch[64];
    const size_t record = std::min<size_t>(stride, sizeof(KilnToolWearChange));
    while (*count < capacity) {
        const size_t n = drain(batch, std::min<size_t>(64, capacity - *count));
        if (!n) break;
        for (size_t i = 0; i < n; ++i) {
            const Change& c = batch[i];
            KilnToolWearChange r{};
            r.size = static_cast<uint32_t>(record);
            r.source = c.source;
            r.world_session = have ? session.sessions[KILN_WORLD_SERVER] : 0;
            r.step = c.step;
            r.actor = c.actor;
            r.item_pide = c.item_entity;
            r.item_id = c.item_id;
            r.instance_stamp = c.stamp;
            r.before = c.before;
            r.after = c.after;
            r.maximum = c.maximum;
            r.original_delta = c.original_delta;
            r.requested_delta = c.requested_delta;
            r.sequence_id = c.sequence_id;
            r.call_index = c.call_index;
            r.trigger_time = c.trigger_time;
            r.trigger_type = c.trigger_type;
            std::memcpy(reinterpret_cast<uint8_t*>(out) + static_cast<size_t>(*count) * stride, &r, record);
            ++*count;
        }
    }
    return KILN_OK;
}

KilnResult KILN_CALL t_status(uint32_t item, KilnToolWearState* out) {
    if (!out || out->size < offsetof(KilnToolWearState, owner_mod_id)) return KILN_ERR_INVALID_ARGUMENT;
    const uint32_t size = std::min<uint32_t>(out->size, sizeof(KilnToolWearState));
    KilnToolWearState s{};
    s.size = size;
    const Status st = status();
    s.dropped_events = st.dropped;
    s.hook_threads = st.threads;  // the tail: copied below only as far as the caller's size reaches
    s.hit_seen = st.hit_seen;
    s.hit_applied = st.hit_applied;
    s.spawn_seen = st.spawn_seen;
    s.spawn_applied = st.spawn_applied;
    if (!st.on) {
        std::snprintf(s.reason, sizeof s.reason, "%s", st.why[0] ? st.why : "items.tool_wear_debit is off");
    } else if (item && !g_api->server_world()) {
        std::snprintf(s.reason, sizeof s.reason, "NoAuthoritativeWorld");
    } else if (item) {
        const KilnResult rc = supported(item, s.reason, sizeof s.reason);
        s.pending = rc == KILN_TOOLWEAR_PENDING;
        s.ok = rc == KILN_OK;
    } else {
        s.ok = 1;
    }
    {
        std::lock_guard lock(g_mu);
        s.generation = g_generation;
        if (const Layer* w = item ? winner_locked(item) : nullptr) {
            s.has_rule = 1;
            s.per_debit = static_cast<uint32_t>(w->cost);
            std::snprintf(s.owner_mod_id, sizeof s.owner_mod_id, "%s", w->mod.c_str());
        }
    }
    std::memcpy(out, &s, size);
    return KILN_OK;
}

KilnResult KILN_CALL t_rule_changes(uint64_t after, KilnToolWearRuleChange* out, uint32_t capacity, uint32_t stride, uint32_t* count,
                                    uint64_t* lost) {
    constexpr size_t kMinStride = offsetof(KilnToolWearRuleChange, before_mod_id);
    if (!count || (capacity && (!out || stride < kMinStride))) return KILN_ERR_INVALID_ARGUMENT;
    *count = 0;
    if (lost) *lost = 0;
    const size_t record = std::min<size_t>(stride, sizeof(KilnToolWearRuleChange));
    std::lock_guard lock(g_mu);
    const uint64_t newest = g_generation;
    const uint64_t oldest = newest > kRuleHistory ? newest - kRuleHistory + 1 : 1;  // the oldest generation held
    uint64_t next = after + 1;
    if (after >= newest) return KILN_OK;
    if (next < oldest) {
        if (lost) *lost = oldest - next;
        next = oldest;
    }
    for (; next <= newest && *count < capacity; ++next) {
        KilnToolWearRuleChange r = g_history[(next - 1) % kRuleHistory];
        r.size = static_cast<uint32_t>(record);
        std::memcpy(reinterpret_cast<uint8_t*>(out) + static_cast<size_t>(*count) * stride, &r, record);
        ++*count;
    }
    return KILN_OK;
}

// An owner went (a mod unloaded): its rules go with it, each item falling back to the rule before or the game's own.
void KILN_CALL on_owner_retiring(KilnOwner retiring, void*) {
    std::lock_guard lock(g_mu);
    g_retired.insert(retiring);
    g_mod_ids.erase(retiring);
    remove_locked([retiring](const Layer& l) { return l.owner == retiring; }, KILN_TOOLWEAR_RULE_RETIRED);
}

const KilnToolWearApi kTable = {{sizeof(KilnToolWearApi), KILN_TOOLWEAR_VERSION}, &t_define, &t_remove, &t_changes, &t_status, &t_rule_changes};

}  // namespace

bool publish(const KilnApi* api, uint64_t owner) {
    g_api = api;
    // rules of an owner that is revoked go by themselves where kiln.hooks can say so (0.2.0-dev.28); the managed side
    // removes a retiring mod's rules itself as well
    const auto* hooks = static_cast<const KilnHooksApi*>(api->service(KilnStr{KILN_HOOKS_SERVICE, std::strlen(KILN_HOOKS_SERVICE)}, 2));
    if (hooks && hooks->header.size >= offsetof(KilnHooksApi, subscribe_owner_retiring) + sizeof(void*) && hooks->subscribe_owner_retiring) {
        KilnHandle h = 0;
        hooks->subscribe_owner_retiring(owner, &on_owner_retiring, nullptr, &h);
    }
    return api->register_service(owner, KilnStr{KILN_TOOLWEAR_SERVICE, std::strlen(KILN_TOOLWEAR_SERVICE)}, &kTable) == KILN_OK;
}

}  // namespace kitems::toolwear
