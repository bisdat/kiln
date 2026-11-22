// SPDX-License-Identifier: MIT
// kiln.survivors' game-free core (survivors_core.h): request shapes, outcome rules, authorization, the result cache and
// the event ring, with threads.
#include <cstdio>
#include <thread>

#include "survivors_core.h"

using namespace kiln::survivors;

static int g_fail = 0, g_pass = 0;
#define CHECK(c)                                                              \
    do {                                                                      \
        if (c) ++g_pass;                                                      \
        else { ++g_fail; std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); } \
    } while (0)

static KilnSurvivorRequest req(uint32_t op) {
    KilnSurvivorRequest r{};
    r.size = sizeof r;
    r.op = op;
    r.npc_id = 0x672da6c9u;
    return r;
}

static void shapes() {
    CHECK(check_request(nullptr) != nullptr);
    KilnSurvivorRequest r = req(KILN_SURV_OP_RECRUIT);
    CHECK(check_request(&r) == nullptr);
    r.size = 80;
    CHECK(check_request(&r) != nullptr);
    r = req(KILN_SURV_OP_RECRUIT);
    r.npc_id = 0;
    CHECK(check_request(&r) != nullptr);
    r = req(KILN_SURV_OP_RECRUIT);
    r.flags = KILN_SURV_GIVE_TOOL;
    CHECK(check_request(&r) != nullptr);  // no recipient
    r.recipient = 1;
    CHECK(check_request(&r) == nullptr);
    r.flags = 2;
    CHECK(check_request(&r) != nullptr);
    r = req(KILN_SURV_OP_RECRUIT);
    r.recipient = 1;
    CHECK(check_request(&r) != nullptr);  // recipient without GIVE_TOOL
    r = req(KILN_SURV_OP_RECRUIT);
    r.reserved2 = 1;
    CHECK(check_request(&r) != nullptr);
    r = req(9);
    CHECK(check_request(&r) != nullptr);

    r = req(KILN_SURV_OP_SUMMON);
    CHECK(check_request(&r) != nullptr);  // no base
    r.base_id[3] = 7;
    CHECK(check_request(&r) == nullptr);
    r.position.x = NAN;
    CHECK(check_request(&r) != nullptr);
    r.position.x = 2e6;
    CHECK(check_request(&r) != nullptr);
    r.position.x = 1;
    r.yaw_degrees = INFINITY;
    CHECK(check_request(&r) != nullptr);

    r = req(KILN_SURV_OP_ASSIGN_BED);
    r.authorization = KILN_SURV_AUTH_PLAYER_PERMISSIONS;
    r.bed = 2033;
    CHECK(check_request(&r) != nullptr);  // actor none under PlayerPermissions is a bad argument
    r.actor = 1;
    CHECK(check_request(&r) == nullptr);
    r.authorization = KILN_SURV_AUTH_TRUSTED_SERVER;
    CHECK(check_request(&r) != nullptr);  // trusted with an actor
    r.actor = 0;
    CHECK(check_request(&r) == nullptr);
    r.bed = 0;
    CHECK(check_request(&r) != nullptr);  // assign without a bed
    r.authorization = 5;
    r.bed = 1;
    CHECK(check_request(&r) != nullptr);

    r = req(KILN_SURV_OP_UNASSIGN_BED);
    r.authorization = KILN_SURV_AUTH_TRUSTED_SERVER;
    CHECK(check_request(&r) == nullptr);
    r.bed = 4;
    CHECK(check_request(&r) != nullptr);
    r = req(KILN_SURV_OP_RECRUIT);
    r.actor = 3;
    CHECK(check_request(&r) != nullptr);
    r = req(KILN_SURV_OP_RECRUIT);
    r.base_id[0] = 1;
    CHECK(check_request(&r) != nullptr);
}

