// SPDX-License-Identifier: MIT
// kiln.gemcraft (gem wave G3): a Shroud Forge's four gem actions from code, and the players' own forge actions before
// (held for one step, cancelable) and after (what changed, what was paid). docs/re/gems-forge.md, ABI
// native/core/abi/services/kiln_gemcraft.h, logic without game memory in gems_logic.h (host test gems_test.cpp).
//
// The game's action is equipment_actions' per-player weapon-gem handler (sigdb gems/GemAction; server 26430253 0x144bb0,
// client 26430625 0x385880). It takes the program's unpacked iterator (0xf0 bytes, layout in gems_logic.h) and reads the
// request from PlayerInput.weaponGemAction, compared with ServerConsumedPlayerInput.consumedWeaponGemAction.
//  - Code actions: kiln_items_gems is an entity program over the player characters, registered through kiln.world with
//    equipment_actions' own access (minus the consumed input, plus kiln_items' Fuel and PickupEvent for its payments).
//    For a request it builds the handler's iterator from its own (mapped by name), with a request buffer of its own as
//    PlayerInput and a consumed record of its own, and calls the handler: every check, cost and payment of the game
//    applies, and the client's input is never touched.
//  - Players' actions: a hook on the handler. While a hold lease runs, a player's new action is not passed on (its
//    version stays unconsumed, so the game offers it again next step) until Pre has an answer: allowed, it runs; canceled,
//    the handler runs once on a copy of the request without a forge, which only marks the version consumed.
//  - Costs: the handler's own cost functions (captured from its calls) price an action before it runs.
#include "gems.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "engine.h"
#include "equip.h"
#include "equip_logic.h"
#include "gems_logic.h"
#include "instances.h"
#include "xfer.h"
#include "kiln_hook.hpp"
#include "services/kiln_caps.h"
#include "services/kiln_gemcraft.h"
#include "services/kiln_players.h"

static_assert(sizeof(KilnGemRequest) == 56 && offsetof(KilnGemRequest, gem_slot) == 24, "kiln_gemcraft.h: KilnGemRequest (managed mirror)");
static_assert(sizeof(KilnGemCraft) == 232 && offsetof(KilnGemCraft, gem_slot) == 40 && offsetof(KilnGemCraft, ticket) == 88 &&
                  offsetof(KilnGemCraft, message) == 104,
              "kiln_gemcraft.h: KilnGemCraft (managed mirror)");
static_assert(sizeof(KilnGemcraftStatus) == 188, "kiln_gemcraft.h: KilnGemcraftStatus (managed mirror)");
static_assert(sizeof(KilnGemcraftApi) == 8 + 8 * 8, "kiln_gemcraft.h: the table (managed mirror)");

