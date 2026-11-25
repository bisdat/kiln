// SPDX-License-Identifier: MIT
// Host tests of kiln.abilities' item_use (since 0.2.0-dev.30.3): item_use.h's layout resolution against a fake kiln.types
// (typedefs, base classes, nested paths, missing names, wrong sizes), its verdicts and request checks, and abilities.cpp's
// own t_item_use (extracted by run.sh) against a fake game: a fake kiln.items read, fake Actor / ActorInput components
// and a fake TriggerSequence that clears and writes the trigger context as the game's does (0x369d0).
#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "kiln_api.h"
#include "services/kiln_abilities.h"
#include "services/kiln_items.h"
#include "services/kiln_types.h"
#include "item_use.h"

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { ++g_fail; std::printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)

// ---------------------------------------------------------------------------------------------- fake kiln.types ----
struct FakeField { std::string name; const struct FakeType* type; uint64_t offset; };
struct FakeType {
    std::string qname;
    uint8_t kind;
    uint32_t size;
    const FakeType* inner = nullptr;  // struct: base; typedef: target
    std::vector<FakeField> fields;
    std::vector<std::pair<std::string, int64_t>> values;  // enums
};
static std::vector<FakeType*> g_types;
static FakeType* add_type(const char* q, uint8_t kind, uint32_t size, const FakeType* inner = nullptr) {
    auto* t = new FakeType{q, kind, size, inner, {}, {}};
    g_types.push_back(t);
    return t;
}
static const FakeType* T(const KilnType* t) { return reinterpret_cast<const FakeType*>(t); }
static const KilnType* K(const FakeType* t) { return reinterpret_cast<const KilnType*>(t); }

static const KilnType* KILN_CALL f_type_by_name(KilnStr n) {
    for (auto* t : g_types)
        if (t->qname.size() == n.len && std::memcmp(t->qname.data(), n.ptr, n.len) == 0) return K(t);
    return nullptr;
}
static KilnResult KILN_CALL f_type_info(const KilnType* type, KilnTypeInfo* out) {
    if (!type || !out || out->size < sizeof(KilnTypeInfo)) return KILN_ERR_INVALID_ARGUMENT;
    const FakeType* t = T(type);
    out->qualified_name = KilnStr{t->qname.c_str(), t->qname.size()};
    out->type_size = t->size;
    out->kind = t->kind;
    out->field_count = static_cast<uint32_t>(t->kind == KILN_TYPE_ENUM ? t->values.size() : t->fields.size());
    out->inner = K(t->inner);
    return KILN_OK;
}
static KilnResult KILN_CALL f_type_field(const KilnType* type, uint32_t i, KilnTypeField* out) {
    const FakeType* t = T(type);
    if (i >= t->fields.size()) return KILN_ERR_NOT_FOUND;
    out->name = KilnStr{t->fields[i].name.c_str(), t->fields[i].name.size()};
    out->type = K(t->fields[i].type);
    out->offset = t->fields[i].offset;
    return KILN_OK;
}
static KilnResult KILN_CALL f_type_enum(const KilnType* type, uint32_t i, KilnStr* name, int64_t* value) {
    const FakeType* t = T(type);
    if (i >= t->values.size()) return KILN_ERR_NOT_FOUND;
    *name = KilnStr{t->values[i].first.c_str(), t->values[i].first.size()};
    *value = t->values[i].second;
    return KILN_OK;
}

static KilnTypesApi g_types_api;
static FakeType *g_item_id_field_owner, *g_state_flag, *g_trigger_enum, *g_slot_id, *g_runtime_id, *g_context;

