// SPDX-License-Identifier: MIT
// kiln.playerstate: the client actions held at the hold point, with nothing of the game or of Windows in it
// (playerstate.cpp gives it the game through ports; tests/held_inputs_test.cpp runs it on the host).
//
// How holding works (playerstate.cpp "holding client actions"): at the start of DeliverEvents, after the game copied
// each player's input into PlayerInput and before any program, a new version of a held action is hidden for one step
// (PlayerInput shows the consumed version, the bytes are kept here); the listener answers in the next Pre; at the next
// hold point a released action gets its bytes back and runs, a canceled one is marked consumed and never runs.
//
// Since 0.2.0-dev.29 a held action belongs to its holder (review P1 "held equipment can outlive its player/world"):
// the record keeps who held it (HeldOwner: the server world, its session, and the player's connection), and the hold
// point applies an answer only while the entity id still names that holder. A world that stops drops every held
// action, released version and lease of this process's last world (the next world starts empty); a holder that is gone
// or replaced (a disconnect, a rejoin, another player on the same entity id, another world at the same address) gets
// nothing applied: its record is dropped and counted, and the entity id's new owner is never written to.
#pragma once
#include <array>
#include <cstdint>
#include <cstring>
#include <vector>

#include "client_leases.h"

namespace kiln::playerstate {

constexpr uint32_t kHeldPayload = 160;  // KilnPlayerHeldAction::payload

// Who held an action. `session` counts the server worlds of the process (a new one at every start and stop), so a
// world at the address of the last one is another. `player`/`joined` are the player's connection as kiln.players
// knows it (its record handle and the step it joined); 0/0 for an entity without a player record (a spawned
// character), which is then the world's alone.
struct HeldOwner {
    uintptr_t world = 0;
    uint64_t session = 0;
    uint64_t player = 0, joined = 0;
    bool operator==(const HeldOwner& o) const {
        return world == o.world && session == o.session && player == o.player && joined == o.joined;
    }
};

// Where an action lives in PlayerInput / ServerConsumedPlayerInput (reflected names: playerstate.cpp).
struct ActionLayout {
    int64_t offset = -1, version = -1, consumed = -1;  // in PlayerInput, PlayerInput, ServerConsumedPlayerInput
    uint32_t bytes = 0;
};

class HeldInputs {
public:
    enum : uint8_t { Wait, Run, Drop };

    struct Held {
        uint32_t entity = 0, index = 0, version = 0;
        uint32_t shown = 0;  // the version PlayerInput's copy shows when no newer input comes: the consumed one
        uint8_t decision = Wait;
        bool listed = false;
        HeldOwner owner;
        std::array<uint8_t, kHeldPayload> bytes{};
    };
    struct Stats {
        uint64_t held = 0, ran = 0, dropped = 0;  // held new; released and run; canceled and consumed
        uint64_t foreign_owner = 0;               // dropped unapplied: their holder was gone or replaced
        uint64_t world_stopped = 0;               // dropped unapplied at a world's stop
    };

    // The hold point. P (the game) gives, for the server world of this hold point:
    //   uint8_t* input(uint32_t e)       e's PlayerInput, null when e has none
    //   uint8_t* consumed(uint32_t e)    e's ServerConsumedPlayerInput, null when none
    //   bool owner(uint32_t e, HeldOwner* out)   who e is now; false: e is no live holder now
    //   bool copy(void* dst, const void* src, size_t n)   a guarded copy (false: it faulted)
    // `holdable(index)` says whether an action can be held at all.
    template <class P, class Holdable>
    void hold_point(P& p, const std::vector<ActionLayout>& actions, ClientLeases& leases, Holdable holdable) {
        const bool holding = !leases.empty();
        std::vector<Held> keep;
        keep.reserve(held_.size());
        for (const Held& h : held_) {
            HeldOwner now;
            if (!p.owner(h.entity, &now) || !(now == h.owner)) {
                ++stats.foreign_owner;  // its holder is gone: nothing is applied to whoever has the id now
                continue;
            }
            // not offered to the listener yet (a step without Pre): it stays hidden
            if (h.decision == Wait && !h.listed && holding && leases.covers(h.entity, h.index)) keep.push_back(h);
            else resolve(p, actions, h, holding && leases.covers(h.entity, h.index), keep);
        }
        if (holding)
            for (const uint32_t e : leases.entities()) hold_new(p, actions, e, leases, holdable, keep);
        held_.swap(keep);
    }

    // The server world stops: every held action, released version and lease of it goes (none is applied).
    void world_stopped(ClientLeases& leases) {
        stats.world_stopped += held_.size();
        held_.clear();
        passed_.clear();
        leases.clear();
    }

    // A lease no longer names `entity`: its released versions are forgotten (client_action_hold / _interest).
    void forget_passed_unless(const ClientLeases& leases) {
        std::vector<Passed> keep;
        for (const Passed& x : passed_)
            if (leases.covers_entity(x.entity)) keep.push_back(x);
        passed_.swap(keep);
    }

