// SPDX-License-Identifier: MIT
// EMF spawn adapters through kiln.spawn: the transform and components are supplied at entity creation, not changed
// afterwards (doors and targets need their original facing). Kept separate for production-path host tests.
#include <intrin.h>

#include <cmath>

#include "emf_internal.h"

namespace kemf {
namespace {
bool in_server_cb(EmfWorld* w) {
    return w && w == t_ctx.world && reinterpret_cast<uintptr_t>(w) == g_api->world(KILN_WORLD_SERVER) &&
           g_api->on_tick_thread();
}

uint32_t do_spawn(Mod* m, EmfWorld* w, const uint8_t guid[16], double x, double y, double z, const char* fn,
                  const KilnSpawnComponent* components = nullptr, uint32_t component_count = 0, float yaw_deg = 0) {
    if (!S.spawn) {
        unsupported(m, fn, "kiln.spawn is not available");
        return 0;
    }
    if (!in_server_cb(w) || !t_ctx.world_cb) {
        refuse(m, fn, "server world only, inside a world callback on the world passed to it");
        return 0;
    }
    if (!guid || !std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z) || !std::isfinite(yaw_deg))
        return refuse(m, fn, "bad arguments"), 0;
    KilnSpawnRequest req{};
    req.size = sizeof req;
    req.components = components;
    req.component_count = component_count;
    std::memcpy(req.template_guid.b, guid, 16);
    req.position = KilnVec3{x, y, z};
    req.yaw_degrees = yaw_deg;
    // EMF: spawned entities are not saved (except villagers); kiln.spawn's sidecar is not used for them
    req.flags = KILN_SPAWN_NO_TRACK;
    KilnTemplateInfo ti{};
    ti.size = sizeof ti;
    if (S.spawn->template_get(&req.template_guid, &ti) == KILN_OK && ti.kind != KILN_TEMPLATE_NPC)
        req.flags |= KILN_SPAWN_TRANSIENT;
    const std::string origin = "emf:" + std::string(mod_name(m));
    req.tag = kstr("emf");
    req.origin = kstr(origin);
    KilnEntity e = 0;
    const KilnResult rc = S.spawn->spawn(&req, &e);
    if (rc != KILN_OK) {
        refuse(m, fn, rc == KILN_ERR_NOT_FOUND ? "unknown template" : rc == KILN_ERR_UNAVAILABLE ? "the world is not running yet"
                                                                                                   : "the engine refused (kiln.log)");
        return 0;
    }
    return e;
}

uint32_t api_spawn(EmfWorld* w, const uint8_t guid[16], double x, double y, double z) {
    return do_spawn(caller_mod(_ReturnAddress()), w, guid, x, y, z, "spawn");
}

uint32_t spawn_with_components(Mod* m, EmfWorld* w, const uint8_t guid[16], double x, double y, double z,
                               const EmfComponentInit* comps, uint32_t count, float yaw_deg, const char* fn) {
    if (count == 0) return do_spawn(m, w, guid, x, y, z, fn, nullptr, 0, yaw_deg);
    // EMF's spawn_ex/spawn_facing limit is 15, independently of kiln.spawn's larger capacity.
    if (!comps || count > 15) return refuse(m, fn, "at most 15 extra components"), 0;
    if (!S.spawn || S.spawn->header.size < offsetof(KilnSpawnApi, capabilities) + sizeof(S.spawn->capabilities) ||
        !S.spawn->capabilities || !(S.spawn->capabilities() & KILN_SPAWN_CAP_COMPONENTS))
        return unsupported(m, fn, "kiln.spawn spawn-time components are unavailable"), 0;
    std::vector<KilnSpawnComponent> values;
    values.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        if (!comps[i].hash || !comps[i].data || !comps[i].size || comps[i].size > (1u << 20))
            return refuse(m, fn, "invalid component bytes"), 0;
        // CurrentTransform is supplied by x/y/z and yaw, never by the extra component list (EMF's rule).
        if (comps[i].hash == 0x027ac564u)
            return refuse(m, fn, "the CurrentTransform comes from x, y, z; do not pass it again"), 0;
        values.push_back({comps[i].hash, 0, comps[i].data, comps[i].size});
    }
    return do_spawn(m, w, guid, x, y, z, fn, values.data(), count, yaw_deg);
}

uint32_t api_spawn_ex(EmfWorld* w, const uint8_t guid[16], double x, double y, double z,
                      const EmfComponentInit* comps, uint32_t count) {
    return spawn_with_components(caller_mod(_ReturnAddress()), w, guid, x, y, z, comps, count, 0, "spawn_ex");
}

uint32_t api_spawn_facing(EmfWorld* w, const uint8_t guid[16], double x, double y, double z, float yaw_deg,
                          const EmfComponentInit* comps, uint32_t count) {
    return spawn_with_components(caller_mod(_ReturnAddress()), w, guid, x, y, z, comps, count, yaw_deg, "spawn_facing");
}

}  // namespace

uint32_t spawn_for(Mod* m, EmfWorld* w, const uint8_t guid[16], double x, double y, double z, const char* fn) {
    return do_spawn(m, w, guid, x, y, z, fn);
}

void fill_spawn(EmfApi* t) {
    t->spawn = &api_spawn;
    t->spawn_ex = &api_spawn_ex;
    t->spawn_facing = &api_spawn_facing;
}

}  // namespace kemf
