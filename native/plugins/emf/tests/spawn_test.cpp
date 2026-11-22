// SPDX-License-Identifier: MIT
// The production spawn adapter against kiln.spawn fakes; no game or Windows needed.
#include "emf_internal.h"
#include <cmath>
#include <cstdio>
#include <limits>

static int checks = 0, failures = 0;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; std::printf("FAIL %d: %s\n", __LINE__, #x); } } while (0)
namespace kemf {
const KilnApi* g_api;
Services S;
thread_local Context t_ctx;
static Mod mod;
Mod* caller_mod(const void*) { return &mod; }
const char* mod_name(const Mod* m) { return m ? m->id.c_str() : "?"; }
std::string last_function;
void unsupported(const Mod*, const char* fn, const char*) { last_function = fn; }
void refuse(const Mod*, const char* fn, const char*, bool) { last_function = fn; }
}  // namespace kemf
using namespace kemf;

static bool tick = true, component_cap = true;
static uint32_t kind = KILN_TEMPLATE_CREATURE;
static KilnResult result = KILN_OK;
static unsigned calls = 0;
static KilnSpawnRequest captured{};
static std::vector<KilnSpawnComponent> components;
static std::string origin, tag;
static uintptr_t KILN_CALL world(uint32_t side) { return side == KILN_WORLD_SERVER ? 0x1000 : 0x2000; }
static bool KILN_CALL on_tick() { return tick; }
static uint32_t KILN_CALL caps() { return component_cap ? KILN_SPAWN_CAP_COMPONENTS : 0; }
static KilnResult KILN_CALL template_get(const KilnGuid*, KilnTemplateInfo* out) { out->kind = kind; return KILN_OK; }
static KilnResult KILN_CALL spawn(const KilnSpawnRequest* req, KilnEntity* out) {
    ++calls;
    captured = *req;
    components.clear();
    if (req->component_count) components.assign(req->components, req->components + req->component_count);
    origin.assign(req->origin.ptr, req->origin.len);
    tag.assign(req->tag.ptr, req->tag.len);
    *out = result == KILN_OK ? 5000 : 0;
    return result;
}
int main() {
    KilnApi api{};
    api.world = &world;
    api.on_tick_thread = &on_tick;
    g_api = &api;
    mod.id = "spawn-test";
    KilnSpawnApi service{};
    service.header.size = sizeof service;
    service.spawn = &spawn;
    service.template_get = &template_get;
    service.capabilities = &caps;
    S.spawn = &service;
    auto* w = reinterpret_cast<EmfWorld*>(world(KILN_WORLD_SERVER));
    ContextScope scope(&mod, w, true);
    EmfApi t{};
    fill_spawn(&t);
    CHECK(t.spawn && t.spawn_ex && t.spawn_facing);
    if (!t.spawn_facing) return 1;
    uint8_t guid[16] = {1, 2, 3, 4};
    uint32_t bytes = 42;
    EmfComponentInit init{0x12345678, sizeof bytes, &bytes};
    // Yaw (including negative and multiple turns) reaches the spawn request, never a later teleport.
    for (float yaw : {0.f, 90.f, -45.f, 720.f}) {
        CHECK(t.spawn_facing(w, guid, 1.25, 2.5, -3.75, yaw, &init, 1) == 5000);
        CHECK(captured.yaw_degrees == yaw && captured.position.x == 1.25 && captured.position.y == 2.5 && captured.position.z == -3.75);
        CHECK(captured.size == sizeof captured && std::memcmp(captured.template_guid.b, guid, 16) == 0);
        CHECK(captured.flags == (KILN_SPAWN_NO_TRACK | KILN_SPAWN_TRANSIENT));
        CHECK(components.size() == 1 && components[0].hash == init.hash && components[0].data == init.data && components[0].size == init.size && components[0].flags == 0);
        CHECK(origin == "emf:spawn-test" && tag == "emf");
    }
    kind = KILN_TEMPLATE_NPC;
    CHECK(t.spawn_facing(w, guid, 1, 2, 3, 180, nullptr, 0) == 5000 && captured.flags == KILN_SPAWN_NO_TRACK);
    CHECK(captured.component_count == 0 && captured.yaw_degrees == 180);
    CHECK(t.spawn(w, guid, 1, 2, 3) == 5000 && captured.yaw_degrees == 0);
    CHECK(t.spawn_ex(w, guid, 1, 2, 3, &init, 1) == 5000 && captured.yaw_degrees == 0 && components.size() == 1);
    CHECK(spawn_for(&mod, w, guid, 1, 2, 3, "spawn_offer") == 5000 && captured.yaw_degrees == 0);
    // EMF permits 15 extra components, and never a replacement for its creation-time transform.
    EmfComponentInit many[16];
    for (uint32_t i = 0; i < 16; ++i) many[i] = {i + 1, sizeof bytes, &bytes};
    CHECK(t.spawn_ex(w, guid, 1, 2, 3, many, 15) == 5000 && captured.component_count == 15);
    CHECK(t.spawn_facing(w, guid, 1, 2, 3, 90, many, 15) == 5000 && captured.component_count == 15);
    const auto before = calls;
    CHECK(t.spawn_ex(w, guid, 1, 2, 3, many, 16) == 0);
    CHECK(t.spawn_facing(w, guid, 1, 2, 3, 90, many, 16) == 0);
    EmfComponentInit transform{0x027ac564, sizeof bytes, &bytes};
    CHECK(t.spawn_ex(w, guid, 1, 2, 3, &transform, 1) == 0);
    CHECK(t.spawn_facing(w, guid, 1, 2, 3, 90, &transform, 1) == 0);
    // A callback on a different world cannot borrow the current server world's pointer.
    for (auto* other : {reinterpret_cast<EmfWorld*>(0x2000), static_cast<EmfWorld*>(nullptr)}) {
        ContextScope wrong_world(&mod, other, true);
        CHECK(t.spawn(w, guid, 1, 2, 3) == 0);
        CHECK(t.spawn_ex(w, guid, 1, 2, 3, &init, 1) == 0);
        CHECK(t.spawn_facing(w, guid, 1, 2, 3, 90, &init, 1) == 0);
    }
    CHECK(calls == before);
    for (float yaw : {std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity(), -std::numeric_limits<float>::infinity()})
        CHECK(t.spawn_facing(w, guid, 1, 2, 3, yaw, nullptr, 0) == 0 && last_function == "spawn_facing");
    CHECK(t.spawn_facing(w, nullptr, 1, 2, 3, 0, nullptr, 0) == 0);
    CHECK(t.spawn_facing(w, guid, std::numeric_limits<double>::infinity(), 2, 3, 0, nullptr, 0) == 0);
    CHECK(t.spawn_facing(nullptr, guid, 1, 2, 3, 0, nullptr, 0) == 0);
    CHECK(t.spawn_facing(reinterpret_cast<EmfWorld*>(0x2000), guid, 1, 2, 3, 0, nullptr, 0) == 0);
    tick = false;
    CHECK(t.spawn_facing(w, guid, 1, 2, 3, 0, nullptr, 0) == 0);
    tick = true;
    t_ctx.world_cb = false;
    CHECK(t.spawn_facing(w, guid, 1, 2, 3, 0, nullptr, 0) == 0);
    t_ctx.world_cb = true;
    CHECK(t.spawn_facing(w, guid, 1, 2, 3, 0, nullptr, 1) == 0);
    CHECK(t.spawn_facing(w, guid, 1, 2, 3, 0, &init, 65) == 0);
    for (EmfComponentInit bad : {EmfComponentInit{0, 4, &bytes}, EmfComponentInit{1, 4, nullptr}, EmfComponentInit{1, 0, &bytes}, EmfComponentInit{1, (1u << 20) + 1, &bytes}})
        CHECK(t.spawn_facing(w, guid, 1, 2, 3, 0, &bad, 1) == 0);
    component_cap = false;
    CHECK(t.spawn_facing(w, guid, 1, 2, 3, 0, &init, 1) == 0);
    component_cap = true;
    service.header.size = offsetof(KilnSpawnApi, capabilities);
    CHECK(t.spawn_facing(w, guid, 1, 2, 3, 0, &init, 1) == 0);
    service.header.size = sizeof service;
    S.spawn = nullptr;
    CHECK(t.spawn_facing(w, guid, 1, 2, 3, 0, nullptr, 0) == 0);
    S.spawn = &service;
    CHECK(calls == before && last_function == "spawn_facing");
    for (KilnResult rc : {KILN_ERR_NOT_FOUND, KILN_ERR_UNAVAILABLE, KILN_ERR_INTERNAL}) {
        result = rc;
        CHECK(t.spawn_facing(w, guid, 1, 2, 3, 90, nullptr, 0) == 0 && last_function == "spawn_facing");
    }
    std::printf("spawn_test: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