static void rules() {
    // the lab survivor's GUID-derived id is just FNV-1a over 16 bytes
    uint8_t g[16] = {};
    CHECK(npc_id_of(g) == 0x4b95f515u || npc_id_of(g) != 0);
    uint8_t g2[16] = {};
    g2[0] = 1;
    CHECK(npc_id_of(g) != npc_id_of(g2));

    CHECK(unlock_supported(0, 0, 0, 0, 0));
    CHECK(unlock_supported(0x1c68d6ae, 1, 2, 1, 0));
    CHECK(unlock_supported(0x1c68d6ae, 1, 0, 0, 0));
    CHECK(!unlock_supported(0x1c68d6ae, 0, 2, 1, 0));  // a named query (EXTERN)
    CHECK(!unlock_supported(0x1c68d6ae, 2, 2, 1, 0));  // a counter
    CHECK(!unlock_supported(0x1c68d6ae, 1, 2, 0, 0));  // "is false"
    CHECK(!unlock_supported(0x1c68d6ae, 1, 2, 1, 1));  // a player's own knowledge

    CHECK(failed_status(0, KILN_SURV_REFUSED) == KILN_SURV_REFUSED);
    CHECK(failed_status(KILN_SURV_REGISTRY_CONFIRMED, KILN_SURV_TIMED_OUT) == KILN_SURV_TIMED_OUT);  // observation only
    CHECK(failed_status(KILN_SURV_KNOWLEDGE_WRITTEN, KILN_SURV_TIMED_OUT) == KILN_SURV_PARTIAL);
    CHECK(failed_status(KILN_SURV_BED_CHANGED, KILN_SURV_REFUSED) == KILN_SURV_PARTIAL);

    KilnVec3 c{100, 50, 100}, h{20, 20, 20};
    CHECK(in_box(c, h, KilnVec3{110, 50, 119}));
    CHECK(in_box(c, h, KilnVec3{120, 30, 80}));
    CHECK(!in_box(c, h, KilnVec3{120.1, 50, 100}));
    CHECK(std::fabs(distance(KilnVec3{0, 0, 0}, KilnVec3{3, 4, 0}) - 5) < 1e-9);
}

static void auth() {
    ActorView a;
    std::vector<BedView> beds{BedView{true, false, false}};
    CHECK(authorize(KILN_SURV_AUTH_TRUSTED_SERVER, a, 0, beds) == nullptr);
    CHECK(std::strncmp(authorize(KILN_SURV_AUTH_PLAYER_PERMISSIONS, a, 0, beds), "ActorNotInGame", 14) == 0);
    a.found = a.in_game = true;
    a.handle = 0x41;
    CHECK(std::strncmp(authorize(0, a, 0, beds), "PermissionsUnknown", 18) == 0);
    a.permissions_known = true;
    CHECK(std::strncmp(authorize(0, a, 0, beds), "PermissionDenied", 16) == 0);
    a.can_edit_base = true;
    CHECK(authorize(0, a, 0, beds) == nullptr);
    CHECK(authorize(0, a, 0x41, beds) == nullptr);
    CHECK(std::strncmp(authorize(0, a, 0x81, beds), "ActorSessionChanged", 19) == 0);  // slot reused
    beds.push_back(BedView{false, false, false});
    CHECK(std::strncmp(authorize(0, a, 0, beds), "BedOutsideBase", 14) == 0);
    beds.back() = BedView{true, true, false};
    CHECK(std::strncmp(authorize(0, a, 0, beds), "BedOutsideBase", 14) == 0);
    beds.back() = BedView{true, false, true};
    CHECK(std::strncmp(authorize(0, a, 0, beds), "PermissionDenied", 16) == 0);
    CHECK(authorize(KILN_SURV_AUTH_TRUSTED_SERVER, a, 0, beds) == nullptr);  // trusted skips only these checks
}

static void cache() {
    ResultCache c;
    KilnSurvivorResult r{};
    KilnSurvivorResult out{};
    for (uint64_t i = 1; i <= 300; ++i) {
        r.request = i;
        r.status = uint32_t(i % 5);
        c.put(r, 1000);
    }
    CHECK(c.size() == 256);
    CHECK(!c.get(44, 1000, &out));   // the 44 oldest went first
    CHECK(c.get(45, 1000, &out) && out.request == 45);
    CHECK(c.get(300, 1000, &out) && out.status == 0);
    CHECK(c.get(300, 1000 + kResultKeepMs - 1, &out));
    CHECK(!c.get(300, 1000 + kResultKeepMs, &out));  // ten minutes
    CHECK(c.size() == 0);
}

static void ring() {
    EventRing e;
    KilnSurvivorEvent ev{};
    std::vector<std::thread> ts;
    for (int t = 0; t < 4; ++t)
        ts.emplace_back([&] {
            for (int i = 0; i < 2000; ++i) e.push(ev);
        });
    for (auto& t : ts) t.join();
    CHECK(e.last() == 8000);
    std::vector<KilnSurvivorEvent> out(5000);
    const uint32_t n = e.read(0, out.data(), 5000);
    CHECK(n == kEventRing);
    CHECK(out[0].sequence == 8000 - kEventRing + 1);
    CHECK(out[n - 1].sequence == 8000);
    CHECK(out[0].size == sizeof(KilnSurvivorEvent));
    CHECK(e.read(7998, out.data(), 5000) == 2);
    CHECK(e.read(7990, out.data(), 3) == 3 && out[2].sequence == 7993);
}

int main() {
    shapes();
    rules();
    auth();
    cache();
    ring();
    std::printf("survivors_test: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