// This build's shapes, as the server's reflection has them (docs/re/player-state.md, docs/re/abilities.md section 4).
static void build_types() {
    std::memset(&g_types_api, 0, sizeof g_types_api);
    g_types_api.header = KilnServiceHeader{sizeof(KilnTypesApi), KILN_TYPES_VERSION};
    g_types_api.type_by_name = &f_type_by_name;
    g_types_api.type_info = &f_type_info;
    g_types_api.type_field = &f_type_field;
    g_types_api.type_enum = &f_type_enum;
    auto* u8 = add_type("keen::uint8", KILN_TYPE_U8, 1);
    auto* u32 = add_type("keen::uint32", KILN_TYPE_U32, 4);
    auto* u64 = add_type("keen::uint64", KILN_TYPE_U64, 8);
    auto* item_id = add_type("keen::ItemId", KILN_TYPE_TYPEDEF, 4, u32);
    g_slot_id = add_type("keen::InventorySlotId", KILN_TYPE_STRUCT, 8);
    g_slot_id->fields = {{"inventoryId", u32, 0}, {"slotIndex", u8, 4}};
    auto* iii = add_type("keen::ItemInstanceInfo", KILN_TYPE_STRUCT, 20);
    iii->fields = {{"slotId", g_slot_id, 0}, {"itemId", item_id, 8}, {"pide", u32, 12}};
    g_item_id_field_owner = iii;
    auto* seq_id = add_type("keen::actor::SequenceResourceId", KILN_TYPE_TYPEDEF, 4, u32);
    g_runtime_id = add_type("keen::actor::SequenceRuntimeId", KILN_TYPE_STRUCT, 24);
    g_runtime_id->fields = {{"sequenceId", seq_id, 0}, {"triggerTime", u64, 8}, {"actorEntityId", u32, 0x10}, {"callIndex", u8, 0x14}};
    auto* ctx = add_type("keen::actor::ActionSequenceTriggerContext", KILN_TYPE_STRUCT, 0x170);
    ctx->fields = {{"sequenceRuntimeId", g_runtime_id, 0}, {"triggerType", u32, 0x18}, {"costItem", iii, 0x1C},
                   {"usedItem", iii, 0x30}, {"usedAmmunition", iii, 0x44}};
    g_context = ctx;
    auto* ctx_td = add_type("keen::actor::TriggerContextAlias", KILN_TYPE_TYPEDEF, 0x170, ctx);
    auto* ai = add_type("keen::ecs::ActorInput", KILN_TYPE_STRUCT, 0x1C8);
    ai->fields = {{"triggerContext", ctx_td, 8}, {"triggerCallCounter", u8, 0x178}};
    auto* mask = add_type("keen::actor::StateMask", KILN_TYPE_TYPEDEF, 8, u64);
    auto* base = add_type("keen::ecs::BaseActor", KILN_TYPE_STRUCT, 0xC08);
    base->fields = {{"currentAction", u64, 0}, {"currentState", mask, 0xBF8}};
    base->fields.push_back({"sequenceRuntimeId", g_runtime_id, 0xC08});  // BaseActor holds it, as in the reflection
    auto* actor = add_type("keen::ecs::Actor", KILN_TYPE_STRUCT, 0xC20, base);
    actor->fields = {{"currentColliderMode", u8, 0xC20 - 8}};
    g_state_flag = add_type("keen::actor::StateFlag", KILN_TYPE_ENUM, 1);
    g_state_flag->values = {{"Grounded", 0}, {"Dead", 7}, {"Sitting", 9}, {"Sleeping", 11}, {"Spawning", 12},
                            {"Climbing", 34}, {"HangGliding", 37}, {"IsReadyForGameplay", 50}};
    g_trigger_enum = add_type("keen::actor::ActionSequenceTrigger", KILN_TYPE_ENUM, 1);
    g_trigger_enum->values = {{"Invalid", 0}, {"Primary", 1}, {"ExternalTrigger", 22}};
}

using kab::use::Facts;
using kab::use::Layout;