    // client_action_held: the actions not offered yet, each once.
    template <class Emit>
    void list(Emit emit) {
        for (Held& h : held_) {
            if (h.listed) continue;
            if (!emit(static_cast<const Held&>(h))) return;  // no room: the rest next time
            h.listed = true;
        }
    }
    // client_action_decide / _cancel. False: no such held action.
    bool decide(uint32_t e, uint32_t index, uint32_t version, bool cancel) {
        for (Held& h : held_)
            if (h.entity == e && h.index == index && h.version == version) {
                h.decision = cancel ? Drop : Run;
                return true;
            }
        return false;
    }
    bool cancel_any(uint32_t e, uint32_t index) {
        for (Held& h : held_)
            if (h.entity == e && h.index == index) {
                h.decision = Drop;
                return true;
            }
        return false;
    }

    bool empty() const { return held_.empty(); }
    size_t size() const { return held_.size(); }
    const std::vector<Held>& held() const { return held_; }
    Stats stats;

private:
    struct Passed {
        uint32_t entity, index, version;
        HeldOwner owner;
    };

    bool passed(uint32_t e, uint32_t index, uint32_t version, const HeldOwner& owner) const {
        for (const Passed& x : passed_)
            if (x.entity == e && x.index == index) return x.version == version && x.owner == owner;
        return false;
    }
    void remember_passed(uint32_t e, uint32_t index, uint32_t version, const HeldOwner& owner) {
        for (Passed& x : passed_)
            if (x.entity == e && x.index == index) {
                x.version = version;
                x.owner = owner;
                return;
            }
        passed_.push_back({e, index, version, owner});
    }
    static bool held_in(uint32_t e, uint32_t index, const std::vector<Held>& in) {
        for (const Held& h : in)
            if (h.entity == e && h.index == index) return true;
        return false;
    }

    // One step after a hold: the listener's answer. A newer version that arrived meanwhile is appended to `keep`, held.
    template <class P>
    void resolve(P& p, const std::vector<ActionLayout>& actions, const Held& h, bool holding, std::vector<Held>& keep) {
        if (h.index >= actions.size()) return;
        const ActionLayout& a = actions[h.index];
        uint8_t* pi = p.input(h.entity);
        uint8_t* sc = p.consumed(h.entity);
        if (!pi || !sc) return;  // the player is gone
        const uint8_t d = h.decision == Wait ? static_cast<uint8_t>(Run) : h.decision;  // nobody answered: it runs
        Held next;
        uint32_t v = 0, c = 0;
        if (!p.copy(&v, pi + a.version, 4) || !p.copy(&c, sc + a.consumed, 4)) return;
        const bool newer = v != h.version && v != h.shown && v != c;  // the client sent another one of this kind meanwhile
        if (d == Run) {
            if (v != h.version) {
                // PlayerInput shows the hidden version (no input came since) or a newer one: put this one back so the
                // program runs it now; the newer one is held in turn and shows h.version until its own input returns.
                if (newer && holding) {
                    next.entity = h.entity;
                    next.index = h.index;
                    next.version = v;
                    next.shown = h.version;
                    next.owner = h.owner;
                    if (!p.copy(next.bytes.data(), pi + a.offset, a.bytes)) return;
                }
                if (!p.copy(pi + a.offset, h.bytes.data(), a.bytes)) return;
            }
            ++stats.ran;
            remember_passed(h.entity, h.index, h.version, h.owner);
            if (newer && holding) keep.push_back(next);
        } else {
            if (!p.copy(sc + a.consumed, &h.version, 4)) return;
            if (v == h.shown) p.copy(pi + a.version, &h.version, 4);  // the hidden copy shows its real, now consumed, version
            ++stats.dropped;
        }
    }

    // New versions of a player held for: hide each one for this step.
    template <class P, class Holdable>
    void hold_new(P& p, const std::vector<ActionLayout>& actions, uint32_t e, const ClientLeases& leases, Holdable holdable,
                  std::vector<Held>& keep) {
        HeldOwner owner;
        if (!p.owner(e, &owner)) return;  // no live holder of this id: nothing to hold for it
        uint8_t* pi = p.input(e);
        const uint8_t* sc = p.consumed(e);
        if (!pi || !sc) return;
        for (uint32_t i = 0; i < actions.size(); ++i) {
            const ActionLayout& a = actions[i];
            if (!holdable(i) || !leases.covers(e, i) || held_in(e, i, keep)) continue;
            uint32_t v = 0, c = 0;
            if (!p.copy(&v, pi + a.version, 4) || !p.copy(&c, sc + a.consumed, 4)) return;
            if (v == c || passed(e, i, v, owner)) continue;
            Held h;
            h.entity = e;
            h.index = i;
            h.version = v;
            h.shown = c;
            h.owner = owner;
            if (!p.copy(h.bytes.data(), pi + a.offset, a.bytes)) return;
            if (!p.copy(pi + a.version, &c, 4)) return;  // the game's program sees no new version in this step
            ++stats.held;
            keep.push_back(h);
        }
    }

    std::vector<Held> held_;     // the server world's thread only
    std::vector<Passed> passed_;  // released versions: one a program leaves pending for a while is not held again
};

}  // namespace kiln::playerstate
