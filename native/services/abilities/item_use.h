// SPDX-License-Identifier: MIT
// kiln.abilities item_use (since 0.2.0-dev.30.3): the decisions and layouts of an item used from a player's slot, kept
// apart from the game so the host tests run them (tests/item_use_test.cpp). abilities.cpp's t_item_use reads the game
// (the slot through kiln.items, Actor and ActorInput through GetComponent) and calls these.
//
// What the game does for a hand use (docs/re/player-state.md "Eating, drinking and other consumables", docs/re/abilities.md
// section 4): player_control_action_sequence calls TriggerSequence (0x369d0) with the item's sequence (ItemInfo.sequences[]
// with triggeredBy Primary) and puts the slot and the item into ActorInput.triggerContext.usedItem; actor_simulation_start
// takes the context in the same step. The sequence's consume event then reads Actor.currentAction.triggerContext.usedItem:
// actor_apply_buff removes one item from that slot in a transaction and applies the buff with the item (its impactValues and
// time), the spawn path (potions) does the same for SpawnImpact / SpawnEntityEvent. kiln.playerstate's lab_use_item has
// done exactly this on spawned player characters since 0.2.0-dev.27 (5 mushrooms to 4, the buff in the food slot).
// The cost item (since 0.2.0-dev.31.5, GitHub #118): the game's hand use of an item sequence with no costOverride writes
// the slot and the item as costItem as well as usedItem (0x36b60; with a costOverride 0x36ce0 writes the override as the
// cost), and PayUsageCost reads costItem (actor_pay_usage_cost 0x45360 -> 0x40d00): the costs from the item's own
// impactValues (a scroll's ManaCost) and, with consume, the item taken from costItem's slot (the repair kit's only
// consume). KILN_ITEM_USE_COST_ITEM asks for that write (docs/re/abilities.md "The cost item of an item use").
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string_view>

#include "services/kiln_abilities.h"
#include "services/kiln_types.h"