static void test_resolve() {
    Layout L;
    char why[200] = {};
    CHECK(kab::use::resolve(&g_types_api, &L, why, sizeof why));
    CHECK(L.ok());
    CHECK(L.used_slot == 8 + 0x30);       // through a typedef'd struct field
    CHECK(L.used_item == 8 + 0x30 + 8);   // a typedef'd u32
    CHECK(L.state == 0xBF8);              // in the base class
    CHECK(L.running == 0xC08);            // Actor.sequenceRuntimeId.sequenceId, in the base class, a typedef'd u32
    CHECK(L.pending == 8);                // ActorInput.triggerContext.sequenceRuntimeId.sequenceId
    CHECK(L.cost_slot == 8 + 0x1C && L.cost_item == 8 + 0x1C + 8);  // ActorInput.triggerContext.costItem (#118)
    CHECK(L.context == 8 && L.context_size == 0x170);  // through the typedef
    CHECK(L.dead == 7 && L.ready == 50 && L.primary == 1);
    CHECK(L.busy == ((1ull << 9) | (1ull << 11) | (1ull << 12) | (1ull << 34) | (1ull << 37)));  // names not here: skipped

    CHECK(!kab::use::resolve(nullptr, &L, why, sizeof why) && std::strstr(why, "kiln.types"));
    // a missing field
    auto saved = g_item_id_field_owner->fields;
    g_item_id_field_owner->fields.erase(g_item_id_field_owner->fields.begin() + 1);
    CHECK(!kab::use::resolve(&g_types_api, &L, why, sizeof why) && std::strstr(why, "itemId"));
    g_item_id_field_owner->fields = saved;
    // a slot id of another size
    g_slot_id->size = 4;
    CHECK(!kab::use::resolve(&g_types_api, &L, why, sizeof why) && std::strstr(why, "slotId"));
    g_slot_id->size = 8;
    // no runtime id's sequence (the pending trigger and the running sequence both read it): refused
    auto rid = g_runtime_id->fields;
    g_runtime_id->fields.erase(g_runtime_id->fields.begin());
    CHECK(!kab::use::resolve(&g_types_api, &L, why, sizeof why) && std::strstr(why, "sequenceRuntimeId"));
    g_runtime_id->fields = rid;
    // a context too small to hold the used item it is said to hold: refused
    for (auto* t : g_types)
        if (t->qname == "keen::actor::ActionSequenceTriggerContext") t->size = 0x38;  // ends inside usedItem.itemId
    CHECK(!kab::use::resolve(&g_types_api, &L, why, sizeof why) && std::strstr(why, "triggerContext"));
    for (auto* t : g_types)
        if (t->qname == "keen::actor::ActionSequenceTriggerContext") t->size = 0x170;
    // no Dead
    auto flags = g_state_flag->values;
    g_state_flag->values.erase(g_state_flag->values.begin() + 1);
    CHECK(!kab::use::resolve(&g_types_api, &L, why, sizeof why) && std::strstr(why, "StateFlag"));
    g_state_flag->values = flags;
    // no Primary
    auto trig = g_trigger_enum->values;
    g_trigger_enum->values.erase(g_trigger_enum->values.begin() + 1);
    CHECK(!kab::use::resolve(&g_types_api, &L, why, sizeof why) && std::strstr(why, "Primary"));
    g_trigger_enum->values = trig;
    CHECK(kab::use::resolve(&g_types_api, &L, why, sizeof why));
    // no costItem in the reflection: item_use stays on, only the cost item is off
    auto ctx_fields = g_context->fields;
    g_context->fields.erase(g_context->fields.begin() + 2);
    CHECK(kab::use::resolve(&g_types_api, &L, why, sizeof why) && L.ok() && L.cost_slot == -1 && L.cost_item == -1);
    g_context->fields = ctx_fields;
    CHECK(kab::use::resolve(&g_types_api, &L, why, sizeof why) && L.cost_slot == 8 + 0x1C);
    CHECK(kab::use::field_path(&g_types_api, "keen::ecs::ActorInput", "triggerContext.nothing", nullptr) == -1);
    CHECK(kab::use::field_path(&g_types_api, "keen::NoSuchType", "a", nullptr) == -1);
    CHECK(kab::use::enum_value(&g_types_api, "keen::actor::StateFlag", "NoSuchFlag") == -1);
}

static void test_verdict() {
    Layout L;
    char why[200];
    kab::use::resolve(&g_types_api, &L, why, sizeof why);
    const uint64_t ready = 1ull << 50;
    Facts ok;
    ok.own_slot = true;
    ok.player = true;
    ok.item = ok.expected = 0x1234;
    ok.count = 5;
    ok.state = ready;
    CHECK(kab::use::verdict(ok, L) == KILN_ITEM_USE_STARTED);
    Facts f = ok;
    f.own_slot = false;
    f.item = 0;  // not own beats empty
    CHECK(kab::use::verdict(f, L) == KILN_ITEM_USE_NOT_OWN_SLOT);
    f = ok; f.item = 0;
    CHECK(kab::use::verdict(f, L) == KILN_ITEM_USE_EMPTY_SLOT);
    f = ok; f.count = 0;
    CHECK(kab::use::verdict(f, L) == KILN_ITEM_USE_EMPTY_SLOT);
    f = ok; f.item = 0x9999;
    CHECK(kab::use::verdict(f, L) == KILN_ITEM_USE_OTHER_ITEM);
    f = ok; f.player = false;
    CHECK(kab::use::verdict(f, L) == KILN_ITEM_USE_NOT_A_PLAYER);
    f = ok; f.state = ready | (1ull << 7);
    CHECK(kab::use::verdict(f, L) == KILN_ITEM_USE_DEAD);
    f = ok; f.state = 1ull << 7;  // dead beats not ready
    CHECK(kab::use::verdict(f, L) == KILN_ITEM_USE_DEAD);
    f = ok; f.state = 0;
    CHECK(kab::use::verdict(f, L) == KILN_ITEM_USE_NOT_READY);
    f = ok; f.running = 0xdeadbeef;
    CHECK(kab::use::verdict(f, L) == KILN_ITEM_USE_BUSY);
    f = ok; f.pending = 0xabc;  // a trigger already waits for this step
    CHECK(kab::use::verdict(f, L) == KILN_ITEM_USE_BUSY);
    f = ok; f.state = ready | (1ull << 37);  // HangGliding
    CHECK(kab::use::verdict(f, L) == KILN_ITEM_USE_BUSY);
    f = ok; f.state = ready | (1ull << 0);  // Grounded is no reason
    CHECK(kab::use::verdict(f, L) == KILN_ITEM_USE_STARTED);
}

