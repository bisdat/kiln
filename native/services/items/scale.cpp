// SPDX-License-Identifier: MIT
// kiln.items v8 (since 0.2.0-dev.29.19, the equipment wave, package P7): per-instance power and armour scales
// (kiln_items.h "power and armour scales", docs/re/items.md section 20).
//
// Power (feature items.gear.power): items/ItemPower (server 0x1933b0, client 0x3f2110) fills one used-item entry of an
// impact that starts with ImpactItemConfig (a swing, a shot, a cast): it copies the ItemInstanceInfo (pide at +0x0c),
// looks the item up, reads its level and computes its power (0x91e9d0) into the entry (+0x24, captured). The impact's
// damage is then (weapon power + ammunition power) x distribution (0x192250). The detour calls the game's function and
// then multiplies the entry's power by the scale of its pide. Its fourth argument points at the job (JobStorage(job) is
// the storage the program runs on): only the server world's storage is scaled.
// Armour (feature items.gear.armor): apply_armor's per-slot function (items/ArmorSlot, server 0x1428f0, client 0x383350)
// adds one worn piece's armour (its type's armorSetup at its base level, then each unlocked perk's) into the wearer's nine
// armour values (r15). items/ArmorSlotBase (right before the base part; [rsp + captured] is the piece's pide, r14 the job
// pointer) records the values and the scale on this thread; items/ArmorSlotEnd (after the perk loop, where both paths
// join) multiplies what the piece added. The two run on one thread inside one call, in that order.
// Each hook does its own work under an SEH guard: a fault leaves the game's value as it is, counts, and switches that kind
// of scale off for the process. The hooks are installed at the first scale other than 1 of their kind, never removed.
#include "scale.h"

#include <windows.h>

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>

#include "engine.h"
#include "instances.h"
#include "kiln_hook.hpp"
#include "scale_logic.h"
#include "services/kiln_caps.h"
#include "services/kiln_hooks.h"
#include "services/kiln_types.h"

