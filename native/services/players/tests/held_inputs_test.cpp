// SPDX-License-Identifier: MIT
// Host test of kiln.playerstate's hold point (held_inputs.h) at the adapter level, on made-up PlayerInput and
// ServerConsumedPlayerInput memory: an equipment input held for one player is never applied to another (review P1
// "held equipment can outlive its player/world", 2026-11-14). Hold an input, then: the player disconnects and another
// takes the same entity id; the world is replaced by another at the same address; the item leaves the inventory while
// the input is held. With -DKILN_HELD_BEFORE the ports know no holder (as playerstate.cpp before the fix: the entity id
// alone) and the world's stop clears nothing, which is the red run.
#include <cstdio>
#include <map>

#include "../held_inputs.h"

using namespace kiln::playerstate;

static int g_pass = 0, g_fail = 0;
#define CHECK(x) ((x) ? ++g_pass : (++g_fail, std::printf("FAIL line %d: %s\n", __LINE__, #x)))

// changeEquipmentAction-like: 16 bytes at PlayerInput +0x20, its version at +0x28 (payload +8), consumed at +0x4.
static const std::vector<ActionLayout> kActions = {ActionLayout{0x20, 0x28, 0x4, 16}};

struct Player {
    uint8_t input[0x80] = {}, consumed[0x10] = {};
    HeldOwner who;
    void send(uint32_t version, uint8_t slot) {  // the client's input as the game copied it into PlayerInput
        std::memset(input + 0x20, 0, 16);
        input[0x20] = slot;  // source inventory slot
        std::memcpy(input + 0x28, &version, 4);
    }
    uint32_t version() const { uint32_t v; std::memcpy(&v, input + 0x28, 4); return v; }
    uint32_t consumed_version() const { uint32_t v; std::memcpy(&v, consumed + 0x4, 4); return v; }
    void consume(uint32_t v) { std::memcpy(consumed + 0x4, &v, 4); }
};

struct World {
    uintptr_t address = 0x10000;
    uint64_t session = 1;
    std::map<uint32_t, Player> players;
    int writes = 0;  // copies into game memory
    uint8_t* input(uint32_t e) { return players.count(e) ? players[e].input : nullptr; }
    uint8_t* consumed(uint32_t e) { return players.count(e) ? players[e].consumed : nullptr; }
    bool owner(uint32_t e, HeldOwner* out) {
        if (!players.count(e)) return false;
#ifdef KILN_HELD_BEFORE
        *out = HeldOwner{};  // before the fix: no holder was kept, an entity id was its own identity
#else
        *out = players[e].who;
#endif
        return true;
    }
    bool copy(void* dst, const void* src, size_t n) {
        for (auto& [e, p] : players)
            if ((dst >= p.input && dst < p.input + sizeof p.input) || (dst >= p.consumed && dst < p.consumed + sizeof p.consumed)) ++writes;
        std::memcpy(dst, src, n);
        return true;
    }
    Player& join(uint32_t e, uint64_t handle, uint64_t joined) {
        Player& p = players[e];
        p = Player{};
        p.who = HeldOwner{address, session, handle, joined};
        p.consume(6);
        p.send(6, 0);
        return p;
    }
};

static void world_stopped(HeldInputs& h, ClientLeases& leases) {
#ifdef KILN_HELD_BEFORE
    (void)h, (void)leases;  // before the fix: nothing was cleared
#else
    h.world_stopped(leases);
#endif
}

static uint32_t lease(ClientLeases& l, uint32_t e) {
    const uint32_t actions[] = {0};
    l.set(77, &e, 1, actions, 1);
    return e;
}

int main() {
    auto holdable = [](uint32_t) { return true; };
    // ---- a disconnect: another player takes entity 5, and the old player's allowed input must not run for them
    {
        World w;
        ClientLeases leases;
        HeldInputs h;
        Player& a = w.join(5, 100, 10);
        lease(leases, 5);
        a.send(7, 3);  // A asks to equip from slot 3
        h.hold_point(w, kActions, leases, holdable);
        CHECK(h.size() == 1 && a.version() == 6);  // hidden: the program sees the consumed version
        h.list([](const HeldInputs::Held&) { return true; });
        CHECK(h.decide(5, 0, 7, false));  // the listener allows it in Pre
        // A leaves; B joins as entity 5 before the next hold point
        Player& b = w.join(5, 101, 50);
        b.send(6, 9);
        const int writes = w.writes;
        lease(leases, 5);
        h.hold_point(w, kActions, leases, holdable);
        CHECK(w.writes == writes);                         // nothing written into B's input or consumed version
        CHECK(b.version() == 6 && b.input[0x20] == 9 && b.consumed_version() == 6);
        CHECK(h.size() == 0 && h.stats.foreign_owner == 1 && h.stats.ran == 0);
    }
    // ---- a cancel is not applied to the new player either (it would consume their version)
    {
        World w;
        ClientLeases leases;
        HeldInputs h;
        Player& a = w.join(5, 100, 10);
        lease(leases, 5);
        a.send(7, 3);
        h.hold_point(w, kActions, leases, holdable);
        CHECK(h.decide(5, 0, 7, true));
        Player& b = w.join(5, 101, 50);
        b.send(7, 4);  // B's own first input happens to carry version 7
        lease(leases, 5);
        h.hold_point(w, kActions, leases, holdable);
        CHECK(b.consumed_version() == 6 && h.stats.dropped == 0);  // B's version 7 is not marked consumed by A's cancel
        CHECK(h.size() == 1 && h.held()[0].owner.player == 101 && h.held()[0].version == 7);  // B's own input, held for B
    }
    // ---- the world is replaced by another at the same address; a new player has entity 5 there
    {
        World w;
        ClientLeases leases;
        HeldInputs h;
        Player& a = w.join(5, 100, 10);
        lease(leases, 5);
        a.send(7, 3);
        h.hold_point(w, kActions, leases, holdable);
        CHECK(h.decide(5, 0, 7, false));
        world_stopped(h, leases);  // KILN_WORLD_STOPPING of the server world
        CHECK(h.size() == 0 && leases.empty() && h.stats.world_stopped == 1);
        w.session = 2;  // the next world, same address
        Player& c = w.join(5, 100, 10);  // even the same handle and joined step: another world
        c.send(6, 1);
        const int writes = w.writes;
        h.hold_point(w, kActions, leases, holdable);
        CHECK(w.writes == writes && c.version() == 6 && c.input[0x20] == 1 && c.consumed_version() == 6);
    }
    // ---- the item left the inventory while the input was held: the same player's input runs as the client sent it
    // (the game's own equip checks the slot again and equips nothing from an empty one); nothing else is written
    {
        World w;
        ClientLeases leases;
        HeldInputs h;
        Player& a = w.join(5, 100, 10);
        lease(leases, 5);
        a.send(7, 3);
        h.hold_point(w, kActions, leases, holdable);
        CHECK(h.decide(5, 0, 7, false));
        lease(leases, 5);
        h.hold_point(w, kActions, leases, holdable);  // (slot 3 emptied meanwhile: inventory, not PlayerInput)
        CHECK(a.version() == 7 && a.input[0x20] == 3 && h.stats.ran == 1 && h.size() == 0);
        // the same version is not held a second time (released)
        h.hold_point(w, kActions, leases, holdable);
        CHECK(h.size() == 0 && h.stats.held == 1);
    }
    std::printf("held_inputs_test: %d PASS, %d FAIL\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