static void test_request() {
    CHECK(kab::use::check_request(nullptr) == KILN_ERR_INVALID_ARGUMENT);
    KilnItemUse u{};
    u.size = sizeof u - 1;
    CHECK(kab::use::check_request(&u) == KILN_ERR_INVALID_ARGUMENT);
    u = KilnItemUse{sizeof(KilnItemUse), 7, KILN_SLOT_ID(100, 2), 0x1234, 0xabc, 0, 99, 99, 0, 99};
    CHECK(kab::use::check_request(&u) == KILN_OK && u.status == KILN_ITEM_USE_STARTED && u.count == 0 && u.trigger_time == 0);
    for (int i = 0; i < 5; ++i) {
        KilnItemUse b{sizeof(KilnItemUse), 7, KILN_SLOT_ID(100, 2), 0x1234, 0xabc, 0, 0, 0, 0, 0};
        if (i == 0) b.entity = 0;
        if (i == 1) b.slot = 0;
        if (i == 2) b.item = 0;
        if (i == 3) b.sequence = 0;
        if (i == 4) b.flags = 2;  // a flag this kiln.abilities does not know
        CHECK(kab::use::check_request(&b) == KILN_ERR_INVALID_ARGUMENT);
    }
    // KILN_ITEM_USE_COST_ITEM (since 0.2.0-dev.31.5) is a known flag
    u = KilnItemUse{sizeof(KilnItemUse), 7, KILN_SLOT_ID(100, 2), 0x1234, 0xabc, KILN_ITEM_USE_COST_ITEM, 0, 0, 0, 0};
    CHECK(kab::use::check_request(&u) == KILN_OK);
    u.flags = KILN_ITEM_USE_COST_ITEM | 4;
    CHECK(kab::use::check_request(&u) == KILN_ERR_INVALID_ARGUMENT);
    // the used item lands where the reflection says, the rest of the context stays
    Layout L;
    char why[200];
    kab::use::resolve(&g_types_api, &L, why, sizeof why);
    uint8_t ai[0x1C8];
    std::memset(ai, 0xEE, sizeof ai);
    kab::use::write_used(ai, L, KILN_SLOT_ID(100, 2), 0x1234);
    uint64_t slot;
    uint32_t item;
    std::memcpy(&slot, ai + 0x38, 8);
    std::memcpy(&item, ai + 0x40, 4);
    CHECK(slot == KILN_SLOT_ID(100, 2) && item == 0x1234);
    CHECK(ai[0x37] == 0xEE && ai[0x44] == 0xEE);
    CHECK(ai[0x24] == 0xEE && ai[0x2c] == 0xEE);  // no cost item asked: costItem untouched
    // with the cost item: the same slot and item at triggerContext.costItem, the hand use's 0x36b60
    kab::use::write_used(ai, L, KILN_SLOT_ID(100, 3), 0x5678, true);
    std::memcpy(&slot, ai + 0x24, 8);
    std::memcpy(&item, ai + 0x2c, 4);
    CHECK(slot == KILN_SLOT_ID(100, 3) && item == 0x5678);
    std::memcpy(&slot, ai + 0x38, 8);
    std::memcpy(&item, ai + 0x40, 4);
    CHECK(slot == KILN_SLOT_ID(100, 3) && item == 0x5678);
    CHECK(ai[0x23] == 0xEE && ai[0x30] == 0xEE);  // costItem.pide and what is before it untouched
    // abort_trigger clears the whole context, as actor_clear_input does, and nothing around it
    kab::use::abort_trigger(ai, L);
    bool cleared = true;
    for (size_t i = 8; i < 8 + 0x170; ++i) cleared = cleared && ai[i] == 0;
    CHECK(cleared && ai[7] == 0xEE && ai[8 + 0x170] == 0xEE);
}