namespace kab::use {

// The states a hand use does not start from, besides a running sequence (keen::actor::StateFlag names; a name this build's
// reflection does not have is skipped). Kiln's own choice: the gate the game's input path checks (0xb1410) is not traced.
inline constexpr const char* kBusyStates[] = {"Spawning",           "Sleeping", "Sitting",        "PlayingInstrument",
                                              "Climbing",           "ClimbingTransition", "Attached", "AttachedToHook",
                                              "HangGliding",        "HeadDiving",         "IsFishing"};

// What item_use needs from this build's reflection (kiln.types), resolved once.
struct Layout {
    int64_t used_slot = -1;  // ActorInput.triggerContext.usedItem.slotId (InventorySlotId, 8 bytes)
    int64_t used_item = -1;  // ActorInput.triggerContext.usedItem.itemId (4 bytes)
    int64_t cost_slot = -1;  // ActorInput.triggerContext.costItem.slotId (8 bytes); -1: not in this build's reflection, so
    int64_t cost_item = -1;  // ...costItem.itemId (4 bytes); KILN_ITEM_USE_COST_ITEM is unavailable (the rest works)
    int64_t pending = -1;    // ActorInput.triggerContext.sequenceRuntimeId.sequenceId (4 bytes): a trigger waiting for the step
    int64_t context = -1;    // ActorInput.triggerContext, and its size: what actor_clear_input clears (abort())
    uint32_t context_size = 0;
    int64_t state = -1;      // Actor.currentState (8 bytes; BaseActor holds it)
    int64_t running = -1;    // Actor.sequenceRuntimeId.sequenceId (4 bytes): the sequence playing (0: none)
    int dead = -1, ready = -1;  // StateFlag.Dead, StateFlag.IsReadyForGameplay (bit numbers)
    uint64_t busy = 0;          // the kBusyStates bits
    int64_t primary = -1;       // ActionSequenceTrigger.Primary
    bool ok() const {
        return used_slot >= 0 && used_item >= 0 && pending >= 0 && context >= 0 && context_size > 0 && state >= 0 &&
               running >= 0 && dead >= 0 && ready >= 0 && primary >= 0;
    }
};

inline KilnStr kstr(const char* s) { return KilnStr{s, std::strlen(s)}; }
inline bool same(KilnStr a, std::string_view b) { return a.ptr && a.len == b.size() && std::memcmp(a.ptr, b.data(), b.size()) == 0; }

// A type with its typedefs taken off; null when it cannot be read.
inline const KilnType* strip(const KilnTypesApi* t, const KilnType* ty, KilnTypeInfo* info = nullptr) {
    for (int i = 0; ty && i < 8; ++i) {
        KilnTypeInfo ti{};
        ti.size = sizeof ti;
        if (t->type_info(ty, &ti) != KILN_OK) return nullptr;
        if (ti.kind != KILN_TYPE_TYPEDEF) {
            if (info) *info = ti;
            return ty;
        }
        ty = ti.inner;
    }
    return nullptr;
}

// A field of a struct, its base classes included (a base sits at offset 0 of the struct, as equip.cpp reads currentState).
inline bool find_field(const KilnTypesApi* t, const KilnType* st, std::string_view name, KilnTypeField* out) {
    for (int depth = 0; st && depth < 8; ++depth) {
        KilnTypeInfo ti{};
        st = strip(t, st, &ti);
        if (!st || ti.kind != KILN_TYPE_STRUCT) return false;
        for (uint32_t i = 0; i < ti.field_count; ++i) {
            KilnTypeField f{};
            if (t->type_field(st, i, &f) == KILN_OK && same(f.name, name)) {
                *out = f;
                return true;
            }
        }
        st = ti.inner;
    }
    return false;
}

// The offset of a dotted field path ("triggerContext.usedItem.slotId") in the named type, and the last field's size; -1
// when a part is missing.
inline int64_t field_path(const KilnTypesApi* t, const char* type, std::string_view path, uint32_t* size) {
    const KilnType* ty = t->type_by_name(kstr(type));
    if (!ty) return -1;
    int64_t off = 0;
    while (!path.empty()) {
        const size_t dot = path.find('.');
        const std::string_view part = path.substr(0, dot);
        KilnTypeField f{};
        if (!find_field(t, ty, part, &f)) return -1;
        off += static_cast<int64_t>(f.offset);
        ty = f.type;
        path = dot == std::string_view::npos ? std::string_view{} : path.substr(dot + 1);
    }
    KilnTypeInfo ti{};
    if (!strip(t, ty, &ti)) return -1;
    if (size) *size = ti.type_size;
    return off;
}

// A value of the named enum; -1 when the enum or the name is missing.
inline int64_t enum_value(const KilnTypesApi* t, const char* type, std::string_view name) {
    KilnTypeInfo ti{};
    const KilnType* ty = strip(t, t->type_by_name(kstr(type)), &ti);
    if (!ty) return -1;
    for (uint32_t i = 0; i < ti.field_count; ++i) {
        KilnStr n{};
        int64_t v = 0;
        if (t->type_enum(ty, i, &n, &v) == KILN_OK && same(n, name)) return v;
    }
    return -1;
}

// Fills *L from kiln.types. False with a reason when something item_use needs is not in this build's reflection.
inline bool resolve(const KilnTypesApi* t, Layout* L, char* why, size_t cap) {
    if (!t) return std::snprintf(why, cap, "kiln.types is not loaded"), false;
    uint32_t n = 0;
    L->used_slot = field_path(t, "keen::ecs::ActorInput", "triggerContext.usedItem.slotId", &n);
    if (L->used_slot < 0 || n != 8) return std::snprintf(why, cap, "ActorInput.triggerContext.usedItem.slotId is not in this build's reflection"), false;
    L->used_item = field_path(t, "keen::ecs::ActorInput", "triggerContext.usedItem.itemId", &n);
    if (L->used_item < 0 || n != 4) return std::snprintf(why, cap, "ActorInput.triggerContext.usedItem.itemId is not in this build's reflection"), false;
    // the cost item is optional: without it only KILN_ITEM_USE_COST_ITEM is unavailable
    uint32_t cs = 0, ci = 0;
    L->cost_slot = field_path(t, "keen::ecs::ActorInput", "triggerContext.costItem.slotId", &cs);
    L->cost_item = field_path(t, "keen::ecs::ActorInput", "triggerContext.costItem.itemId", &ci);
    if (L->cost_slot < 0 || cs != 8 || L->cost_item < 0 || ci != 4) L->cost_slot = L->cost_item = -1;
    L->pending = field_path(t, "keen::ecs::ActorInput", "triggerContext.sequenceRuntimeId.sequenceId", &n);
    if (L->pending < 0 || n != 4)
        return std::snprintf(why, cap, "ActorInput.triggerContext.sequenceRuntimeId.sequenceId is not in this build's reflection"), false;
    L->context = field_path(t, "keen::ecs::ActorInput", "triggerContext", &L->context_size);
    // the used item and the pending id must lie inside the context abort_trigger() clears
    const auto inside = [&](int64_t off, int64_t size) {
        return off >= L->context && off + size <= L->context + static_cast<int64_t>(L->context_size);
    };
    if (L->context < 0 || L->context_size == 0 || !inside(L->used_slot, 8) || !inside(L->used_item, 4) || !inside(L->pending, 4))
        return std::snprintf(why, cap, "ActorInput.triggerContext is not in this build's reflection"), false;
    if (L->cost_slot >= 0 && (!inside(L->cost_slot, 8) || !inside(L->cost_item, 4))) L->cost_slot = L->cost_item = -1;
    L->state = field_path(t, "keen::ecs::Actor", "currentState", &n);
    if (L->state < 0 || n != 8) return std::snprintf(why, cap, "Actor.currentState is not in this build's reflection"), false;
    L->running = field_path(t, "keen::ecs::Actor", "sequenceRuntimeId.sequenceId", &n);
    if (L->running < 0 || n != 4) return std::snprintf(why, cap, "Actor.sequenceRuntimeId.sequenceId is not in this build's reflection"), false;
    const int64_t dead = enum_value(t, "keen::actor::StateFlag", "Dead");
    const int64_t ready = enum_value(t, "keen::actor::StateFlag", "IsReadyForGameplay");
    if (dead < 0 || dead > 63 || ready < 0 || ready > 63)
        return std::snprintf(why, cap, "StateFlag.Dead or StateFlag.IsReadyForGameplay is not in this build's reflection"), false;
    L->dead = static_cast<int>(dead);
    L->ready = static_cast<int>(ready);
    L->busy = 0;
    for (const char* s : kBusyStates) {
        const int64_t b = enum_value(t, "keen::actor::StateFlag", s);
        if (b >= 0 && b < 64) L->busy |= 1ull << b;
    }
    L->primary = enum_value(t, "keen::actor::ActionSequenceTrigger", "Primary");
    if (L->primary < 0 || L->primary > 0xFFFF)
        return std::snprintf(why, cap, "ActionSequenceTrigger.Primary is not in this build's reflection"), false;
    return true;
}

// What the call found in the game.
struct Facts {
    bool own_slot = false;   // the slot is one of the entity's inventories (kiln.items read)
    bool player = false;     // those inventories are a player's (InventorySetup IsPlayer)
    uint32_t item = 0;       // the slot's stack now
    uint32_t count = 0;
    uint32_t expected = 0;   // KilnItemUse.item
    uint64_t state = 0;      // Actor.currentState
    uint32_t running = 0;    // Actor.sequenceRuntimeId.sequenceId (0: none)
    uint32_t pending = 0;    // ActorInput.triggerContext.sequenceRuntimeId.sequenceId (0: no trigger waits for the step)
};

// KILN_ITEM_USE_*: the slot first (the caller's mistakes), then the player (what changes from step to step).
inline uint32_t verdict(const Facts& f, const Layout& L) {
    if (!f.own_slot) return KILN_ITEM_USE_NOT_OWN_SLOT;
    if (f.item == 0 || f.count == 0) return KILN_ITEM_USE_EMPTY_SLOT;
    if (f.item != f.expected) return KILN_ITEM_USE_OTHER_ITEM;
    if (!f.player) return KILN_ITEM_USE_NOT_A_PLAYER;
    if (f.state & (1ull << L.dead)) return KILN_ITEM_USE_DEAD;
    if (!(f.state & (1ull << L.ready))) return KILN_ITEM_USE_NOT_READY;
    // a trigger already waiting (another item_use or sequence_play this step, or one the game queued after
    // actor_clear_input) would be replaced by this one: the first caller was told STARTED
    if (f.running != 0 || f.pending != 0 || (f.state & L.busy)) return KILN_ITEM_USE_BUSY;
    return KILN_ITEM_USE_STARTED;
}

// KilnItemUse's own checks (KILN_OK or KILN_ERR_INVALID_ARGUMENT); clears the outputs.
inline KilnResult check_request(KilnItemUse* u) {
    if (!u || u->size < sizeof(KilnItemUse)) return KILN_ERR_INVALID_ARGUMENT;
    u->status = KILN_ITEM_USE_STARTED;
    u->count = 0;
    u->trigger_time = 0;
    if (!u->entity || !u->slot || !u->item || !u->sequence || (u->flags & ~uint32_t{KILN_ITEM_USE_COST_ITEM}))
        return KILN_ERR_INVALID_ARGUMENT;
    return KILN_OK;
}

// The used item into a trigger context TriggerSequence has just written (it clears the context first, so the rest of
// usedItem, the pide, stays 0 as for a stack item), and with cost the same slot and item as costItem (the hand use's
// 0x36b60). The caller checked L.cost_slot for cost and guards the writes.
inline void write_used(uint8_t* actor_input, const Layout& L, uint64_t slot, uint32_t item, bool cost = false) {
    std::memcpy(actor_input + L.used_slot, &slot, sizeof slot);
    std::memcpy(actor_input + L.used_item, &item, sizeof item);
    if (cost && L.cost_slot >= 0) {
        std::memcpy(actor_input + L.cost_slot, &slot, sizeof slot);
        std::memcpy(actor_input + L.cost_item, &item, sizeof item);
    }
}

// Takes back a trigger that did not get its used item: the context cleared as actor_clear_input clears it, so
// actor_simulation_start finds nothing to start (a consuming sequence with no used item takes the server down). Only
// called when no trigger waited before the call (verdict), so nothing of anyone else's is cleared. The caller guards it.
inline void abort_trigger(uint8_t* actor_input, const Layout& L) {
    std::memset(actor_input + L.context, 0, L.context_size);
}

}  // namespace kab::use
