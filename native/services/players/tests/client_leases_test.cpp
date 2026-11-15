// SPDX-License-Identifier: MIT
// Host test of kiln.playerstate's client action leases (client_leases.h): tests/run.sh.
// The legacy wildcard lease holds every action of its players; a selective lease (client_action_interest) holds only
// its (player, action) pairs, so an equipment listener never delays an unrelated client action; leases end without a
// renewal; owners replace and remove their own lease only.
#include <cstdio>

#include "../client_leases.h"

using kiln::playerstate::ClientLeases;

static int g_pass = 0, g_fail = 0;
#define CHECK(x) ((x) ? ++g_pass : (++g_fail, std::printf("FAIL line %d: %s\n", __LINE__, #x)))

constexpr uint32_t kChangeEquipment = 7, kSwapSet = 8, kActionBar = 2, kCraft = 0;

int main() {
    // the legacy hold: every action of the listed players
    {
        ClientLeases l;
        CHECK(l.empty());
        const uint32_t players[] = {5, 3, 5, 0};
        l.set_wildcard(players, 4);
        CHECK(!l.empty() && !l.wildcard_empty());
        CHECK(l.covers(3, kActionBar) && l.covers(5, kChangeEquipment) && l.covers(5, kCraft));
        CHECK(!l.covers(4, kChangeEquipment));
        CHECK((l.entities() == std::vector<uint32_t>{3, 5}));  // sorted, once, no entity 0
        l.set_wildcard(nullptr, 0);
        CHECK(l.empty() && !l.covers(3, kActionBar));
    }
    // a selective lease: only its (player, action) pairs; unrelated actions of the same player are not held
    {
        ClientLeases l;
        const uint32_t players[] = {3};
        const uint32_t equip[] = {kSwapSet, kChangeEquipment};
        CHECK(l.set(11, players, 1, equip, 2));
        CHECK(l.covers(3, kChangeEquipment) && l.covers(3, kSwapSet));
        CHECK(!l.covers(3, kActionBar) && !l.covers(3, kCraft));  // no one-step delay for what nobody asked for
        CHECK(!l.covers(4, kChangeEquipment));
        CHECK(l.covers_entity(3) && !l.covers_entity(4) && l.wildcard_empty());
        // action index 0 is an action like any other
        const uint32_t zero[] = {0};
        CHECK(l.set(12, players, 1, zero, 1) && l.covers(3, kCraft) && !l.covers(3, kActionBar));
        // no index: every action of those players
        const uint32_t p9[] = {9};
        CHECK(l.set(13, p9, 1, nullptr, 0) && l.covers(9, kActionBar) && l.covers(9, kChangeEquipment));
        CHECK((l.entities() == std::vector<uint32_t>{3, 9}));
    }
    // owners: a set replaces the owner's lease, entity_count 0 removes it, another owner's stays
    {
        ClientLeases l;
        const uint32_t a[] = {3}, b[] = {4};
        const uint32_t equip[] = {kChangeEquipment};
        CHECK(l.set(1, a, 1, equip, 1) && l.set(2, b, 1, equip, 1));
        CHECK(l.set(1, b, 1, equip, 1));  // owner 1 now holds 4 only
        CHECK(!l.covers(3, kChangeEquipment) && l.covers(4, kChangeEquipment));
        CHECK(l.set(1, nullptr, 0, nullptr, 0));  // owner 1 gone; owner 2 still holds 4
        CHECK(l.covers(4, kChangeEquipment));
        CHECK(l.set(2, nullptr, 0, nullptr, 0) && l.empty());
        // the legacy lease and a selective one are independent
        l.set_wildcard(a, 1);
        CHECK(l.set(5, b, 1, equip, 1));
        l.set_wildcard(nullptr, 0);
        CHECK(l.covers(4, kChangeEquipment) && !l.covers(3, kActionBar));
    }
    // bad arguments change nothing
    {
        ClientLeases l;
        const uint32_t a[] = {3};
        const uint32_t equip[] = {kChangeEquipment};
        CHECK(!l.set(0, a, 1, equip, 1));          // no owner
        CHECK(!l.set(1, nullptr, 1, equip, 1));    // a count without ids
        CHECK(!l.set(1, a, 1, nullptr, 1));
        CHECK(!l.set(1, a, ClientLeases::kMaxEntities + 1, equip, 1));
        CHECK(!l.set(1, a, 1, equip, ClientLeases::kMaxActions + 1));
        CHECK(l.empty());
        for (uint64_t o = 1; o <= ClientLeases::kMaxOwners; ++o) CHECK(l.set(o, a, 1, equip, 1));
        CHECK(!l.set(ClientLeases::kMaxOwners + 1, a, 1, equip, 1));  // one owner too many
        CHECK(l.set(1, a, 1, equip, 1));                               // a known owner renews
    }
    // leases end without a renewal: `stale` hold points, then gone; a renewal starts the count again
    {
        ClientLeases l(3);
        const uint32_t a[] = {3}, b[] = {4};
        const uint32_t equip[] = {kChangeEquipment};
        l.set_wildcard(a, 1);
        CHECK(l.set(7, b, 1, equip, 1));
        CHECK(l.age() == 0 && l.age() == 0 && l.age() == 0);
        CHECK(l.covers(3, kActionBar) && l.covers(4, kChangeEquipment));
        CHECK(l.set(7, b, 1, equip, 1));  // renewed; the wildcard was not
        CHECK(l.age() == 1);              // the wildcard ends at the fourth hold point without a renewal
        CHECK(!l.covers(3, kActionBar) && l.covers(4, kChangeEquipment));
        CHECK(l.age() == 0 && l.age() == 0 && l.age() == 1 && l.empty());
    }
    std::printf("client_leases_test: %d PASS, %d FAIL\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