// ------------------------------------------------------------------------------- abilities.cpp's t_item_use ----
// The globals and helpers of abilities.cpp the extracted code uses, faked.
#define __try try
#define __except(x) catch (...)
#define EXCEPTION_EXECUTE_HANDLER 1
constexpr uint32_t kActor = 0x009e8b34u, kActorInput = 0x5c3c6dd8u;
struct Engine {
    static constexpr int64_t actor_sequence = 0xc08;
    int64_t trigger_counter = 0x178;
};
static Engine E;
struct SeqHold { uint32_t entity = 0; };
static std::vector<SeqHold> g_seq_holds;
static std::atomic<uintptr_t> g_world{0};
static bool g_have_sequences = true;
static bool g_tick = true;
static uintptr_t g_server_world = 0;
static std::map<uint32_t, int> g_sequence_keys;  // known sequences
static int g_key_marker;
static const void* sequence_key(uint32_t s) { return g_sequence_keys.count(s) ? &g_key_marker : nullptr; }
static bool on_tick() { return g_tick; }
static void logi(const char*, ...) {}
static void logw(const char*, ...) {}
static std::map<std::pair<uint32_t, uint32_t>, std::vector<uint8_t>> g_comps;
static void* component(uintptr_t, uint32_t hash, uint32_t e, uint64_t* size) {
    auto it = g_comps.find({hash, e});
    if (it == g_comps.end()) return nullptr;
    if (size) *size = it->second.size();
    return it->second.data();
}
static bool read_bytes(const void* p, void* out, size_t n) { std::memcpy(out, p, n); return true; }
static int64_t ecs_now(uintptr_t) { return 123456789; }
struct TriggerCall { uint32_t entity, sequence; int64_t now; uint16_t trigger; };
static std::vector<TriggerCall> g_triggers;
static bool g_trigger_ok = true;
static bool g_trigger_half = false;  // a fault after the runtime id is written (the guarded call returns false)
// as the game's 0x369d0: clears the context (0x170 bytes at +8), writes the runtime id and the trigger type
static bool trigger_sequence(void* ai, uint32_t e, uint32_t seq, int64_t now, uint16_t trigger) {
    if (!g_trigger_ok) return false;
    auto* p = static_cast<uint8_t*>(ai);
    if (g_trigger_half) {
        std::memset(p + 8, 0, 0x170);
        std::memcpy(p + 8, &seq, 4);
        return false;
    }
    std::memset(p + 8, 0, 0x170);
    std::memcpy(p + 8, &seq, 4);
    std::memcpy(p + 0x10, &now, 8);
    std::memcpy(p + 0x18, &e, 4);
    std::memcpy(p + 0x20, &trigger, 2);
    g_triggers.push_back({e, seq, now, trigger});
    return true;
}
struct FakeKilnApi {
    const void* (*service)(KilnStr name, uint32_t min_version);
    uintptr_t (*server_world)();
};
static bool g_types_here = true, g_items_here = true;
static KilnItemsApi g_items_api;
static const void* f_service(KilnStr n, uint32_t) {
    if (n.len == std::strlen(KILN_TYPES_SERVICE) && std::memcmp(n.ptr, KILN_TYPES_SERVICE, n.len) == 0)
        return g_types_here ? &g_types_api : nullptr;
    if (n.len == std::strlen(KILN_ITEMS_SERVICE) && std::memcmp(n.ptr, KILN_ITEMS_SERVICE, n.len) == 0)
        return g_items_here ? &g_items_api : nullptr;
    return nullptr;
}
static uintptr_t f_server_world() { return g_server_world; }
static FakeKilnApi g_fake_api{&f_service, &f_server_world};
static FakeKilnApi* g_api = &g_fake_api;
// abilities.cpp's thread-locals of a Kiln native call (the thunk sets them while a native, attachment or wrap runs)
struct NativeCtx;
static thread_local int t_slot = -1;
static thread_local NativeCtx* t_ctx = nullptr;

// The fake player's inventories as kiln.items reads them.
static std::vector<KilnItemsSlot> g_slots;
static uint8_t g_setup_flags = 0x01;
static KilnResult g_read_rc = KILN_OK;
static uint32_t g_read_entity = 0;
static KilnResult KILN_CALL f_read(KilnEntity e, KilnItemsInventory* info, KilnItemsSlot* slots, uint32_t cap, uint32_t* n) {
    g_read_entity = e;
    if (n) *n = 0;
    if (g_read_rc != KILN_OK) return g_read_rc;
    info->setup_flags = g_setup_flags;
    uint32_t w = 0;
    for (const auto& s : g_slots)
        if (w < cap) slots[w++] = s;
    *n = w;
    return KILN_OK;
}

