// SPDX-License-Identifier: MIT
// Host test of world_gate.h: only a program context of the core's server world is the server world's, told apart by
// the storage the job names and its kind byte, never by an entity id (a hosting client's three worlds share ids).
#include <cstdio>

#include "fake_worlds.h"

static int g_pass = 0, g_fail = 0;
#define CHECK(x) ((x) ? ++g_pass : (++g_fail, std::printf("FAIL line %d: %s\n", __LINE__, #x)))

int main() {
    FakeWorlds w;
    const uintptr_t server = w.server_world();
    CHECK(w.gate.layout_known());
    CHECK(w.gate.owns(w.ctx_server, server));
    CHECK(!w.gate.owns(w.ctx_client, server));      // the client world: another storage
    CHECK(!w.gate.owns(w.ctx_prediction, server));  // the prediction world: storage inside another object, kind 2
    CHECK(!w.gate.owns(w.ctx_server, 0));           // a joined client (or the menu): no server world
    CHECK(!w.gate.owns(nullptr, server));
    // the client world's object published as "the server world" (it is not one): the storage matches, the kind does not
    CHECK(!w.gate.owns(w.ctx_client, reinterpret_cast<uintptr_t>(w.client)));
    // a job without a storage, a context without a job
    alignas(16) uint8_t empty_job[0x40] = {}, ctx_no_job[0x10] = {}, ctx_empty_job[0x10] = {};
    const uintptr_t j = reinterpret_cast<uintptr_t>(empty_job);
    std::memcpy(ctx_empty_job, &j, sizeof j);
    CHECK(!w.gate.owns(ctx_no_job, server) && !w.gate.owns(ctx_empty_job, server));
    // a server world that stopped and a new one started elsewhere: the old world's contexts are not the new one's
    alignas(16) uint8_t next[0x200] = {};
    next[FakeWorlds::kWorldStorage + FakeWorlds::kStorageKind] = 0;
    CHECK(!w.gate.owns(w.ctx_server, reinterpret_cast<uintptr_t>(next)));
    // layout unknown: nothing is the server world's
    kiln::decisions::WorldGate unknown;
    CHECK(!unknown.layout_known() && !unknown.owns(w.ctx_server, server));
    unknown.set_layout(FakeWorlds::kWorldStorage, -1);
    CHECK(!unknown.owns(w.ctx_server, server));
    std::printf("world_gate_test: %d PASS, %d FAIL\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
