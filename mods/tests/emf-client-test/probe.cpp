// SPDX-License-Identifier: MIT
// emf_client_probe: an EMF mod for the guided check programs.client-emf (mods/tests/client-pass Dev28ClientWorldPrograms.cs)
// in a game client. It registers, all for EMF_WORLD_CLIENT (Kiln's client-world registration, since 0.2.0-dev.29): a
// program, a definition that reads HitEvent, and on_event for NotifyEnvFxEvent (the client program footdown_effects
// reads it: footsteps), HitEvent (seven client programs read it) and KilnClientPassNoSuchEvent (no world has it: the
// client world refuses its relay by name). Every 600 client steps it writes one line the check reads: how often the
// program and the definition ran, each event's deliveries, the payload's size and first bytes, how many deliveries came
// off the thread emf_mod_load ran on (the game's main thread) and how many in a step other than the one its relay copied
// it in. Built for labs and the client pass only (native/plugins/emf/CMakeLists.txt, emf-client-probe).
#include <windows.h>

#include <atomic>
#include <cstdio>
#include <cstring>

#include "emf/emf_api.h"

namespace {
const EmfApi* api;
DWORD g_main = 0;
std::atomic<long long> g_program{0}, g_definition{0}, g_steps{0};

struct Seen {
    const char* name;
    std::atomic<long long> n{0}, off_main{0};
    std::atomic<uint32_t> size{0};
    std::atomic<bool> first{false};
    unsigned char bytes[8]{};
};
Seen g_env{"NotifyEnvFxEvent"}, g_hit{"HitEvent"}, g_none{"KilnClientPassNoSuchEvent"};

void program(void*, void*) { ++g_program; }

void definition(void* ctx, void*) {
    ++g_definition;
    api->read_events(ctx, "HitEvent", 0, [](const void*, void*) {}, nullptr);
}

void on_event(EmfWorld*, const void* e, uint32_t size, void* user) {
    auto* s = static_cast<Seen*>(user);
    ++s->n;
    s->size = size;
    if (GetCurrentThreadId() != g_main) ++s->off_main;
    if (!s->first.exchange(true)) std::memcpy(s->bytes, e, size < 8 ? size : 8);
}

void line(const Seen& s, char* out, size_t cap) {
    std::snprintf(out, cap, "%s %lld (%u bytes, first %02x%02x%02x%02x%02x%02x%02x%02x, %lld off the main thread)", s.name, s.n.load(),
                  s.size.load(), s.bytes[0], s.bytes[1], s.bytes[2], s.bytes[3], s.bytes[4], s.bytes[5], s.bytes[6], s.bytes[7],
                  s.off_main.load());
}

void tick(EmfWorld* w, void*) {
    if (api->world_kind(w) != EMF_WORLD_CLIENT || ++g_steps % 600) return;
    char a[160], b[160], c[160];
    line(g_env, a, sizeof a);
    line(g_hit, b, sizeof b);
    line(g_none, c, sizeof c);
    api->log("emf_client_probe", "client world after %lld steps: program %lld, definition %lld; %s; %s; %s; on_tick %s the main thread",
             g_steps.load(), g_program.load(), g_definition.load(), a, b, c, GetCurrentThreadId() == g_main ? "on" : "off");
}
}  // namespace

extern "C" __declspec(dllexport) bool emf_mod_load(const EmfApi* table) {
    api = table;
    g_main = GetCurrentThreadId();  // EMF loads its mods in on_ready, on the game's main thread
    const bool p = api->register_program("emf_client_probe_sys", EMF_WORLD_CLIENT, &program, nullptr);
    const char* in[] = {"HitEvent", nullptr};
    EmfProgramDef d{};
    d.size = sizeof d;
    d.name = "emf_client_probe_def";
    d.world_kind = EMF_WORLD_CLIENT;
    d.fn = &definition;
    d.events_in = in;
    const bool q = api->register_program_def(&d);
    const bool e1 = api->on_event(EMF_WORLD_CLIENT, g_env.name, 0, &on_event, &g_env);
    const bool e2 = api->on_event(EMF_WORLD_CLIENT, g_hit.name, 0, &on_event, &g_hit);
    const bool e3 = api->on_event(EMF_WORLD_CLIENT, g_none.name, 0, &on_event, &g_none);  // the client world refuses its relay
    api->on_tick(&tick, nullptr);
    api->log("emf_client_probe", "loaded: register_program %d, register_program_def %d, on_event %d %d %d", p, q, e1, e2, e3);
    return true;
}