#include "item_use_under_test.h"

constexpr uint32_t kPlayer = 7, kMushroom = 0x1234, kSeq = 0xabc;
constexpr uint64_t kSlot = KILN_SLOT_ID(100, 2);
static uint64_t g_ready_state = 1ull << 50;

static void reset_world() {
    g_comps.clear();
    g_comps[{kActorInput, kPlayer}] = std::vector<uint8_t>(0x1C8, 0xEE);
    std::memset(g_comps[{kActorInput, kPlayer}].data() + 8, 0, 0x170);  // no trigger waits: actor_clear_input cleared it
    std::vector<uint8_t> actor(0xC20, 0);
    std::memcpy(actor.data() + 0xBF8, &g_ready_state, 8);
    g_comps[{kActor, kPlayer}] = actor;
    g_slots.clear();
    for (uint32_t i = 0; i < 8; ++i) {
        KilnItemsSlot s{};
        s.slot = KILN_SLOT_ID(100, i);
        if (i == 2) { s.item = kMushroom; s.count = 5; }
        if (i == 3) { s.item = 0x5555; s.count = 1; }
        g_slots.push_back(s);
    }
    g_setup_flags = 0x01;
    g_read_rc = KILN_OK;
    g_read_entity = 0;
    g_triggers.clear();
    g_trigger_ok = true;
    g_trigger_half = false;
    t_slot = -1;
    t_ctx = nullptr;
    g_have_sequences = true;
    g_tick = true;
    g_server_world = 0x1000;
    g_world.store(0x1000);
    g_sequence_keys = {{kSeq, 1}, {0xbbb, 1}};
    g_types_here = g_items_here = true;
    g_use_state = 0;  // resolved again
}

static KilnItemUse req(uint64_t slot = kSlot, uint32_t item = kMushroom, uint32_t seq = kSeq) {
    KilnItemUse u{};
    u.size = sizeof u;
    u.entity = kPlayer;
    u.slot = slot;
    u.item = item;
    u.sequence = seq;
    return u;
}

static void set_state(uint64_t s) { std::memcpy(g_comps[{kActor, kPlayer}].data() + 0xBF8, &s, 8); }
static void set_running(uint32_t s) { std::memcpy(g_comps[{kActor, kPlayer}].data() + 0xC08, &s, 4); }