namespace kitems {
namespace {

using gems::Snapshot;

#define LOGI(...) items_log(KILN_LOG_INFO, __VA_ARGS__)
#define LOGW(...) items_log(KILN_LOG_WARN, __VA_ARGS__)
#define LOGE(...) items_log(KILN_LOG_ERROR, __VA_ARGS__)

constexpr const char* kFeature = "gems.craft";
constexpr const char* kProgram = "kiln_items_gems";
constexpr uint32_t kMaxPending = 256;
constexpr uint32_t kPerPlayerStep = 4;
constexpr uint64_t kTimeoutSteps = 250;
constexpr uint32_t kMaxCrafted = 1024;
constexpr uint32_t kInputBytes = 0x1000;     // our request's PlayerInput stand-in (the handler reads up to +0x27c)
constexpr uint32_t kConsumedBytes = 0x200;   // our consumed record's stand-in (it reads and writes +0x98)
constexpr uint32_t kInputCopy = 0x400;       // what a cancel copies of a player's PlayerInput (covers the gem action)
constexpr uint32_t kSocketBytes = 12;        // WeaponGemSlot {ItemStack weaponGem}
constexpr uint32_t kLevelMin = 8;

template <typename T>
T rd(const void* p, int64_t off) {
    T v;
    std::memcpy(&v, static_cast<const uint8_t*>(p) + off, sizeof v);
    return v;
}
template <typename T>
void wr(void* p, int64_t off, T v) {
    std::memcpy(static_cast<uint8_t*>(p) + off, &v, sizeof v);
}

uint32_t fnv(const char* s) {
    uint32_t h = 0x811c9dc5u;
    for (; *s; ++s) h = (h ^ static_cast<uint8_t>(*s)) * 0x01000193u;
    return h;
}
KilnStr str(const char* s) { return KilnStr{s, std::strlen(s)}; }

// ---- the game's functions and layouts (sigdb server.d/gems-forge.json) ----
using GemActionFn = void(__fastcall*)(uint64_t* it);
using ReadTemplateFn = const uint8_t*(__fastcall*)(void* storage, uint64_t accessor, uint32_t entity);
using SlotCostFn = uint64_t(__fastcall*)(uint32_t max_level, float multiplier, float modifier, void* balancing);
using UpgradeCostFn = uint64_t(__fastcall*)(uint32_t level, uint32_t max_level, float multiplier, float modifier, void* balancing);
using RecycleYieldFn = uint64_t(__fastcall*)(uint32_t level, uint32_t max_level, void* balancing);
using MultiplierFn = float(__fastcall*)(uint32_t material, uint32_t key, void* recipes);
using CurrentEntityFn = uint32_t*(__fastcall*)(void* ctx, uint32_t* out);

struct Game {
    GemActionFn action = nullptr;
    ReadTemplateFn read_template = nullptr;
    SlotCostFn slot_cost = nullptr, unslot_cost = nullptr;
    UpgradeCostFn upgrade_cost = nullptr;
    RecycleYieldFn recycle_yield = nullptr;
    MultiplierFn multiplier = nullptr;
    const float* one = nullptr;
    CurrentEntityFn current_entity = nullptr;
    int64_t in_weapon = 0, in_source = 0, in_forge = 0, in_material = 0, in_version = 0, in_action = 0;
    int64_t consumed_version = 0;
    int64_t tpl_material = 0, tpl_allow = 0, tpl_max = 0, tpl_modifier = 0;
    int64_t item_category = 0;
    uint32_t attr_level = 0, attr_cap = 0, mult_key = 0;
    bool ok = false;
};
Game G;

const KilnApi* g_api = nullptr;
KilnOwner g_owner = 0;

std::mutex g_mu;  // everything below that is not atomic; never held while calling the game or a callback
char g_detail[160] = "not resolved";
std::atomic<int> g_layout{0};  // 0 not checked in this world, 1 ok, 2 differs (gems.craft off)
std::atomic<bool> g_off{false};  // switched off after a fault or a failed check, until restart

// hook
kiln::RawHook g_hook;
std::atomic<bool> g_hooked{false};
bool g_hook_failed = false;  // g_mu
std::atomic<uint64_t> g_hold_until{0};
std::atomic<bool> g_watch{false};
std::atomic<uint64_t> g_view_storage{0};  // the storage whose handler accessors were checked
gems::HoldBook g_book;                    // g_mu
std::vector<KilnGemCraft> g_crafted;      // g_mu
std::atomic<uint32_t> g_crafted_total{0};
std::atomic<uint32_t> g_hook_faults{0};

// code actions
struct Request {
    uint64_t ticket;
    KilnOwner owner;
    KilnGemRequest r;
    uint64_t submitted;
    bool session = false;
    uint32_t handle = 0;
    uint64_t verified_step = 0;
};
struct Callback {
    KilnGemDoneFn fn;
    void* user;
};
std::vector<Request> g_pending;                   // g_mu
std::vector<KilnGemCraft> g_done;                 // g_mu
std::unordered_map<uint64_t, Callback> g_callbacks;  // g_mu
std::atomic<uint32_t> g_pending_n{0};

// program
const KilnWorldApi* g_w = nullptr;
const KilnPlayersApi* g_players = nullptr;
KilnHandle g_prog = 0;
std::atomic<int> g_state{KILN_ITEMS_PROGRAM_NONE};
bool g_slots = false;
int g_s_equipment = -1, g_s_sing[6] = {-1, -1, -1, -1, -1, -1}, g_s_tpl = -1, g_s_other[9] = {}, g_s_ev[3] = {-1, -1, -1};
uint8_t* g_tx = nullptr;
int g_v_sing[4] = {}, g_v_acc[A_COUNT] = {}, g_v_ev[2] = {};  // resolve_slots: the slots in verify_access' order
alignas(16) uint8_t g_input[kInputBytes];
alignas(16) uint8_t g_consumed[kConsumedBytes];
thread_local bool t_in_code = false;  // this thread runs a code action: the hook passes it straight through

void set_detail(const char* s) {
    std::lock_guard lock(g_mu);
    std::snprintf(g_detail, sizeof g_detail, "%s", s);
}

void lower(uint32_t reason, const char* why) {
    set_detail(why);
    const auto* caps = static_cast<const KilnCapsApi*>(g_api ? g_api->service(str(KILN_CAPS_SERVICE), KILN_CAPS_VERSION) : nullptr);
    if (caps && caps->report) caps->report(g_owner, str(kFeature), KILN_CAP_UNAVAILABLE, reason, str(why));
}

void switch_off(const char* why) {
    if (!g_off.exchange(true)) {
        LOGE("gems: forge actions off until the server, or the hosting player's game, restarts: %s", why);
        lower(KILN_CAP_REASON_FAULTED, why);
    }
}

// ---- the handler's iterator as Kiln reads it ----
struct View {
    uint64_t* it = nullptr;
    void* ctx = nullptr;
    void* storage = nullptr;
    void* balancing = nullptr;
    void* items = nullptr;
    void* recipes = nullptr;
    void* attributes = nullptr;
    Access a{};  // for read_pages (Inventory, InventorySetup)
};

bool view_of(uint64_t* it, View* v) {
    v->it = it;
    v->ctx = reinterpret_cast<void*>(it[gems::I_CTX]);
    if (!v->ctx) return false;
    void* job = *static_cast<void**>(v->ctx);
    if (!job) return false;
    v->storage = E.job_storage(job);
    if (!v->storage) return false;
    v->balancing = reinterpret_cast<void*>(it[gems::I_BALANCING]);
    v->items = reinterpret_cast<void*>(it[gems::I_ITEMS]);
    v->recipes = reinterpret_cast<void*>(it[gems::I_RECIPES]);
    v->attributes = reinterpret_cast<void*>(it[gems::I_ATTRIBUTES]);
    v->a.ctx = v->ctx;
    v->a.job = job;
    v->a.storage = v->storage;
    v->a.items = v->items;
    v->a.balancing = v->balancing;
    v->a.attributes = v->attributes;
    v->a.acc[A_INVENTORY] = it[gems::I_INVENTORY];
    v->a.acc[A_EMPTY] = it[gems::I_EMPTY];
    v->a.acc[A_SETUP] = it[gems::I_SETUP];
    v->a.acc[A_ITEMSTATE] = it[gems::I_ITEMSTATE];
    v->a.acc[A_OWNER] = it[gems::I_OWNER];
    v->a.acc[A_SLOTSEL] = it[gems::I_SLOTSEL];
    return v->balancing && v->items && v->attributes && v->recipes;
}

// The handler iterator's eight other-entity accessors hold the type indices of the components the layout names (once
// per entity storage; the type indices are the world's).
bool view_checked(const View& v, char* why, size_t cap) {
    if (g_view_storage.load(std::memory_order_acquire) == reinterpret_cast<uint64_t>(v.storage)) return true;
    void* registry = *static_cast<void**>(v.storage);
    if (!registry) return std::snprintf(why, cap, "no ECS registry"), false;
    for (int i = 0; i < 8; ++i) {
        char name[64];
        std::snprintf(name, sizeof name, "keen::ecs::%s", gems::kOtherWrite[i]);
        const uint32_t h = fnv(name);
        const uint16_t* want = E.component_map_find(static_cast<uint8_t*>(registry) + E.registry_components, &h);
        const uint64_t acc = v.it[gems::I_INVENTORY + i];
        if (!want || !acc || *reinterpret_cast<const uint16_t*>(acc) != *want)
            return std::snprintf(why, cap, "the handler's accessor %d is not %s (iterator layout)", i, gems::kOtherWrite[i]), false;
    }
    g_view_storage.store(reinterpret_cast<uint64_t>(v.storage), std::memory_order_release);
    return true;
}

const uint8_t* other(const View& v, uint64_t acc, uint32_t e, uint64_t min) {
    if (!acc || !e) return nullptr;
    PtrSize ps{};
    E.read_other(&ps, v.storage, acc, e);
    return ps.ptr && ps.size >= min ? static_cast<const uint8_t*>(ps.ptr) : nullptr;
}

// A Level-like attribute block {u32 group; u16 values offset; u8 flags (bit 0: values); ...; s32 values[]}.
bool attr_value(const View& v, const uint8_t* block, uint32_t attribute, uint32_t* out) {
    if (!block || !(block[6] & 1)) return false;
    AttrRef r{};
    E.attr_find(&r, attribute, v.attributes);
    if (r.error || !r.desc || rd<uint32_t>(r.desc, 0) != rd<uint32_t>(block, 0)) return false;
    *out = rd<uint32_t>(block, rd<uint16_t>(block, 4) + 4ll * rd<uint16_t>(r.desc, 0xc));
    return true;
}

bool level_of(const View& v, uint32_t pide, uint32_t* level, uint32_t* cap) {
    const uint8_t* lv = other(v, v.it[gems::I_LEVEL], pide, kLevelMin);
    return lv && attr_value(v, lv, G.attr_level, level) && attr_value(v, lv, G.attr_cap, cap);
}

bool socket_of(const View& v, uint32_t carrier, uint32_t* item, uint32_t* pide) {
    const uint8_t* s = other(v, v.it[gems::I_GEMSLOT], carrier, kSocketBytes);
    if (!s) return false;
    *item = rd<uint32_t>(s, 0);
    const uint32_t p = rd<uint32_t>(s, 8);
    *pide = *item && rd<uint32_t>(s, 4) ? (p == 0xFFFFFFFFu ? 0 : p) : 0;
    if (!*pide) *item = 0;
    return true;
}

// The stack in an InventorySlotId as the handler's lookup finds it: an inventory's slot, or (an entity without an
// Inventory, index 0) its socket.
bool stack_at(const View& v, uint64_t slot, uint32_t* item, uint32_t* count, uint32_t* pide) {
    *item = *count = *pide = 0;
    const uint32_t e = KILN_SLOT_INVENTORY(slot), k = KILN_SLOT_INDEX(slot);
    if (k >= kSlotsPerPage) return false;
    if (const uint8_t* inv = other(v, v.it[gems::I_INVENTORY], e, kInventorySize)) {
        *item = rd<uint32_t>(inv, 12 * k);
        *count = rd<uint32_t>(inv, 12 * k + 4);
        const uint32_t p = rd<uint32_t>(inv, 12 * k + 8);
        *pide = p == 0xFFFFFFFFu ? 0 : p;
        if (!*item || !*count) *item = *count = *pide = 0;
        return true;
    }
    if (k != 0) return false;
    if (!socket_of(v, e, item, pide)) return false;
    *count = *item ? 1 : 0;
    return true;
}

struct Counts {
    uint32_t a = 0, b = 0;
    bool has_pide = false;
};
Counts count_in(const View& v, KilnEntity holder, uint32_t item_a, uint32_t item_b, uint32_t pide) {
    Counts c;
    PageSet ps;
    bool init = false;
    if (!read_pages(v.a, holder, &ps, &init)) return c;
    for (uint32_t i = 0; i < ps.n; ++i)
        for (uint32_t k = 0; k < kSlotsPerPage; ++k) {
            const Stack& s = ps.p[i].s[k];
            if (is_empty(s)) continue;
            if (item_a && s.item == item_a) c.a += s.count;
            if (item_b && s.item == item_b) c.b += s.count;
            if (pide && pide_of(s) == pide) c.has_pide = true;
        }
    return c;
}

// A free slot in one of the player's Generic (4) links: where the game's unslot puts the gem.
bool free_generic(const View& v, KilnEntity p) {
    const uint8_t* setup = other(v, v.it[gems::I_SETUP], p, kSetupSize);
    if (!setup) return true;  // unknown: let the game decide
    for (int k = 0; k < 16; ++k) {
        const uint32_t link = rd<uint32_t>(setup, 4 * k);
        if (link == 0) break;
        if (setup[0x40 + k] != 4) continue;
        if (const uint8_t* inv = other(v, v.it[gems::I_INVENTORY], link, kInventorySize))
            for (uint32_t j = 0; j < 8; ++j)
                if (rd<uint32_t>(inv, 12 * j + 4) == 0) return true;
    }
    return false;
}

struct Forge {
    bool ok = false;
    uint32_t max_level = 0, essence = 0;
    float modifier = 1.0f;
    bool allow_all = false;
};
Forge forge_of(const View& v, uint32_t forge) {
    Forge f;
    if (!forge) return f;
    const uint8_t* t = G.read_template(v.storage, v.it[gems::I_FORGE_TPL], forge);
    if (!t) return f;
    f.max_level = rd<uint32_t>(t, G.tpl_max);
    std::memcpy(&f.modifier, t + G.tpl_modifier, 4);
    f.allow_all = t[G.tpl_allow] != 0;
    f.essence = E.guid_to_id ? E.guid_to_id(t + G.tpl_material) : 0;
    f.ok = f.max_level != 0;
    return f;
}

// One action as the handler sees it (the request part of ChangeWeaponGemAction).
struct Act {
    uint32_t action = 0, weapon = 0, forge = 0, material = 0;
    uint64_t source = 0;
};

// What the game will charge (Slot, Unslot, Upgrade: in `material`) or give (Recycle: the forge's essence).
uint32_t game_price(const View& v, const Act& a, const Forge& f, uint32_t level) {
    float mult = G.one ? *G.one : 1.0f;
    if (f.allow_all && a.action != KILN_GEM_ACTION_RECYCLE) mult = G.multiplier(a.material, G.mult_key, v.recipes);
    switch (a.action) {
        case KILN_GEM_ACTION_SLOT: return static_cast<uint32_t>(G.slot_cost(f.max_level, mult, f.modifier, v.balancing));
        case KILN_GEM_ACTION_UNSLOT: return static_cast<uint32_t>(G.unslot_cost(f.max_level, mult, f.modifier, v.balancing));
        case KILN_GEM_ACTION_UPGRADE: return static_cast<uint32_t>(G.upgrade_cost(level, f.max_level, mult, f.modifier, v.balancing));
        case KILN_GEM_ACTION_RECYCLE: return static_cast<uint32_t>(G.recycle_yield(level, f.max_level, v.balancing));
        default: return 0;
    }
}

// The state an action is judged by. `look_pide`: a gem entity to look for in the player's inventories.
void snapshot(const View& v, KilnEntity player, const Act& a, uint32_t material, uint32_t essence, uint32_t look_pide, Snapshot* s) {
    *s = Snapshot{};
    if (a.weapon) {
        uint32_t item = 0, pide = 0;
        s->socket = socket_of(v, a.weapon, &item, &pide);
        s->socket_item = item;
        s->socket_pide = pide;
    }
    if (a.source) stack_at(v, a.source, &s->source_item, &s->source_count, &s->source_pide);
    switch (a.action) {
        case KILN_GEM_ACTION_SLOT: s->gem_pide = s->source_pide; break;
        case KILN_GEM_ACTION_UNSLOT: s->gem_pide = s->socket_pide; break;
        default: s->gem_pide = a.source ? s->source_pide : s->socket_pide; break;
    }
    if (s->gem_pide) s->level_known = level_of(v, s->gem_pide, &s->level, &s->cap);
    const Counts c = count_in(v, player, material, essence, look_pide);
    s->material = c.a;
    s->essence = c.b;
    s->gem_in_player = c.has_pide;
}

uint32_t item_of_pide(const View& v, uint32_t pide) {
    const uint8_t* st = other(v, v.it[gems::I_ITEMSTATE], pide, kItemStateSize);
    return st ? rd<uint32_t>(st, 8) : 0;
}

uint8_t category_of(const View& v, uint32_t item) {
    const auto* info = static_cast<const uint8_t*>(E.item_find(item, v.items));
    return info ? info[G.item_category] : 0xFF;
}

// The record of an action before it runs: who, what, where, the forge, the price.
void describe(const View& v, KilnEntity player, const Act& a, const Forge& f, const Snapshot& b, KilnGemCraft* c) {
    std::memset(c, 0, sizeof *c);
    c->size = sizeof *c;
    c->action = a.action;
    c->status = KILN_GEM_DONE;
    c->player = player;
    c->forge = a.forge;
    c->carrier = a.weapon;
    c->carrier_item = a.weapon ? item_of_pide(v, a.weapon) : 0;
    c->gem = b.gem_pide;
    c->gem_item = b.gem_pide ? item_of_pide(v, b.gem_pide) : 0;
    c->gem_slot = a.source ? a.source : (a.weapon ? KILN_SLOT_ID(a.weapon, 0) : 0);
    c->level_before = c->level_after = b.level;
    c->level_cap = b.cap;
    c->forge_max_level = f.max_level;
    if (f.ok) {
        const uint32_t price = game_price(v, a, f, b.level);
        c->flags |= KILN_GEMCRAFT_PRICED;
        if (a.action == KILN_GEM_ACTION_RECYCLE) {
            c->yield_item = f.essence;
            c->yield_count = price;
        } else if (price) {
            c->cost_item = a.material;
            c->cost_count = price;
        }
    }
}

void push_crafted(const KilnGemCraft& c) {
    std::lock_guard lock(g_mu);
    if (g_crafted.size() < kMaxCrafted) g_crafted.push_back(c);
    g_crafted_total.fetch_add(1);
}

void call_handler(uint64_t* it) {
    if (g_hooked.load(std::memory_order_acquire)) g_hook.call<void>(it);
    else G.action(it);
}

// ================================================================================ the hook (players' actions) ====

Act act_of(const uint8_t* input) {
    Act a;
    a.weapon = rd<uint32_t>(input, G.in_weapon);
    a.source = rd<uint64_t>(input, G.in_source) & 0xFF'FFFF'FFFFull;
    a.forge = rd<uint32_t>(input, G.in_forge);
    a.material = rd<uint32_t>(input, G.in_material);
    a.action = input[G.in_action];
    return a;
}

// What the hook does for one call, decided with the game's memory read under a guard.
enum class HookDo : uint8_t { Pass, Hold, Cancel, Watch };
struct HookCall {
    View v;
    uint32_t player = 0, version = 0;
    Act act;
    Forge forge;
    Snapshot before;
    KilnGemCraft info{};
};

bool new_action(uint64_t* it, uint32_t* version) {
    const auto* input = reinterpret_cast<const uint8_t*>(it[gems::I_INPUT]);
    const auto* consumed = reinterpret_cast<const uint8_t*>(it[gems::I_CONSUMED]);
    if (!input || !consumed) return false;
    *version = rd<uint32_t>(input, G.in_version);
    return *version != rd<uint32_t>(consumed, G.consumed_version);
}

bool prepare_impl(uint64_t* it, HookCall* h, char* why) {
    if (!new_action(it, &h->version)) return false;
    if (!view_of(it, &h->v) || !view_checked(h->v, why, 160)) return false;
    uint32_t e = 0;
    G.current_entity(h->v.ctx, &e);
    h->player = e;
    h->act = act_of(reinterpret_cast<const uint8_t*>(it[gems::I_INPUT]));
    h->forge = forge_of(h->v, h->act.forge);
    const uint32_t essence = h->act.action == KILN_GEM_ACTION_RECYCLE ? h->forge.essence : 0;
    snapshot(h->v, h->player, h->act, h->act.material, essence, 0, &h->before);
    describe(h->v, h->player, h->act, h->forge, h->before, &h->info);
    h->info.version = h->version;
    h->info.step = current_step();
    return true;
}
bool prepare(uint64_t* it, HookCall* h, bool* go, char* why) {
    __try {
        *go = prepare_impl(it, h, why);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// After an allowed (or never held) action ran: what it did.
void after_impl(HookCall* h) {
    const auto* consumed = reinterpret_cast<const uint8_t*>(h->v.it[gems::I_CONSUMED]);
    if (rd<uint32_t>(consumed, G.consumed_version) != h->version) return;  // the handler did not take it
    Snapshot a;
    const uint32_t essence = h->act.action == KILN_GEM_ACTION_RECYCLE ? h->forge.essence : 0;
    snapshot(h->v, h->player, h->act, h->act.material, essence, h->before.socket_pide, &a);
    const gems::Outcome o = gems::judge(h->act.action, h->before, a);
    KilnGemCraft c = h->info;
    c.status = o.status;
    c.level_after = o.level_after;
    c.cost_item = o.paid ? h->act.material : 0;
    c.cost_count = o.paid;
    c.yield_item = o.got ? essence : 0;
    c.yield_count = o.got;
    c.step = current_step();
    if (o.status != KILN_GEM_DONE) std::snprintf(c.message, sizeof c.message, "the game's %s changed nothing", gems::action_name(c.action));
    push_crafted(c);
}
bool after(HookCall* h) {
    __try {
        after_impl(h);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// The handler on a copy of the iterator whose request has no forge: it only stores the consumed version.
void cancel_call(uint64_t* it) {
    alignas(16) uint8_t input[kInputCopy];
    uint64_t copy[gems::I_COUNT];
    std::memcpy(copy, it, sizeof copy);
    std::memcpy(input, reinterpret_cast<const void*>(it[gems::I_INPUT]), sizeof input);
    wr<uint32_t>(input, G.in_forge, 0);
    copy[gems::I_INPUT] = reinterpret_cast<uint64_t>(input);
    call_handler(copy);
}

void __fastcall hook_gem_action(uint64_t* it) {
    if (t_in_code) return g_hook.call<void>(it);  // our own request (kiln_items_gems), never a player's action
    const uint64_t step = current_step();
    const bool holding = step <= g_hold_until.load(std::memory_order_acquire);
    const bool watching = g_watch.load(std::memory_order_acquire);
    if ((!holding && !watching) || g_layout.load(std::memory_order_acquire) != 1 || g_off.load(std::memory_order_acquire))
        return g_hook.call<void>(it);
    HookCall h;
    bool go = false;
    char why[160] = "";
    if (!prepare(it, &h, &go, why)) {
        if (g_hook_faults.fetch_add(1) == 0) LOGE("gems: reading a player's forge action faulted; the hook passes everything through");
        switch_off("reading a player's forge action faulted (see kiln.log)");
        return g_hook.call<void>(it);
    }
    if (!go) {
        if (why[0]) switch_off(why);
        return g_hook.call<void>(it);
    }
    if (holding) {
        gems::HoldBook::Now now;
        {
            std::lock_guard lock(g_mu);
            now = g_book.on_action(h.player, h.version, step, &h.info);
        }
        if (now == gems::HoldBook::Now::Hold) return;  // not passed on: the version stays unconsumed, the game offers it again
        if (now == gems::HoldBook::Now::Cancel) return cancel_call(it);
    }
    g_hook.call<void>(it);
    if (watching && !after(&h)) switch_off("reading the state after a player's forge action faulted");
}

bool install_hook(char* why, size_t cap) {
    if (g_hooked.load()) return true;
    std::lock_guard lock(g_mu);
    if (g_hooked.load()) return true;
    if (g_hook_failed) return std::snprintf(why, cap, "the gem handler hook could not be installed (see kiln.log)"), false;
    uintptr_t target = 0;
    if (g_api->symbol(str("gems/GemAction"), &target) != KILN_OK || !target)
        return std::snprintf(why, cap, "gems/GemAction is not resolved"), false;
    if (!g_hook.install(g_api, g_owner, target, reinterpret_cast<void*>(&hook_gem_action), "gems/GemAction")) {
        g_hook_failed = true;
        LOGE("gems: hooking the game's gem handler failed: %s", g_hook.error() ? g_hook.error() : "?");
        return std::snprintf(why, cap, "hooking the game's gem handler failed"), false;
    }
    g_hooked.store(true, std::memory_order_release);
    LOGI("gems: the game's gem handler %p hooked (players' forge actions: hold, cancel, report)", reinterpret_cast<void*>(target));
    return true;
}

// ================================================================================ the program (code actions) ====

const char* const kSing[6] = {"BalancingRegistry", "g38_items::ItemRegistry", "g38_items::RecipeRegistry",
                              "impact::AttributeRegistry", "KnowledgeTriggerQueue", nullptr};
const char* const kOtherNames[9] = {"Fuel", "Inventory", "InventoryEmptyState", "InventorySetup", "ItemState",
                                    "Level", "OwnerRelationship", "SlotSelection", "WeaponGemSlot"};
const char* const kEvents[3] = {"GemCraftingEvent", "InventoryFull", "PickupEvent"};

bool resolve_slots() {
    if (g_slots) return true;
    g_s_equipment = g_w->program_slot(g_prog, KILN_SLOT_WRITE, str("Equipment"));
    for (int i = 0; i < 5; ++i) g_s_sing[i] = g_w->program_slot(g_prog, KILN_SLOT_SINGLETON, str(kSing[i]));
    g_s_tpl = g_w->program_slot(g_prog, KILN_SLOT_OTHER_TPL, str("ShroudForge"));
    for (int i = 0; i < 9; ++i) g_s_other[i] = g_w->program_slot(g_prog, KILN_SLOT_OTHER_WRITE, str(kOtherNames[i]));
    for (int i = 0; i < 3; ++i) g_s_ev[i] = g_w->program_slot(g_prog, KILN_SLOT_EVENT_OUT, str(kEvents[i]));
    auto good = [](int s) { return s >= 1 && s < 64; };
    bool ok = good(g_s_equipment) && good(g_s_tpl);
    for (int i = 0; i < 5; ++i) ok = ok && good(g_s_sing[i]);
    for (int i = 0; i < 9; ++i) ok = ok && good(g_s_other[i]);
    for (int i = 0; i < 3; ++i) ok = ok && good(g_s_ev[i]);
    // verify_access' order: singletons Balancing, ItemRegistry, AttributeRegistry, KnowledgeTriggerQueue; accessors
    // Fuel, Inventory, InventoryEmptyState, InventorySetup, ItemState, OwnerRelationship, SlotSelection; events PickupEvent,
    // InventoryFull
    const int sing[4] = {g_s_sing[0], g_s_sing[1], g_s_sing[3], g_s_sing[4]};
    const int accs[A_COUNT] = {g_s_other[0], g_s_other[1], g_s_other[2], g_s_other[3], g_s_other[4], g_s_other[6], g_s_other[7]};
    std::memcpy(g_v_sing, sing, sizeof sing);
    std::memcpy(g_v_acc, accs, sizeof accs);
    g_v_ev[0] = g_s_ev[2];
    g_v_ev[1] = g_s_ev[1];
    return g_slots = ok;
}

// The handler's iterator from ours (mapped by name), with our request buffer and consumed record.
void build_iterator(void* ctx, const uint64_t* it, uint64_t* h) {
    std::memset(h, 0, gems::kIterBytes);
    h[gems::I_CTX] = reinterpret_cast<uint64_t>(ctx);
    h[gems::I_INPUT] = reinterpret_cast<uint64_t>(g_input);
    for (uint32_t k = 0; k < gems::kWriteRecordQwords; ++k) h[gems::I_EQUIPMENT + k] = it[g_s_equipment + static_cast<int>(k)];
    h[gems::I_CONSUMED] = reinterpret_cast<uint64_t>(g_consumed);  // tracker (+5) stays 0: nothing marks our record
    for (int i = 0; i < 4; ++i) h[gems::I_BALANCING + i] = it[g_s_sing[i]];
    h[gems::I_KNOWLEDGE] = it[g_s_sing[4]];
    h[gems::I_FORGE_TPL] = it[g_s_tpl];
    for (int i = 0; i < 8; ++i) h[gems::I_INVENTORY + i] = it[g_s_other[1 + i]];
    h[gems::I_EV_GEM] = it[g_s_ev[0]];
    h[gems::I_EV_FULL] = it[g_s_ev[1]];
}

void set_request(const Act& a) {
    std::memset(g_input, 0, sizeof g_input);
    std::memset(g_consumed, 0, sizeof g_consumed);
    wr<uint32_t>(g_input, G.in_weapon, a.weapon);
    wr<uint64_t>(g_input, G.in_source, a.source);
    wr<uint32_t>(g_input, G.in_forge, a.forge);
    wr<uint32_t>(g_input, G.in_material, a.material);
    wr<uint32_t>(g_input, G.in_version, 1);
    g_input[G.in_action] = static_cast<uint8_t>(a.action);
    wr<uint32_t>(g_consumed, G.consumed_version, 0);
}

int32_t give_take(const Access& acc, bool take, KilnEntity e, uint32_t item, uint32_t count, char* why) {
    if (!item || !count) return KILN_ITEMS_DONE;
    Job j;
    j.take = take;
    j.r.size = sizeof j.r;
    j.r.entity = e;
    j.r.item = item;
    j.r.count = count;
    j.r.flags = take ? 0 : KILN_ITEMS_ALL_OR_NOTHING;
    KilnItemsResult res{};
    res.size = sizeof res;
    unsigned long code = 0;
    if (!run_job(acc, j, g_tx, &res, &code)) {
        abandon_tx(g_tx);
        std::snprintf(why, 128, "the game faulted (0x%08lx) in a %s", code, take ? "take" : "give");
        return KILN_ITEMS_UNAVAILABLE;
    }
    return res.status;
}

struct CodeRun {
    const Access* acc;
    uint64_t* h;
    KilnEntity player;
    KilnGemRequest r;
    KilnGemCraft out;
    bool fault;
};

void finish_status(KilnGemCraft* c, int32_t status, const char* why) {
    c->status = status;
    std::snprintf(c->message, sizeof c->message, "%s", why);
}

// One code action inside the program update (guarded by run_code).
void run_code_impl(CodeRun* cr) {
    View v;
    KilnGemCraft* c = &cr->out;
    const KilnGemRequest& r = cr->r;
    if (!view_of(cr->h, &v)) return finish_status(c, KILN_GEM_UNAVAILABLE, "the program's iterator is incomplete");
    char why[160] = "";
    if (!view_checked(v, why, sizeof why)) {
        switch_off(why);
        return finish_status(c, KILN_GEM_UNAVAILABLE, why);
    }
    const KilnEntity player = cr->player;
    const KilnEntity holder = r.holder ? r.holder : player;
    // where the carrier and the gem are: the holder's inventories (the player's own, or the one the caller names)
    PageSet ps;
    bool init = false;
    if (!read_pages(*cr->acc, holder, &ps, &init) || !init)
        return finish_status(c, KILN_GEM_NOT_FOUND, holder == player ? "the player has no initialized inventory" : "the holder has no initialized inventory");
    auto find_pide = [&](uint32_t pide) -> uint64_t {
        for (uint32_t i = 0; i < ps.n; ++i)
            for (uint32_t k = 0; k < kSlotsPerPage; ++k)
                if (!is_empty(ps.p[i].s[k]) && pide_of(ps.p[i].s[k]) == pide) return ps.p[i].s[k].slot;
        return 0;
    };
    auto in_holder = [&](uint64_t slot) {
        for (uint32_t i = 0; i < ps.n; ++i)
            if (ps.p[i].entity == KILN_SLOT_INVENTORY(slot)) return true;
        return false;
    };
    if (r.carrier && !find_pide(r.carrier))
        return finish_status(c, KILN_GEM_NOT_FOUND, "the carrier is not in the holder's inventories");
    if (r.gem_slot && !in_holder(r.gem_slot)) return finish_status(c, KILN_GEM_NOT_FOUND, "the gem slot is not one of the holder's");
    Act a;
    a.action = r.action;
    a.weapon = r.carrier;
    a.forge = r.forge;
    a.source = r.gem_slot;
    if (!r.gem_slot && r.carrier && (r.action == KILN_GEM_ACTION_UPGRADE || r.action == KILN_GEM_ACTION_RECYCLE))
        a.source = KILN_SLOT_ID(r.carrier, 0);  // a socketed gem, as the client names it: the carrier's socket
    const Forge f = forge_of(v, r.forge);
    a.material = f.essence;
    const uint32_t essence = r.action == KILN_GEM_ACTION_RECYCLE ? f.essence : 0;
    Snapshot b;
    snapshot(v, player, a, a.material, essence, 0, &b);
    if (r.gem && b.gem_pide != r.gem) return finish_status(c, KILN_GEM_NOT_FOUND, "the gem is not the item entity the request names");
    describe(v, player, a, f, b, c);
    c->ticket = 0;
    c->flags |= KILN_GEMCRAFT_BY_CODE | ((r.flags & KILN_GEM_FREE) ? KILN_GEMCRAFT_FREE : 0) | ((r.flags & KILN_GEM_COST) ? KILN_GEMCRAFT_COST : 0);
    if (r.action == KILN_GEM_ACTION_SLOT && b.source_pide && category_of(v, b.source_item) != gems::kWeaponGemCategory)
        return finish_status(c, KILN_GEM_NOT_FOUND, "the item in the gem slot is not a gem");
    const uint32_t price = f.ok ? game_price(v, a, f, b.level) : 0;
    const gems::CostPlan plan = gems::plan_cost(r, a.action == KILN_GEM_ACTION_RECYCLE ? 0 : price);
    const uint32_t have = plan.give_material ? price : b.material;  // a free or replaced cost: Kiln gives the game's price
    const char* pre = "";
    const int32_t st = gems::precheck(r.action, b, f.max_level, a.action == KILN_GEM_ACTION_RECYCLE ? 0 : price, have,
                                      r.action != KILN_GEM_ACTION_UNSLOT || free_generic(v, player), &pre);
    if (st != KILN_GEM_DONE) return finish_status(c, st, pre);
    // a replaced cost: taken first (all or nothing), then the game's price given (the game takes it back)
    char w[128] = "";
    if (plan.take_count) {
        const int32_t t = give_take(*cr->acc, true, player, plan.take_item, plan.take_count, w);
        if (t != KILN_ITEMS_DONE) return finish_status(c, t == KILN_ITEMS_UNAVAILABLE ? KILN_GEM_UNAVAILABLE : KILN_GEM_NOT_ENOUGH,
                                                       w[0] ? w : "the player cannot pay the replacement cost");
    }
    if (plan.give_material) {
        const int32_t g = give_take(*cr->acc, false, player, a.material, plan.give_material, w);
        if (g != KILN_ITEMS_DONE) {
            give_take(*cr->acc, false, player, plan.take_item, plan.take_count, w);  // the replacement back
            return finish_status(c, KILN_GEM_NO_ROOM, "no room for the forge's payment Kiln hands over for a free or replaced cost");
        }
    }
    // the game's action, on our request
    Snapshot b2;
    snapshot(v, player, a, a.material, essence, 0, &b2);
    set_request(a);
    t_in_code = true;
    call_handler(cr->h);
    t_in_code = false;
    Snapshot after;
    snapshot(v, player, a, a.material, essence, b2.socket_pide, &after);
    const gems::Outcome o = gems::judge(r.action, b2, after);
    c->level_after = o.level_after;
    c->step = current_step();
    if (o.status != KILN_GEM_DONE) {
        // undo what Kiln did around it
        if (plan.give_material) give_take(*cr->acc, true, player, a.material, plan.give_material, w);
        if (plan.take_count) give_take(*cr->acc, false, player, plan.take_item, plan.take_count, w);
        c->cost_item = c->cost_count = c->yield_item = c->yield_count = 0;
        return finish_status(c, KILN_GEM_REFUSED, "the game's action changed nothing (its own checks refused it)");
    }
    c->status = KILN_GEM_DONE;
    c->message[0] = 0;
    if (r.action == KILN_GEM_ACTION_RECYCLE) {
        c->cost_item = c->cost_count = 0;
        c->yield_item = o.got ? essence : 0;
        c->yield_count = o.got;
        if (plan.replace_yield) {
            give_take(*cr->acc, true, player, essence, o.got, w);
            const int32_t g = give_take(*cr->acc, false, player, plan.yield_item, plan.yield_count, w);
            c->yield_item = plan.yield_item;
            c->yield_count = plan.yield_count;
            if (g != KILN_ITEMS_DONE) {
                give_take(*cr->acc, false, player, essence, o.got, w);  // no room for the replacement: the forge's yield stays
                c->yield_item = o.got ? essence : 0;
                c->yield_count = o.got;
                std::snprintf(c->message, sizeof c->message, "the replacement did not fit: the forge's essence was given");
            }
        }
    } else if (plan.give_material || plan.take_count || (r.flags & KILN_GEM_FREE)) {
        c->cost_item = plan.take_count ? plan.take_item : 0;
        c->cost_count = plan.take_count;
    } else {
        c->cost_item = o.paid ? a.material : 0;
        c->cost_count = o.paid;
    }
}

bool run_code(CodeRun* cr, unsigned long* code) {
    __try {
        run_code_impl(cr);
        return true;
    } __except (*code = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

KilnGemCraft result_base(const Request& q) {
    KilnGemCraft c{};
    c.size = sizeof c;
    c.action = q.r.action;
    c.player = q.r.player;
    c.forge = q.r.forge;
    c.carrier = q.r.carrier;
    c.gem = q.r.gem;
    c.gem_slot = q.r.gem_slot;
    c.ticket = q.ticket;
    c.flags = KILN_GEMCRAFT_BY_CODE;
    return c;
}

void KILN_CALL each(KilnProgramCtx* ctx, uint64_t* it, KilnEntity player, void*) {
    if (g_pending_n.load(std::memory_order_acquire) == 0) return;
    Request batch[kPerPlayerStep];
    uint32_t n = 0;
    {
        std::lock_guard lock(g_mu);
        const uint64_t now = current_step();
        for (auto i = g_pending.begin(); i != g_pending.end() && n < kPerPlayerStep;) {
            if (i->r.player == player && (!i->session || i->verified_step == now)) {
                batch[n++] = *i;
                i = g_pending.erase(i);
            } else {
                ++i;
            }
        }
        g_pending_n.store(static_cast<uint32_t>(g_pending.size()), std::memory_order_release);
    }
    if (!n) return;
    Access acc{};
    bool ok = false;
    char why[160] = "";
    unsigned long code = 0;
    if (g_off.load()) {
        std::snprintf(why, sizeof why, "gems.craft was switched off (see kiln.log)");
    } else if (!resolve_slots()) {
        switch_off("kiln.world did not report kiln_items_gems' iterator slots");
        std::snprintf(why, sizeof why, "gems.craft is off (see kiln.log)");
    } else if (!verify_access(it, g_v_sing, g_v_acc, g_v_ev, ctx, &acc, &ok, why, &code) || !ok) {
        LOGE("gems: kiln_items_gems' iterator: equipment %d, singletons %d %d %d %d %d (%p %p %p %p %p), template %d, events %d %d %d",
             g_s_equipment, g_s_sing[0], g_s_sing[1], g_s_sing[2], g_s_sing[3], g_s_sing[4], reinterpret_cast<void*>(it[g_s_sing[0]]),
             reinterpret_cast<void*>(it[g_s_sing[1]]), reinterpret_cast<void*>(it[g_s_sing[2]]), reinterpret_cast<void*>(it[g_s_sing[3]]),
             reinterpret_cast<void*>(it[g_s_sing[4]]), g_s_tpl, g_s_ev[0], g_s_ev[1], g_s_ev[2]);
        switch_off(why[0] ? why : "checking kiln_items_gems' iterator faulted");
    } else if (g_state.exchange(KILN_ITEMS_PROGRAM_RUNNING) != KILN_ITEMS_PROGRAM_RUNNING) {
        LOGI("program %s runs: accessors verified (player %u)", kProgram, player);
    }
    uint64_t h[gems::I_COUNT];
    for (uint32_t i = 0; i < n; ++i) {
        CodeRun cr{};
        cr.acc = &acc;
        cr.h = h;
        cr.player = player;
        cr.r = batch[i].r;
        cr.out = result_base(batch[i]);
        if (g_off.load() || g_layout.load() != 1) {
            finish_status(&cr.out, KILN_GEM_UNAVAILABLE, why[0] ? why : "gems.craft is off (see kiln.log)");
        } else {
            build_iterator(ctx, it, h);
            if (!run_code(&cr, &code)) {
                t_in_code = false;
                abandon_tx(g_tx);
                char b[160];
                std::snprintf(b, sizeof b, "the game faulted (0x%08lx) in a %s for player %u", code, gems::action_name(cr.r.action), player);
                finish_status(&cr.out, KILN_GEM_UNAVAILABLE, b);
                switch_off(b);
            }
        }
        if (cr.out.status != KILN_GEM_DONE) cr.out.cost_item = cr.out.cost_count = cr.out.yield_item = cr.out.yield_count = 0;  // a refusal pays nothing
        cr.out.ticket = batch[i].ticket;
        if (!cr.out.step) cr.out.step = current_step();
        std::lock_guard lock(g_mu);
        g_done.push_back(cr.out);
        if (cr.out.status == KILN_GEM_DONE && g_crafted.size() < kMaxCrafted) {
            g_crafted.push_back(cr.out);
            g_crafted_total.fetch_add(1);
        }
    }
}

// ================================================================================ Pre ====

void poll_program() {
    if (!g_w || g_state.load() != KILN_ITEMS_PROGRAM_PENDING) return;
    KilnProgramInfo pi{};
    pi.size = sizeof pi;
    if (g_w->program_info(g_prog, &pi) != KILN_OK) return;
    if (pi.state == KILN_PROGRAM_ACTIVE) {
        int expected = KILN_ITEMS_PROGRAM_PENDING;
        g_state.compare_exchange_strong(expected, KILN_ITEMS_PROGRAM_ACTIVE);
    } else if (pi.state == KILN_PROGRAM_REFUSED || pi.state == KILN_PROGRAM_FAULTED || pi.state == KILN_PROGRAM_SKIPPED) {
        g_state.store(KILN_ITEMS_PROGRAM_NONE);
        char b[160];
        std::snprintf(b, sizeof b, "kiln.world reports %s as state %u (see kiln.log)", kProgram, pi.state);
        lower(KILN_CAP_REASON_FAULTED, b);
        LOGE("gems: %s", b);
    }
}

// equipment_actions' declaration, read from the server world's program directory, against the handler's iterator layout.
void check_layout() {
    if (g_layout.load() != 0 || !g_w || g_w->header.size < offsetof(KilnWorldApi, game_program_list) + sizeof(void*)) return;
    const int32_t idx = g_w->game_program_find(KILN_WORLD_SERVER, str("equipment_actions"));
    if (idx < 0) return;  // the directory is not published yet
    std::vector<std::vector<std::string>> lists(KILN_GAME_LISTS);
    for (int l = 0; l < KILN_GAME_LISTS; ++l) {
        KilnStr names[32];
        const int32_t n = g_w->game_program_list(KILN_WORLD_SERVER, idx, l, names, 32);
        if (n < 0 || n > 32) {
            g_layout.store(2);
            lower(KILN_CAP_REASON_SYMBOL_MISSING, "equipment_actions' access lists cannot be read");
            return;
        }
        for (int32_t k = 0; k < n; ++k) lists[l].emplace_back(names[k].ptr, names[k].len);
    }
    char why[160] = "";
    if (!gems::lists_match(lists, why, sizeof why)) {
        g_layout.store(2);
        lower(KILN_CAP_REASON_SYMBOL_MISSING, why);
        LOGE("gems: forge actions off: %s", why);
        return;
    }
    g_layout.store(1);
    LOGI("gems: equipment_actions' declaration matches the gem handler's iterator (%d entries)", gems::I_COUNT);
}

bool session_still(KilnEntity player, uint32_t handle, bool* readable) {
    *readable = false;
    if (!g_players || handle == 0) return false;
    KilnPlayerSlotRaw now{};
    now.size = sizeof now;
    if (g_players->raw_slot(slot_of_handle(handle), &now) != KILN_OK) return false;
    *readable = true;
    return session_holds(now, handle, player);
}

bool guarded_done(KilnGemDoneFn fn, const KilnGemCraft* r, void* user) {
    __try {
        fn(r, user);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void deliver() {
    std::vector<KilnGemCraft> done;
    std::vector<std::pair<KilnGemCraft, Callback>> calls;
    {
        std::lock_guard lock(g_mu);
        done.swap(g_done);
        for (const KilnGemCraft& r : done) {
            auto i = g_callbacks.find(r.ticket);
            if (i == g_callbacks.end()) continue;
            calls.emplace_back(r, i->second);
            g_callbacks.erase(i);
        }
    }
    for (const KilnGemCraft& r : done)
        LOGI("gems: ticket %llu: %s player %u carrier %u gem %u (level %u -> %u) forge %u: status %d, paid %u x 0x%08x, got %u x 0x%08x%s%s",
             static_cast<unsigned long long>(r.ticket), gems::action_name(r.action), r.player, r.carrier, r.gem, r.level_before,
             r.level_after, r.forge, r.status, r.cost_count, r.cost_item, r.yield_count, r.yield_item, r.message[0] ? ": " : "", r.message);
    for (auto& [r, cb] : calls)
        if (!guarded_done(cb.fn, &r, cb.user)) LOGE("gems: a result callback faulted (ticket %llu)", static_cast<unsigned long long>(r.ticket));
}

// ================================================================================ the table ====

KilnResult KILN_CALL t_status(KilnGemcraftStatus* out) {
    if (!out || out->size < sizeof(KilnGemcraftStatus)) return KILN_ERR_INVALID_ARGUMENT;
    const uint32_t size = out->size;
    std::memset(out, 0, sizeof *out);
    out->size = size;
    const int state = g_state.load();
    out->available = G.ok && !g_off.load() && g_layout.load() != 2 && state != KILN_ITEMS_PROGRAM_NONE ? 1 : 0;
    out->hook = g_hooked.load() ? 1 : 0;
    out->program = static_cast<uint8_t>(state);
    out->holding = current_step() <= g_hold_until.load() ? 1 : 0;
    out->crafted = g_crafted_total.load();
    out->pending = g_pending_n.load();
    std::lock_guard lock(g_mu);
    out->held = g_book.total();
    out->canceled = g_book.canceled();
    std::snprintf(out->detail, sizeof out->detail, "%s", out->available ? "" : g_detail);
    return KILN_OK;
}

KilnResult KILN_CALL t_submit(KilnOwner owner, const KilnGemRequest* r, KilnGemDoneFn done, void* user, uint64_t* out_ticket) {
    if (out_ticket) *out_ticket = 0;
    if (!r || !gems::request_valid(*r)) return KILN_ERR_INVALID_ARGUMENT;
    const int state = g_state.load();
    static std::atomic<uint32_t> refusals{0};
    auto refuse = [&](const char* why) {
        if (refusals.fetch_add(1) < 5) LOGW("gems: a forge action was refused: %s", why);
        return KILN_ERR_UNAVAILABLE;
    };
    if (!G.ok || g_off.load() || g_layout.load() == 2) return refuse("gems.craft is off (see kiln.log)");
    if (!g_w || !g_tx || state == KILN_ITEMS_PROGRAM_NONE || state == KILN_ITEMS_PROGRAM_BROKEN)
        return refuse("the program kiln_items_gems is not registered or broke");
    KilnSessionInfo si{};
    si.size = sizeof si;
    const KilnResult src = items_api()->session(&si);
    if (equip_here(src, si.state) != EquipHere::Yes) {
        char b[96];
        std::snprintf(b, sizeof b, "no live server world here (session query %d, state %u)", src, si.state);
        return refuse(b);
    }
    Request q{};
    q.ticket = items_next_ticket();
    q.owner = owner;
    q.r = *r;
    q.r.size = sizeof(KilnGemRequest);
    q.submitted = current_step();
    if (r->player >= 1 && r->player <= 1023 && g_players) {
        q.session = true;
        KilnPlayer p{};
        p.size = sizeof p;
        if (g_players->get(r->player, &p) == KILN_OK && p.state == KILN_PLAYER_IN_GAME) q.handle = p.handle;
    }
    {
        std::lock_guard lock(g_mu);
        if (g_pending.size() >= kMaxPending) return KILN_ERR_BUFFER_TOO_SMALL;
        g_pending.push_back(q);
        if (done) g_callbacks.emplace(q.ticket, Callback{done, user});
        g_pending_n.store(static_cast<uint32_t>(g_pending.size()), std::memory_order_release);
    }
    if (out_ticket) *out_ticket = q.ticket;
    return KILN_OK;
}

KilnResult KILN_CALL t_cancel(uint64_t ticket) {
    std::lock_guard lock(g_mu);
    return g_callbacks.erase(ticket) ? KILN_OK : KILN_ERR_NOT_FOUND;
}

KilnResult KILN_CALL t_hold(uint32_t steps) {
    if (steps > gems::kMaxHoldSteps) return KILN_ERR_INVALID_ARGUMENT;
    if (steps == 0) {
        g_hold_until.store(0);
        std::lock_guard lock(g_mu);
        g_book.release_all();
        return KILN_OK;
    }
    if (!G.ok || g_off.load() || g_layout.load() == 2) return KILN_ERR_UNAVAILABLE;
    char why[160] = "";
    if (!install_hook(why, sizeof why)) {
        lower(KILN_CAP_REASON_FAULTED, why);
        return KILN_ERR_UNAVAILABLE;
    }
    g_hold_until.store(current_step() + steps);
    return KILN_OK;
}

KilnResult KILN_CALL t_held(KilnGemCraft* out, uint32_t capacity, uint32_t* out_count) {
    if (!out_count || (capacity && !out)) return KILN_ERR_INVALID_ARGUMENT;
    std::lock_guard lock(g_mu);
    uint32_t n = 0;
    g_book.report([&](const gems::Held& h) { out[n++] = h.info; }, capacity);
    *out_count = n;
    return g_book.waiting_reports() ? KILN_ERR_BUFFER_TOO_SMALL : KILN_OK;
}

KilnResult KILN_CALL t_decide(KilnEntity player, uint32_t version, uint32_t cancel) {
    std::lock_guard lock(g_mu);
    return g_book.decide(player, version, cancel != 0) ? KILN_OK : KILN_ERR_NOT_FOUND;
}

KilnResult KILN_CALL t_watch(uint32_t on) {
    if (!on) {
        g_watch.store(false);
        return KILN_OK;
    }
    if (!G.ok || g_off.load() || g_layout.load() == 2) return KILN_ERR_UNAVAILABLE;
    char why[160] = "";
    if (!install_hook(why, sizeof why)) {
        lower(KILN_CAP_REASON_FAULTED, why);
        return KILN_ERR_UNAVAILABLE;
    }
    g_watch.store(true);
    return KILN_OK;
}

KilnResult KILN_CALL t_crafted(KilnGemCraft* out, uint32_t capacity, uint32_t* out_count) {
    if (!out_count || (capacity && !out)) return KILN_ERR_INVALID_ARGUMENT;
    std::lock_guard lock(g_mu);
    const uint32_t n = static_cast<uint32_t>(std::min<size_t>(capacity, g_crafted.size()));
    for (uint32_t i = 0; i < n; ++i) out[i] = g_crafted[i];
    g_crafted.erase(g_crafted.begin(), g_crafted.begin() + n);
    *out_count = n;
    return g_crafted.empty() ? KILN_OK : KILN_ERR_BUFFER_TOO_SMALL;
}

const KilnGemcraftApi kTable = {
    {sizeof(KilnGemcraftApi), KILN_GEMCRAFT_VERSION},
    &t_status, &t_submit, &t_cancel, &t_hold, &t_held, &t_decide, &t_watch, &t_crafted,
};

template <typename F>
bool sym(const char* name, F* out) {
    uintptr_t a = 0;
    if (g_api->symbol(str(name), &a) != KILN_OK || !a) return false;
    *out = reinterpret_cast<F>(a);
    return true;
}
bool cst(const char* name, int64_t* out) { return g_api->constant(str(name), out) == KILN_OK; }

}  // namespace

void gems_resolve() {
    g_api = items_api();
    g_owner = items_owner();
    int64_t lvl = 0, cap = 0, key = 0, iter = 0;
    uintptr_t one = 0;
    bool ok = g_api->feature(str(kFeature)) && E.tx_ok && E.read_ok && E.attr_find && E.guid_to_id && E.component_map_find &&
              sym("gems/GemAction", &G.action) && sym("gems/ReadTemplate", &G.read_template) &&
              sym("gems/GemSlotCost", &G.slot_cost) && sym("gems/GemUnslotCost", &G.unslot_cost) &&
              sym("gems/GemUpgradeCost", &G.upgrade_cost) && sym("gems/GemRecycleYield", &G.recycle_yield) &&
              sym("gems/MaterialMultiplier", &G.multiplier) && sym("gems/MultiplierOne", &one) &&
              sym("world/CurrentEntityId", &G.current_entity) && cst("gems/iter.size", &iter) &&
              cst("gems/input.weapon", &G.in_weapon) && cst("gems/input.source", &G.in_source) &&
              cst("gems/input.material", &G.in_material) && cst("gems/input.version", &G.in_version) &&
              cst("gems/input.action", &G.in_action) && cst("gems/consumed.version", &G.consumed_version) &&
              cst("gems/forge.allow_all", &G.tpl_allow) && cst("gems/forge.max_level", &G.tpl_max) &&
              cst("gems/forge.cost_modifier", &G.tpl_modifier) && cst("gems/attr.level", &lvl) &&
              cst("gems/attr.level_cap", &cap) && cst("gems/multiplier.key", &key) && cst("items/item.category", &G.item_category);
    G.one = reinterpret_cast<const float*>(one);
    G.attr_level = static_cast<uint32_t>(lvl);
    G.attr_cap = static_cast<uint32_t>(cap);
    G.mult_key = static_cast<uint32_t>(key);
    // ChangeWeaponGemAction (28 bytes): weaponPide +0, weaponGem +4 (8), forgeEntityId +0xc, materialItemId +0x10,
    // versionData +0x14, action +0x18; ShroudForge: upgradeMaterial (a GUID) +0x10, allowAll... +0x20
    G.in_forge = G.in_weapon + 0xc;
    G.tpl_material = G.tpl_allow - 0x10;
    const bool layout = iter == static_cast<int64_t>(gems::kIterBytes) && G.in_source == G.in_weapon + 4 &&
                        G.in_material == G.in_weapon + 0x10 && G.in_version == G.in_weapon + 0x14 &&
                        G.in_action == G.in_weapon + 0x18 && G.in_action + 1 <= static_cast<int64_t>(kInputCopy) &&
                        G.consumed_version + 4 <= static_cast<int64_t>(kConsumedBytes) && G.tpl_allow == 0x20 &&
                        G.tpl_max > G.tpl_allow && G.tpl_modifier > G.tpl_max && G.tpl_modifier < 0x40 &&
                        G.item_category > 0 && G.item_category < 0x800 && G.attr_level && G.attr_cap;
    G.ok = ok && layout && G.one;
    if (!G.ok) {
        const char* why = !g_api->feature(str(kFeature)) ? "gems.craft did not resolve on this game build"
                          : !ok ? "a symbol of the item transaction or the gem handler did not resolve"
                                : "the gem handler's layout constants do not check out";
        set_detail(why);
        if (g_api->feature(str(kFeature))) lower(KILN_CAP_REASON_SYMBOL_MISSING, why);
    }
    LOGI("gems: forge actions %s", G.ok ? "possible (needs kiln.world)" : "OFF");
}

void gems_register(const KilnWorldApi* w) {
    if (!G.ok) return;
    if (!w || w->header.size < sizeof(KilnWorldApi)) {
        lower(KILN_CAP_REASON_SERVICE_MISSING, "forge actions need kiln.world (its entity programs): kiln.world is not loaded");
        G.ok = false;
        return;
    }
    g_players = static_cast<const KilnPlayersApi*>(items_api()->service(str(KILN_PLAYERS_SERVICE), 1));
    if (g_players && g_players->header.size < sizeof(KilnPlayersApi)) g_players = nullptr;
    g_tx = static_cast<uint8_t*>(VirtualAlloc(nullptr, static_cast<SIZE_T>(E.tx_size), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (!g_tx) {
        set_detail("no memory for the forge actions' transaction");
        G.ok = false;
        return;
    }
    std::vector<KilnStr> read{str("PlayerInput")}, write{str("Equipment")}, sr, sw{str(kSing[4])}, ot{str("ShroudForge")}, ow, eo;
    for (int i = 0; i < 4; ++i) sr.push_back(str(kSing[i]));
    for (const char* n : kOtherNames) ow.push_back(str(n));
    for (const char* e : kEvents) eo.push_back(str(e));
    KilnProgramDesc d{};
    d.size = sizeof d;
    d.flags = KILN_PROGRAM_HOST_ONLY;  // an entity query: the player characters (PlayerInput)
    d.name = str(kProgram);
    d.each = &each;
    d.read = KilnNames{read.data(), static_cast<uint32_t>(read.size()), 0};
    d.write = KilnNames{write.data(), static_cast<uint32_t>(write.size()), 0};
    d.singletons_read = KilnNames{sr.data(), static_cast<uint32_t>(sr.size()), 0};
    d.singletons_write = KilnNames{sw.data(), static_cast<uint32_t>(sw.size()), 0};
    d.other_tpl = KilnNames{ot.data(), static_cast<uint32_t>(ot.size()), 0};
    d.other_write = KilnNames{ow.data(), static_cast<uint32_t>(ow.size()), 0};
    d.events_out = KilnNames{eo.data(), static_cast<uint32_t>(eo.size()), 0};
    KilnHandle h = 0;
    if (w->register_program(items_owner(), &d, &h) != KILN_OK) {
        lower(KILN_CAP_REASON_FAULTED, "kiln.world refused the program kiln_items_gems (see kiln.log)");
        G.ok = false;
        return;
    }
    g_w = w;
    g_prog = h;
    g_state.store(KILN_ITEMS_PROGRAM_PENDING);
    set_detail("waiting for the server world (kiln.world)");
    LOGI("program %s registered through kiln.world (forge actions)", kProgram);
}

void gems_pre(uint64_t step) {
    if (!G.ok) return;
    poll_program();
    check_layout();
    const bool holding = step <= g_hold_until.load();
    std::vector<std::pair<uint64_t, std::pair<KilnEntity, uint32_t>>> check;  // ticket, player, handle
    {
        std::lock_guard lock(g_mu);
        if (!holding) g_book.release_all();
        g_book.expire(step, kTimeoutSteps);
        const int state = g_state.load();
        for (auto i = g_pending.begin(); i != g_pending.end();) {
            const bool off = g_off.load() || g_layout.load() == 2 || state == KILN_ITEMS_PROGRAM_NONE;
            const bool stale = step > i->submitted + kTimeoutSteps;
            if (off || stale) {
                KilnGemCraft c = result_base(*i);
                finish_status(&c, off ? KILN_GEM_UNAVAILABLE : KILN_GEM_DROPPED,
                              off ? "gems.craft went off" : "the player's program did not run it within 10 s");
                c.step = step;
                g_done.push_back(c);
                i = g_pending.erase(i);
                continue;
            }
            if (i->session) check.push_back({i->ticket, {i->r.player, i->handle}});
            ++i;
        }
        g_pending_n.store(static_cast<uint32_t>(g_pending.size()), std::memory_order_release);
    }
    // the session each request was made for, read now from the game's record (tick thread)
    std::vector<std::pair<uint64_t, bool>> verdicts;
    for (auto& [ticket, ph] : check) {
        bool readable = false;
        verdicts.push_back({ticket, session_still(ph.first, ph.second, &readable)});
    }
    {
        std::lock_guard lock(g_mu);
        for (auto& [ticket, good] : verdicts)
            for (auto i = g_pending.begin(); i != g_pending.end(); ++i) {
                if (i->ticket != ticket) continue;
                if (good) {
                    i->verified_step = step;
                } else {
                    KilnGemCraft c = result_base(*i);
                    finish_status(&c, KILN_GEM_DROPPED, "the player is not in game (or another session holds the slot)");
                    c.step = step;
                    g_done.push_back(c);
                    g_pending.erase(i);
                }
                break;
            }
        g_pending_n.store(static_cast<uint32_t>(g_pending.size()), std::memory_order_release);
    }
    deliver();
}

void gems_world_closed() {
    {
        std::lock_guard lock(g_mu);
        for (const Request& q : g_pending) {
            KilnGemCraft c = result_base(q);
            finish_status(&c, KILN_GEM_DROPPED, "the server world closed");
            g_done.push_back(c);
        }
        g_pending.clear();
        g_pending_n.store(0, std::memory_order_release);
        g_book.clear();
    }
    g_layout.store(0);
    g_view_storage.store(0);
    int expected = KILN_ITEMS_PROGRAM_RUNNING;
    g_state.compare_exchange_strong(expected, KILN_ITEMS_PROGRAM_ACTIVE);
}

KilnResult gems_publish(const KilnApi* api, KilnOwner owner) {
    return api->register_service(owner, str(KILN_GEMCRAFT_SERVICE), &kTable);
}

}  // namespace kitems