namespace kitems {
void items_log(int level, const char* fmt, ...);
uintptr_t items_server_world();

namespace {

#define LOGI(...) items_log(KILN_LOG_INFO, __VA_ARGS__)
#define LOGW(...) items_log(KILN_LOG_WARN, __VA_ARGS__)
#define LOGE(...) items_log(KILN_LOG_ERROR, __VA_ARGS__)

constexpr const char* kPowerFeature = "items.gear.power";
constexpr const char* kArmorFeature = "items.gear.armor";
constexpr uint32_t kArmorValues = 9;  // ArmorSetup.distribution: physical .. lightning (isSet at +0x24, checked)

KilnStr str(const char* s) { return KilnStr{s, std::strlen(s)}; }

const KilnApi* g_api = nullptr;
KilnOwner g_owner = 0;
std::mutex g_mu;  // the writer's side of the table, installs and the details
scale::Table g_table;

struct Kind {
    bool resolved = false;             // the feature's symbols and checks passed
    std::atomic<bool> hooked{false};   // its hooks are in
    std::atomic<bool> off{false};      // a fault or a failed install: off for the process
    char why[160] = "";
};
Kind g_power, g_armor;

uintptr_t g_power_fn = 0, g_armor_base = 0, g_armor_end = 0;
int64_t g_power_off = 0;     // the entry's power (captured: mov [rsi+d8], eax)
int64_t g_armor_pide = 0;    // the piece's pide on the slot function's stack (captured: mov r9d, [rsp+d8])
constexpr int64_t kEntryPide = 0x0C;  // the entry starts with the ItemInstanceInfo it copies; pide at +0x0c (checked)

kiln::RawHook h_power;
kiln::MidHook2 h_armor_base, h_armor_end;

std::atomic<uint64_t> g_powered{0}, g_armored{0}, g_faults{0};
uint64_t g_epoch = 0;  // the stamp table's epoch the entries were set under (g_mu)

template <typename T>
T rd(const void* p, int64_t off) {
    T v;
    std::memcpy(&v, static_cast<const uint8_t*>(p) + off, sizeof v);
    return v;
}

void set_why(Kind& k, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(k.why, sizeof k.why, fmt, ap);
    va_end(ap);
}

void lower(const char* feature, uint32_t reason, const char* why) {
    const auto* caps = static_cast<const KilnCapsApi*>(g_api ? g_api->service(str(KILN_CAPS_SERVICE), KILN_CAPS_VERSION) : nullptr);
    if (caps && caps->report) caps->report(g_owner, str(feature), KILN_CAP_UNAVAILABLE, reason, str(why));
}

void switch_off(Kind& k, const char* feature, const char* why) {
    if (k.off.exchange(true)) return;
    {
        std::lock_guard lock(g_mu);
        set_why(k, "%s", why);
    }
    LOGE("%s: off until the server, or the hosting player's game, restarts: %s", feature, why);
    lower(feature, KILN_CAP_REASON_FAULTED, why);
}

// The storage the job runs on is the server world's (never a game client's own world).
bool server_job(void* const* job_ref) {
    if (!job_ref || !E.job_storage || !E.world_storage) return false;
    void* job = *job_ref;
    if (!job) return false;
    const uintptr_t world = items_server_world();
    return world && reinterpret_cast<uintptr_t>(E.job_storage(job)) == world + static_cast<uintptr_t>(E.world_storage);
}

// ---- power ----
using PowerFn = uint64_t(__fastcall*)(uint8_t* entry, const uint8_t* info, void* ctx, void* const* job_ref);

bool scale_power(uint8_t* entry, void* const* job_ref) {
    __try {
        const int32_t power = rd<int32_t>(entry, g_power_off);
        if (power <= 0) return true;
        const float s = g_table.power(rd<uint32_t>(entry, kEntryPide));
        if (s == 1.0f || !server_job(job_ref)) return true;
        const int32_t scaled = scale::apply(power, s);
        std::memcpy(entry + g_power_off, &scaled, 4);
        g_powered.fetch_add(1, std::memory_order_relaxed);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

uint64_t __fastcall hk_power(uint8_t* entry, const uint8_t* info, void* ctx, void* const* job_ref) {
    const uint64_t rv = h_power.original<PowerFn>()(entry, info, ctx, job_ref);
    if (entry && g_table.live() != 0 && !g_power.off.load(std::memory_order_relaxed) && !scale_power(entry, job_ref)) {
        g_faults.fetch_add(1, std::memory_order_relaxed);
        switch_off(g_power, kPowerFeature, "a fault while scaling an item's power (the game's power stood)");
    }
    return rv;
}

// ---- armour ----
struct ArmorFrame {
    int32_t* values = nullptr;  // the wearer's nine armour values (the slot function's r15)
    int32_t before[kArmorValues] = {};
    float scale = 1.0f;
};
thread_local ArmorFrame t_armor;

bool armor_base(KilnRegisters& c) {
    __try {
        t_armor.values = nullptr;
        const uint32_t pide = rd<uint32_t>(reinterpret_cast<const void*>(c.rsp), g_armor_pide);
        const float s = g_table.armor(pide);
        if (s == 1.0f || !c.r15 || !server_job(reinterpret_cast<void* const*>(c.r14))) return true;
        auto* values = reinterpret_cast<int32_t*>(c.r15);
        std::memcpy(t_armor.before, values, sizeof t_armor.before);
        t_armor.scale = s;
        t_armor.values = values;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        t_armor.values = nullptr;
        return false;
    }
}

bool armor_end(KilnRegisters& c) {
    __try {
        int32_t* values = t_armor.values;
        t_armor.values = nullptr;
        if (!values || reinterpret_cast<uintptr_t>(values) != c.r15) return true;  // not this call's piece
        scale::apply_armor(values, t_armor.before, kArmorValues, t_armor.scale);
        g_armored.fetch_add(1, std::memory_order_relaxed);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void hk_armor_base(KilnRegisters& c) {
    if (g_table.live() == 0 || g_armor.off.load(std::memory_order_relaxed)) {
        t_armor.values = nullptr;
        return;
    }
    if (!armor_base(c)) {
        g_faults.fetch_add(1, std::memory_order_relaxed);
        switch_off(g_armor, kArmorFeature, "a fault while reading an armour piece (the game's armour stood)");
    }
}

void hk_armor_end(KilnRegisters& c) {
    if (!t_armor.values) return;
    if (!armor_end(c)) {
        g_faults.fetch_add(1, std::memory_order_relaxed);
        switch_off(g_armor, kArmorFeature, "a fault while scaling an armour piece (the game's armour stood)");
    }
}

// ---- reflection: the layouts the hooks read ----
bool field(const KilnTypesApi* t, const KilnType* type, const char* name, uint32_t* offset, uint32_t* size) {
    KilnTypeInfo ti{};
    ti.size = sizeof ti;
    if (!type || t->type_info(type, &ti) != KILN_OK) return false;
    const size_t n = std::strlen(name);
    for (uint32_t i = 0; i < ti.field_count; ++i) {
        KilnTypeField f{};
        if (t->type_field(type, i, &f) != KILN_OK || f.name.len != n || std::memcmp(f.name.ptr, name, n) != 0) continue;
        KilnTypeInfo fi{};
        fi.size = sizeof fi;
        if (t->type_info(f.type, &fi) != KILN_OK) return false;
        *offset = static_cast<uint32_t>(f.offset);
        if (size) *size = fi.type_size;
        return true;
    }
    return false;
}

// ItemInstanceInfo {slotId, itemId, pide +0x0c, stateMask; 20 bytes}: what the power entry copies first.
bool power_layout(const KilnTypesApi* t, char* why, size_t cap) {
    uint32_t o = 0, s = 0;
    const KilnType* info = t->type_by_name(str("keen::ItemInstanceInfo"));
    KilnTypeInfo ti{};
    ti.size = sizeof ti;
    if (!info || t->type_info(info, &ti) != KILN_OK || ti.type_size != 20 || !field(t, info, "pide", &o, &s) ||
        o != kEntryPide || s != 4)
        return std::snprintf(why, cap, "this build's ItemInstanceInfo is not 20 bytes with its pide at +0x0c"), false;
    return true;
}

// ItemInfo.armorSetup at 0x6bc with isSet at +0x24: nine armour values before it, as the slot function adds them.
bool armor_layout(const KilnTypesApi* t, char* why, size_t cap) {
    uint32_t o = 0, s = 0, is_set = 0;
    const KilnType* info = t->type_by_name(str("keen::ItemInfo"));
    if (!field(t, info, "armorSetup", &o, &s) || o != 0x6BC)
        return std::snprintf(why, cap, "this build's ItemInfo.armorSetup is not at 0x6bc"), false;
    KilnTypeField f{};
    KilnTypeInfo ti{};
    ti.size = sizeof ti;
    const KilnType* setup = nullptr;
    if (t->type_info(info, &ti) == KILN_OK)
        for (uint32_t i = 0; i < ti.field_count && !setup; ++i)
            if (t->type_field(info, i, &f) == KILN_OK && f.name.len == 10 && std::memcmp(f.name.ptr, "armorSetup", 10) == 0) setup = f.type;
    if (!field(t, setup, "isSet", &is_set, nullptr) || is_set != 4 * kArmorValues)
        return std::snprintf(why, cap, "this build's ArmorSetup does not hold nine values before isSet"), false;
    return true;
}

bool held(const char* feature) {
    const auto* caps = static_cast<const KilnCapsApi*>(g_api->service(str(KILN_CAPS_SERVICE), KILN_CAPS_VERSION));
    return caps && kiln_caps_held(caps, str(feature));
}

// g_mu held. Installs a kind's hooks once; false + why when it cannot.
bool ensure_hooked(Kind& k, bool power) {
    if (k.hooked.load()) return true;
    if (!k.resolved || k.off.load()) return false;
    const char* feature = power ? kPowerFeature : kArmorFeature;
    if (held(feature)) {
        set_why(k, "%s is held until its guided client check passes (kiln.json preview lifts it)", feature);
        return false;
    }
    bool ok;
    if (power) {
        ok = h_power.install(g_api, g_owner, g_power_fn, reinterpret_cast<void*>(&hk_power), "items/ItemPower");
        if (!ok) set_why(k, "items/ItemPower could not be hooked: %s", h_power.error() ? h_power.error() : "?");
    } else {
        ok = h_armor_end.install(g_api, g_owner, g_armor_end, &hk_armor_end, "items/ArmorSlotEnd") &&
             h_armor_base.install(g_api, g_owner, g_armor_base, &hk_armor_base, "items/ArmorSlotBase");
        if (!ok) {
            set_why(k, "the armour mid hooks could not be installed: %s",
                    h_armor_end.error() ? h_armor_end.error() : h_armor_base.error() ? h_armor_base.error() : "?");
            h_armor_end.revoke();  // a lone end hook does nothing (no frame is ever recorded), but take it out
        }
    }
    if (!ok) {
        k.off.store(true);
        LOGE("%s: %s", feature, k.why);
        lower(feature, KILN_CAP_REASON_FAULTED, k.why);
        return false;
    }
    k.hooked.store(true);
    k.why[0] = 0;
    LOGI("%s: hooks installed (the first scale was set)", feature);
    return true;
}

}  // namespace

void scale_resolve(const KilnApi* api, KilnOwner owner) {
    g_api = api;
    g_owner = owner;
    auto sym = [api](const char* n, uintptr_t* out) { return api->symbol(str(n), out) == KILN_OK && *out; };
    auto cst = [api](const char* n, int64_t* out) { return api->constant(str(n), out) == KILN_OK; };
    const auto* t = static_cast<const KilnTypesApi*>(api->service(str(KILN_TYPES_SERVICE), 1));
    const bool types = t && t->header.size >= sizeof(KilnTypesApi);
    char why[160];
    std::lock_guard lock(g_mu);
    const bool world = E.job_storage && E.world_storage > 0 && E.instances_ok;
    if (!api->feature(str(kPowerFeature))) {
        set_why(g_power, "the sigdb feature items.gear.power is off on this build");
    } else if (!world || !sym("items/ItemPower", &g_power_fn) || !cst("items/ItemPower.power", &g_power_off) ||
               g_power_off <= kEntryPide || g_power_off > 0x7C) {
        set_why(g_power, "items.gear.power needs item instances, the job's storage and the entry's power offset");
        lower(kPowerFeature, KILN_CAP_REASON_SYMBOL_MISSING, g_power.why);
    } else if (!types || !power_layout(t, why, sizeof why)) {
        set_why(g_power, "%s", types ? why : "kiln.types is not loaded");
        lower(kPowerFeature, KILN_CAP_REASON_SYMBOL_MISSING, g_power.why);
    } else {
        g_power.resolved = true;
    }
    if (!api->feature(str(kArmorFeature))) {
        set_why(g_armor, "the sigdb feature items.gear.armor is off on this build");
    } else if (!world || !sym("items/ArmorSlotBase", &g_armor_base) || !sym("items/ArmorSlotEnd", &g_armor_end) ||
               !cst("items/ArmorSlot.pide", &g_armor_pide) || g_armor_pide <= 0 || g_armor_pide > 0x88) {
        set_why(g_armor, "items.gear.armor needs item instances, the job's storage and the piece's pide offset");
        lower(kArmorFeature, KILN_CAP_REASON_SYMBOL_MISSING, g_armor.why);
    } else if (!types || !armor_layout(t, why, sizeof why)) {
        set_why(g_armor, "%s", types ? why : "kiln.types is not loaded");
        lower(kArmorFeature, KILN_CAP_REASON_SYMBOL_MISSING, g_armor.why);
    } else {
        g_armor.resolved = true;
    }
    LOGI("per-instance power (gear_scale): %s; armour: %s", g_power.resolved ? "on (hooked at the first scale)" : g_power.why,
         g_armor.resolved ? "on (hooked at the first scale)" : g_armor.why);
}

void scale_world_reset() {
    std::lock_guard lock(g_mu);
    if (g_table.live() || g_table.used()) g_table.clear();
    g_epoch = instances_epoch();
}

void scale_pre() {
    if (g_table.used() == 0) return;  // the writer's count: read on the tick thread, the only writer
    std::lock_guard lock(g_mu);
    const uint64_t now = instances_epoch();
    if (now != g_epoch) {
        g_table.clear();
        g_epoch = now;
        LOGI("per-instance scales: the item stamps were forgotten (the entity id counter went down): the scales too");
    }
}

KilnResult scale_set(const KilnItemsGearScale* s) {
    if (!s || !scale::request_valid(*s)) return KILN_ERR_INVALID_ARGUMENT;
    std::lock_guard lock(g_mu);
    const bool power = (s->flags & KILN_ITEMS_SCALE_POWER) != 0, armor = (s->flags & KILN_ITEMS_SCALE_ARMOR) != 0;
    if ((power && !g_power.resolved) || (armor && !g_armor.resolved) || (power && g_power.off.load()) || (armor && g_armor.off.load()))
        return KILN_ERR_UNAVAILABLE;
    if (!instances_on() || instances_peek(s->entity) != s->stamp) return KILN_ERR_NOT_FOUND;
    if (g_table.used() == 0) g_epoch = instances_epoch();
    // a scale other than 1 needs its hooks; setting 1 (removing) never installs anything
    if (power && s->power != 1.0f && !ensure_hooked(g_power, true)) return KILN_ERR_UNAVAILABLE;
    if (armor && s->armor != 1.0f && !ensure_hooked(g_armor, false)) return KILN_ERR_UNAVAILABLE;
    return g_table.set(s->entity, s->flags, s->power, s->armor) ? KILN_OK : KILN_ERR_BUFFER_TOO_SMALL;
}

KilnResult scale_read(KilnEntity entity, uint32_t stamp, KilnItemsGearScale* out) {
    if (!out || entity == 0) return KILN_ERR_INVALID_ARGUMENT;
    *out = KilnItemsGearScale{};
    out->size = sizeof *out;
    if (!instances_on()) return KILN_ERR_UNAVAILABLE;
    const uint32_t have = instances_peek(entity);
    if (have == 0 || (stamp != 0 && have != stamp)) return KILN_ERR_NOT_FOUND;
    out->entity = entity;
    out->stamp = have;
    out->power = g_table.power(entity);
    out->armor = g_table.armor(entity);
    if (out->power != 1.0f) out->flags |= KILN_ITEMS_SCALE_POWER;
    if (out->armor != 1.0f) out->flags |= KILN_ITEMS_SCALE_ARMOR;
    return KILN_OK;
}

KilnResult scale_status(KilnItemsScaleStatus* out) {
    if (!out || out->size < sizeof(KilnItemsScaleStatus)) return KILN_ERR_INVALID_ARGUMENT;
    const uint32_t size = out->size;
    *out = KilnItemsScaleStatus{};
    out->size = size;
    std::lock_guard lock(g_mu);
    out->power = g_power.resolved && !g_power.off.load() ? 1 : 0;
    out->armor = g_armor.resolved && !g_armor.off.load() ? 1 : 0;
    out->power_hooked = g_power.hooked.load() ? 1 : 0;
    out->armor_hooked = g_armor.hooked.load() ? 1 : 0;
    out->entries = g_table.live();
    out->capacity = scale::kCapacity;
    out->powered = g_powered.load(std::memory_order_relaxed);
    out->armored = g_armored.load(std::memory_order_relaxed);
    out->faults = g_faults.load(std::memory_order_relaxed);
    if (!out->power || !out->armor)
        std::snprintf(out->detail, sizeof out->detail, "%s%s%s", out->power ? "" : g_power.why,
                      (!out->power && !out->armor) ? "; " : "", out->armor ? "" : g_armor.why);
    else if (g_power.why[0] || g_armor.why[0])
        std::snprintf(out->detail, sizeof out->detail, "%s%s", g_power.why, g_armor.why);
    return KILN_OK;
}

}  // namespace kitems