static void test_service() {
    std::memset(&g_items_api, 0, sizeof g_items_api);
    g_items_api.header = KilnServiceHeader{sizeof(KilnItemsApi), KILN_ITEMS_VERSION};
    g_items_api.read = &f_read;

    // the hand use, as the game does it
    reset_world();
    g_seq_holds = {{kPlayer}, {8}};
    KilnItemUse u = req();
    CHECK(t_item_use(&u) == KILN_OK);
    CHECK(u.status == KILN_ITEM_USE_STARTED && u.count == 5 && u.trigger_time == 123456789);
    CHECK(g_read_entity == kPlayer);
    CHECK(g_triggers.size() == 1 && g_triggers[0].entity == kPlayer && g_triggers[0].sequence == kSeq &&
          g_triggers[0].trigger == 1 /* Primary */ && g_triggers[0].now == 123456789);
    {
        const uint8_t* ai = g_comps[{kActorInput, kPlayer}].data();
        uint64_t slot;
        uint32_t item, seq, pide;
        std::memcpy(&slot, ai + 0x38, 8);
        std::memcpy(&item, ai + 0x40, 4);
        std::memcpy(&pide, ai + 0x44, 4);
        std::memcpy(&seq, ai + 8, 4);
        CHECK(slot == kSlot && item == kMushroom && pide == 0 && seq == kSeq);  // the used item after the trigger's clear
    }
    CHECK(g_seq_holds.size() == 1 && g_seq_holds[0].entity == 8);  // the player's held sequence is replaced
    CHECK(g_use_state == 1);
    {
        // no KILN_ITEM_USE_COST_ITEM: costItem stays as the trigger cleared it (a food's use, unchanged since dev.30.3)
        const uint8_t* ai = g_comps[{kActorInput, kPlayer}].data();
        uint64_t slot;
        uint32_t item;
        std::memcpy(&slot, ai + 0x24, 8);
        std::memcpy(&item, ai + 0x2c, 4);
        CHECK(slot == 0 && item == 0);
    }

    // #118: a repair kit's use, KILN_ITEM_USE_COST_ITEM: the slot and the item as costItem too, which PayUsageCost reads
    reset_world();
    u = req();
    u.flags = KILN_ITEM_USE_COST_ITEM;
    CHECK(t_item_use(&u) == KILN_OK && u.status == KILN_ITEM_USE_STARTED && g_triggers.size() == 1);
    {
        const uint8_t* ai = g_comps[{kActorInput, kPlayer}].data();
        uint64_t used_slot, cost_slot;
        uint32_t used_item, cost_item, cost_pide;
        std::memcpy(&used_slot, ai + 0x38, 8);
        std::memcpy(&used_item, ai + 0x40, 4);
        std::memcpy(&cost_slot, ai + 0x24, 8);
        std::memcpy(&cost_item, ai + 0x2c, 4);
        std::memcpy(&cost_pide, ai + 0x30, 4);
        CHECK(used_slot == kSlot && used_item == kMushroom);
        CHECK(cost_slot == kSlot && cost_item == kMushroom && cost_pide == 0);
    }
    // a refusal with the flag writes nothing either
    reset_world();
    set_running(0xbbb);
    u = req();
    u.flags = KILN_ITEM_USE_COST_ITEM;
    CHECK(t_item_use(&u) == KILN_OK && u.status == KILN_ITEM_USE_BUSY && g_triggers.empty());
    {
        uint32_t cost_item;
        std::memcpy(&cost_item, g_comps[{kActorInput, kPlayer}].data() + 0x2c, 4);
        CHECK(cost_item == 0);
    }
    // a build whose reflection has no costItem: the flag is unavailable (nothing triggered), a use without it still works
    reset_world();
    {
        auto ctx_fields = g_context->fields;
        g_context->fields.erase(g_context->fields.begin() + 2);
        u = req();
        u.flags = KILN_ITEM_USE_COST_ITEM;
        CHECK(t_item_use(&u) == KILN_ERR_UNAVAILABLE && g_triggers.empty());
        u = req();
        CHECK(t_item_use(&u) == KILN_OK && u.status == KILN_ITEM_USE_STARTED && g_triggers.size() == 1);
        g_context->fields = ctx_fields;
    }

    // refusals: KILN_OK with a status, and no trigger
    struct Case { const char* name; void (*setup)(); KilnItemUse r; uint32_t status; };
    const Case cases[] = {
        {"a slot of another inventory", [] {}, req(KILN_SLOT_ID(200, 2)), KILN_ITEM_USE_NOT_OWN_SLOT},
        {"no inventory at all", [] { g_read_rc = KILN_ERR_NOT_FOUND; }, req(), KILN_ITEM_USE_NOT_OWN_SLOT},
        {"an empty slot", [] {}, req(KILN_SLOT_ID(100, 4)), KILN_ITEM_USE_EMPTY_SLOT},
        {"another item", [] {}, req(KILN_SLOT_ID(100, 3)), KILN_ITEM_USE_OTHER_ITEM},
        {"not a player's inventories", [] { g_setup_flags = 0x02; }, req(), KILN_ITEM_USE_NOT_A_PLAYER},
        {"dead", [] { set_state((1ull << 50) | (1ull << 7)); }, req(), KILN_ITEM_USE_DEAD},
        {"not ready", [] { set_state(0); }, req(), KILN_ITEM_USE_NOT_READY},
        {"in a sequence", [] { set_running(0xbbb); }, req(), KILN_ITEM_USE_BUSY},
        {"sleeping", [] { set_state((1ull << 50) | (1ull << 11)); }, req(), KILN_ITEM_USE_BUSY},
        {"a trigger waits for this step", [] {
             const uint32_t other = 0xbbb;
             std::memcpy(g_comps[{kActorInput, kPlayer}].data() + 8, &other, 4);
         }, req(), KILN_ITEM_USE_BUSY},
    };
    for (const Case& c : cases) {
        reset_world();
        c.setup();
        KilnItemUse r = c.r;
        const KilnResult rc = t_item_use(&r);
        if (rc != KILN_OK || r.status != c.status || !g_triggers.empty()) {
            ++g_fail;
            std::printf("FAIL %s: rc %d status %u triggers %zu\n", c.name, rc, r.status, g_triggers.size());
        }
    }

    // a second use in the same step: the first one's trigger waits, so the second is BUSY and the first is kept
    reset_world();
    u = req();
    CHECK(t_item_use(&u) == KILN_OK && u.status == KILN_ITEM_USE_STARTED);
    KilnItemUse again = req();
    CHECK(t_item_use(&again) == KILN_OK && again.status == KILN_ITEM_USE_BUSY && g_triggers.size() == 1);
    {
        uint32_t item;
        std::memcpy(&item, g_comps[{kActorInput, kPlayer}].data() + 0x40, 4);
        CHECK(item == kMushroom);
    }

    // inside a Kiln native call (the world's step): refused before anything is read or written
    reset_world();
    t_slot = 3;
    u = req();
    CHECK(t_item_use(&u) == KILN_ERR_WRONG_THREAD && g_triggers.empty() && g_read_entity == 0);
    reset_world();
    t_ctx = reinterpret_cast<NativeCtx*>(&g_key_marker);
    CHECK(t_item_use(&u) == KILN_ERR_WRONG_THREAD && g_triggers.empty());

    // TriggerSequence faults half way (its runtime id written, no used item): the trigger is taken back
    reset_world();
    g_trigger_half = true;
    u = req();
    CHECK(t_item_use(&u) == KILN_ERR_INTERNAL);
    {
        const uint8_t* ai = g_comps[{kActorInput, kPlayer}].data();
        bool cleared = true;
        for (size_t i = 8; i < 8 + 0x170; ++i) cleared = cleared && ai[i] == 0;
        CHECK(cleared && ai[7] == 0xEE);
        CHECK(g_use_state == 1);  // taken back: item_use stays on
    }

    // errors
    reset_world();
    g_tick = false;
    u = req();
    CHECK(t_item_use(&u) == KILN_ERR_WRONG_THREAD && g_triggers.empty());
    reset_world();
    g_have_sequences = false;
    CHECK(t_item_use(&u) == KILN_ERR_UNAVAILABLE);
    reset_world();
    g_server_world = 0;  // a joined client: no server world
    CHECK(t_item_use(&u) == KILN_ERR_WRONG_THREAD && g_triggers.empty());
    reset_world();
    g_types_here = false;
    CHECK(t_item_use(&u) == KILN_ERR_UNAVAILABLE && g_use_state == -1);
    g_types_here = true;
    CHECK(t_item_use(&u) == KILN_ERR_UNAVAILABLE);  // off for the process once it failed
    reset_world();
    g_items_here = false;
    CHECK(t_item_use(&u) == KILN_ERR_UNAVAILABLE && g_triggers.empty());
    reset_world();
    u = req(kSlot, kMushroom, 0xfff);
    CHECK(t_item_use(&u) == KILN_ERR_NOT_FOUND);  // a sequence the game does not have
    reset_world();
    g_sequence_keys.clear();  // no sequence list on this build: sequence_play's rule, the call goes on
    CHECK(t_item_use(&u) == KILN_OK && u.status == KILN_ITEM_USE_STARTED);
    reset_world();
    g_comps.erase({kActorInput, kPlayer});
    u = req();
    CHECK(t_item_use(&u) == KILN_OK && u.status == KILN_ITEM_USE_NOT_A_PLAYER && g_triggers.empty());
    reset_world();
    g_comps[{kActor, kPlayer}].resize(0xBF8);  // too small for currentState
    CHECK(t_item_use(&u) == KILN_OK && u.status == KILN_ITEM_USE_NOT_A_PLAYER && g_triggers.empty());
    reset_world();
    g_read_rc = KILN_ERR_UNAVAILABLE;
    CHECK(t_item_use(&u) == KILN_ERR_UNAVAILABLE && g_triggers.empty());
    reset_world();
    g_trigger_ok = false;
    CHECK(t_item_use(&u) == KILN_ERR_INTERNAL);
    reset_world();
    u = req();
    u.flags = 2;
    CHECK(t_item_use(&u) == KILN_ERR_INVALID_ARGUMENT);
    CHECK(t_item_use(nullptr) == KILN_ERR_INVALID_ARGUMENT);
}

int main() {
    static_assert(sizeof(KilnItemUse) == 48 && offsetof(KilnItemUse, trigger_time) == 40);
    static_assert(offsetof(KilnAbilitiesApi, item_use) == 312 && sizeof(KilnAbilitiesApi) == 320);
    build_types();
    test_resolve();
    test_verdict();
    test_request();
    test_service();
    for (auto* t : g_types) delete t;
    g_types.clear();
    if (g_fail) {
        std::printf("item_use_test: %d FAIL\n", g_fail);
        return 1;
    }
    std::printf("item_use_test: PASS\n");
    return 0;
}
