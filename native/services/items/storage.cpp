// SPDX-License-Identifier: MIT
// Player storage: native-only capture hooks; persistence pumps at captures-done and quiescent Post housekeeping.
#include "storage.h"
#include "../characters/kiln_characters.h"
#include "../spawn/midstep.h"
#include "kiln_hook.hpp"
#include "services/kiln_caps.h"
#include "services/kiln_gamedata.h"
#include "services/kiln_players.h"
#include "services/kiln_playerstate.h"
#include "services/kiln_saves.h"
#include "services/kiln_types.h"
#include "storage_client_guard.h"
#include "storage_engine.h"
#include "storage_guard.h"
#include "xfer.h"
#include <atomic>
#include <mutex>
#include <windows.h>
namespace kitems {
KilnResult items_item_info(uint32_t, KilnItemInfo*);
namespace {
using namespace storage;
std::recursive_mutex storage_mu;
std::recursive_mutex callback_mu;
Model model;
const KilnApi* api = nullptr;
const KilnWorldApi* world_api = nullptr;
const KilnTypesApi* types_api = nullptr;
const KilnPlayersApi* players_api = nullptr;
const KilnSavesApi* saves_api = nullptr;
kiln::RawHook capture_hook, read_hook, join_hook, stage_hook;
// The client guard's continuation: kiln.hooks writes it before the link can be reached, and never frees it.
void* transfer_next = nullptr;
KilnStorageStageFn stage_fn = nullptr;
void* stage_user = nullptr;
std::atomic<bool> enabled{false};
bool stage_point = false, post_point = false;
uint32_t reason = KILN_CAP_REASON_SYMBOL_MISSING;
std::string detail = "player storage is starting";
uint64_t step = 0, restore_count = 0, capture_count = 0;
thread_local int read_slot = -1;
thread_local bool in_post = false;
Access structural_access{};
std::vector<Holder> garbage;
struct Ref {
    uint32_t slot;
    uint64_t join;
    std::string key;
    uint32_t equip, index;
    bool only_empty = false;
};
struct Park {
    uint32_t slot;
    uint64_t join;
    std::string key;
    uint32_t reason;
    bool unpark;
    bool retired = false; // the owner went away: tearing its holder down is housekeeping (handle 0)
};
std::deque<Ref> refs;
std::deque<Park> parks;
struct Death {
    uint32_t slot, grave, mode;
    uint64_t join, submitted;
    std::vector<uint32_t> holders;
};
std::deque<Death> deaths;
struct Return {
    KilnOwner owner = 0;
    uint64_t ticket = 0, join = 0, submitted = 0;
    uint32_t slot = 0, target = 0, phase = 0, original_slots = 0, reason = 0;
    std::string key, internal_key;
    KilnStorageReturnFn done = nullptr;
    void* user = nullptr;
    Holder holder;
    std::array<uint16_t, 64> original_indices{};
    KilnStorageReturnResult result{};
    bool all_moved = false;
};
std::deque<Return> returns;
struct ReturnDone {
    KilnOwner owner;
    KilnStorageReturnFn fn;
    void* user;
    KilnStorageReturnResult result;
};
std::deque<ReturnDone> returned;
uint64_t next_return = 1;
struct GraveBefore {
    uint32_t player = 0, mode = 0;
};
std::map<uint32_t, GraveBefore> grave_before;
std::set<uint32_t> graves_done;
KilnHandle death_before_wrap = 0, death_after_wrap = 0;
int32_t loot_list = -1, loot_slot = -1, settings_list = -1, settings_slot = -1;
uint32_t loot_state_offset = 0, tombstone_mode_offset = 0;
struct DeathSlice {
    KilnGameSlice* slice;
    bool before;
};
int64_t player_stride = 0, account_offset = 0, id_offset = 0;
std::array<int64_t, 7> binding_offsets{};
std::array<uint8_t, 128> bindings{};
uint64_t save_cursor = 0;
bool dropped_capture_batch = false;
uint32_t refused_capture_slots = 0;
std::vector<ClientActionLayout> client_actions;
uint32_t input_size = 0, consumed_size = 0;
std::atomic<uint32_t> main_baseline{5}; // read by the join's read hook outside the model mutex
std::atomic<bool> baseline_authored{false};
KilnHandle retiring_subscription = 0;
KilnStr str(const char* s) { return {s, std::strlen(s)}; }
bool same(KilnStr s, const char* text) {
    return s.ptr && s.len == std::strlen(text) && !std::memcmp(s.ptr, text, s.len);
}
bool storage_copy(void* out, const void* in, size_t n) {
    __try {
        if (!in)
            return false;
        std::memcpy(out, in, n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
template <class T> bool load(const void* p, size_t off, T* out) {
    return p && storage_copy(out, static_cast<const uint8_t*>(p) + off, sizeof(T));
}
uint32_t hash_name(const char* name) {
    uint32_t h = 2166136261u;
    for (const char* p = "keen::ecs::"; *p; ++p)
        h = (h ^ static_cast<uint8_t>(*p)) * 16777619u;
    for (; *name; ++name)
        h = (h ^ static_cast<uint8_t>(*name)) * 16777619u;
    return h;
}
void report(const char* name, uint32_t state, uint32_t why, const char* text) {
    auto caps = static_cast<const KilnCapsApi*>(api->service(str(KILN_CAPS_SERVICE), 1));
    if (caps && caps->header.size >= offsetof(KilnCapsApi, report) + sizeof(caps->report))
        caps->report(items_owner(), str(name), state, why, str(text));
}
void off(uint32_t why, const std::string& text) {
    enabled = false;
    reason = why;
    detail = text;
    items_log(KILN_LOG_ERROR, "player-storage: %s", text.c_str());
    report("items.storage", KILN_CAP_UNAVAILABLE, why, text.c_str());
}
void resolve_main_baseline() {
    const uint32_t old_baseline = main_baseline.load();
    const bool old_authored = baseline_authored.load();
    static bool reported = false;
    uint32_t pages = 5;
    bool authored = false;
    auto gd = static_cast<const KilnGamedataApi*>(api->service(str(KILN_GAMEDATA_SERVICE), 1));
    if (gd && gd->header.size >= offsetof(KilnGamedataApi, get) + sizeof(gd->get) && gd->find && gd->get) {
        KilnGdEntry entry{};
        entry.size = sizeof entry;
        KilnContentValue current{}, original{};
        if (gd->find(KILN_GD_TEMPLATES, str("c3db66d6-89a5-4a6c-a8e3-d9a1f6009812"), &entry) == KILN_OK &&
            gd->get(KILN_GD_TEMPLATES, entry.id, str("InventorySetup.genericSlotCount"), &current, &original) ==
                KILN_OK &&
            original.type == KILN_CV_INT && original.i > 0 && original.i <= 128) {
            pages = static_cast<uint32_t>((original.i + 7) / 8);
            authored = true;
        }
    }
    // one store each: the join's read hook never sees a half-updated baseline
    main_baseline = pages;
    baseline_authored = authored;
    if (reported && old_baseline == pages && old_authored == authored)
        return;
    reported = true;
    items_log(KILN_LOG_INFO, "player-storage: main-page baseline %u (%s)", pages,
              authored ? "1_Player original InventorySetup" : "baseline from the authored default");
    report("items.storage.main_page_baseline", KILN_CAP_AVAILABLE, KILN_CAP_REASON_NONE,
           authored ? "1_Player original InventorySetup.genericSlotCount"
                    : "5 pages: baseline from the authored default");
}
bool structural_gate() {
    return in_post && api && api->on_tick_thread() && api->server_world() &&
           kspawn::structural_gate(true, world_api, types_api) == kspawn::Gate::Ok;
}
bool symbol(const char* name, uintptr_t* out) {
    if (api->symbol(str(name), out) == KILN_OK && *out)
        return true;
    detail = std::string("symbol missing: ") + name;
    return false;
}
bool constant(const char* name, int64_t* out) {
    if (api->constant(str(name), out) == KILN_OK)
        return true;
    detail = std::string("constant missing: ") + name;
    return false;
}
template <class F> bool function(const char* name, F* out) {
    uintptr_t at = 0;
    if (!symbol(name, &at))
        return false;
    *out = reinterpret_cast<F>(at);
    return true;
}
// The module that holds an address (its file name, never the path, and the offset), or an empty name (a hook's relay or
// stub memory, or nothing mapped).
ModulePlace module_place(uintptr_t at) {
    HMODULE m = nullptr;
    wchar_t path[MAX_PATH]{};
    if (!at ||
        !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(at), &m) ||
        !GetModuleFileNameW(m, path, MAX_PATH))
        return {};
    const wchar_t* name = path;
    for (const wchar_t* c = path; *c; ++c)
        if (*c == L'\\' || *c == L'/')
            name = c + 1;
    char text[MAX_PATH * 3]{};
    if (!WideCharToMultiByte(CP_UTF8, 0, name, -1, text, sizeof text, nullptr, nullptr) || !text[0])
        return {};
    return {text, static_cast<uint64_t>(at - reinterpret_cast<uintptr_t>(m))};
}
// Live code Kiln may chain a hook on: the exe file's, or changed only by Kiln's own hook registry (another Kiln owner's
// detour, which the new link joins). Otherwise detail says why, where another module's jump leads (through its relay)
// and what to do, with the bytes kiln.hooks saw.
bool qualified(uintptr_t at, const char* name) {
    const auto* hooks = kiln::hooks_v2(api);
    const auto v = code_verdict(hooks, at);
    if (v.ok)
        return true;
    uintptr_t to = 0;
    ModulePlace where;
    if (v.jump) {
        auto read = [hooks](uintptr_t a, void* out, size_t n) { return hooks->read && hooks->read(a, out, n) == KILN_OK; };
        to = follow_jumps(v.jump, read, &module_place);
        where = module_place(to);
    }
    detail = code_refusal(name, v, to, where);
    return false;
}
bool field(const char* type, const char* name, uint32_t* out, uint32_t wanted) {
    if (!types_api)
        return false;
    auto t = types_api->type_by_name(str(type));
    KilnTypeInfo info{};
    info.size = sizeof info;
    if (!t || types_api->type_info(t, &info) != KILN_OK)
        return false;
    for (uint32_t i = 0; i < info.field_count; ++i) {
        KilnTypeField f{};
        if (types_api->type_field(t, i, &f) != KILN_OK || !same(f.name, name))
            continue;
        KilnTypeInfo v{};
        v.size = sizeof v;
        if (types_api->type_info(f.type, &v) != KILN_OK || v.type_size != wanted || f.offset > info.type_size ||
            wanted > info.type_size - f.offset)
            return false;
        *out = static_cast<uint32_t>(f.offset);
        return true;
    }
    return false;
}
bool identity(uint32_t slot, uint64_t* account, std::array<uint8_t, 16>* id) {
    if (!players_api || slot >= 16)
        return false;
    KilnPlayersStatus st{};
    st.size = sizeof st;
    if (players_api->status(&st) != KILN_OK || !st.records || !st.server_players)
        return false;
    auto p = reinterpret_cast<const uint8_t*>(st.server_players) + slot * player_stride;
    return load(p, account_offset, account) && storage_copy(id->data(), p + id_offset, 16);
}
Player* player_entity(uint32_t entity) {
    for (auto& [s, p] : model.players)
        if (p.witness.player == entity && !(p.witness.flags & KIS_WITNESS_LEFT))
            return &p;
    return nullptr;
}
// A slot's player as long as it is the same join (and, when `live`, has not left). A hint releases the model mutex,
// so Post re-finds its player, holder and registration after every hint instead of keeping iterators across it.
Player* joined(uint32_t slot, uint64_t join, bool live = true) {
    auto i = model.players.find(slot);
    if (i == model.players.end() || i->second.witness.join != join ||
        (live && (i->second.witness.flags & KIS_WITNESS_LEFT)))
        return nullptr;
    return &i->second;
}
Entry entry_for(const std::string& key, uint32_t kind, uint32_t slots, uint32_t why = 0) {
    Entry e;
    e.row.size = sizeof e.row;
    e.row.kind = kind;
    e.row.slots = slots;
    e.row.reason = why;
    std::snprintf(e.row.key, sizeof e.row.key, "%s", key.c_str());
    std::fill(std::begin(e.row.refs), std::end(e.row.refs), KIS_REF_NONE);
    return e;
}
bool garbage_contains(uint32_t entity) {
    for (const auto& h : garbage)
        if (h.entity == entity || std::find(h.pages.begin(), h.pages.end(), entity) != h.pages.end())
            return true;
    return false;
}
bool contains(uint32_t entity) {
    std::lock_guard lock(storage_mu);
    return model.entity_owner(entity) != 0 || garbage_contains(entity);
}
bool holder_kind(uint32_t entity) {
    std::lock_guard lock(storage_mu);
    if (model.is_holder(entity))
        return true;
    for (const auto& h : garbage)
        if (h.entity == entity)
            return true;
    return false;
}
bool mutable_storage(uint32_t entity) {
    std::lock_guard lock(storage_mu);
    return !contains(entity) || (!garbage_contains(entity) && enabled && model.bound());
}
bool actor(uint32_t entity, uint32_t player) {
    std::lock_guard lock(storage_mu);
    return model.actor_allowed(entity, player);
}
bool accepts(uint32_t entity, uint32_t item, bool pide, uint64_t slot) {
    std::lock_guard lock(storage_mu);
    for (const auto& [s, p] : model.players)
        for (const auto& [key, h] : p.holders) {
            bool root = h.entity == entity;
            auto page = std::find(h.pages.begin(), h.pages.end(), entity);
            if (!root && page == h.pages.end())
                continue;
            auto r = model.registrations.find(key);
            if (r == model.registrations.end())
                return false;
            ItemTraits info{};
            if (!item_traits(structural_access.items, item, &info))
                return false;
            if (slot) {
                auto it = std::find(h.pages.begin(), h.pages.end(), KILN_SLOT_INVENTORY(slot));
                if (it == h.pages.end() || size_t(it - h.pages.begin()) * 8 + KILN_SLOT_INDEX(slot) >= r->second.slots)
                    return false;
            }
            return r->second.filter.allows(item, info.category, info.equip, pide || info.pide);
        }
    return true;
}
uint32_t root_of(uint32_t entity) {
    std::lock_guard lock(storage_mu);
    for (const auto& [s, p] : model.players)
        for (const auto& [key, h] : p.holders)
            if (h.entity == entity || std::find(h.pages.begin(), h.pages.end(), entity) != h.pages.end())
                return h.entity;
    return 0;
}
const StorageGuards guards{&contains, &holder_kind, &accepts, &actor, &root_of, &mutable_storage};
bool build_bindings(const Access& a) {
    void* registry = nullptr;
    if (!load(a.storage, 0, &registry) || !registry)
        return false;
    const char* names[]{"ColorPaletteItem", "Durability",       "Fuel",         "Level",
                        "PerkContainer",    "PerkContainerNew", "WeaponGemSlot"};
    for (size_t i = 0; i < 7; ++i) {
        uint32_t hash = hash_name(names[i]);
        uint16_t index = 0;
        __try {
            auto ptr = E.component_map_find(static_cast<uint8_t*>(registry) + E.registry_components, &hash);
            if (!ptr)
                return false;
            index = *ptr;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
        if (binding_offsets[i] < 0 || binding_offsets[i] + 2 > bindings.size())
            return false;
        std::memcpy(bindings.data() + binding_offsets[i], &index, 2);
    }
    return true;
}
std::vector<Full> group_entries(std::vector<Entry>*);
Return* return_for(const std::string& key) {
    for (auto& q : returns)
        if (q.internal_key == key)
            return &q;
    return nullptr;
}
bool freeze_return(Return& q, void* attrs, void* bind, EntityRef* entity, Entry* out) {
    // the records keep the reason they were parked with (a held bag stays held while it is being returned)
    *out = entry_for(q.key, KIS_ENTRY_PARKED, std::max(q.original_slots, 64u), q.reason);
    if (!freeze_holder(q.holder, attrs, bind, entity, out))
        return false;
    for (auto& r : out->records) {
        if (r.slot >= q.original_indices.size())
            return false;
        r.slot = q.original_indices[r.slot];
        r.data[0] = static_cast<uint8_t>(r.slot);
    }
    return true;
}
bool freeze(Player& p, void* attrs, void* bind, EntityRef* entity, std::vector<Entry>* out) {
    *out = p.entries;
    for (auto& [key, h] : p.holders) {
        auto r = model.registrations.find(key);
        Entry e;
        if (r == model.registrations.end()) {
            auto* q = return_for(key);
            if (q) {
                if (!freeze_return(*q, attrs, bind, entity, &e))
                    return false;
            } else {
                e = entry_for(key, KIS_ENTRY_PARKED, h.slots, KIS_PARK_UNREGISTERED);
                if (!freeze_holder(h, attrs, bind, entity, &e))
                    return false;
            }
        } else {
            e = entry_for(key, KIS_ENTRY_LIVE, r->second.slots);
            if (!freeze_holder(h, attrs, bind, entity, &e))
                return false;
        }
        h.stacks = static_cast<uint32_t>(e.records.size());
        std::copy(std::begin(e.row.refs), std::end(e.row.refs), h.refs.begin());
        out->push_back(std::move(e));
    }
    group_entries(out);
    return true;
}
void begin_save() {
    if (model.active_tag())
        return;
    if (model.drop_complete())
        dropped_capture_batch = true;
    KilnSaveEvent ev{};
    ev.size = sizeof ev;
    uint64_t seq = 0;
    uint32_t rev = 0;
    uint8_t guid[16]{};
    if (saves_api)
        while (saves_api->next_event(save_cursor, &ev) == KILN_OK) {
            save_cursor = ev.seq;
            if (ev.kind == KILN_SAVE_EVENT_SAVING) {
                seq = ev.seq;
                rev = ev.revision;
                std::memcpy(guid, ev.world_guid, 16);
            }
        }
    auto tag = model.begin_batch(seq, rev, step);
    FILETIME time{};
    GetSystemTimeAsFileTime(&time);
    ULARGE_INTEGER ticks{};
    ticks.LowPart = time.dwLowDateTime;
    ticks.HighPart = time.dwHighDateTime;
    model.metadata(tag, static_cast<int64_t>(ticks.QuadPart / 10000ull - 11644473600000ull), guid);
}
using CaptureFn = Result8*(__fastcall*)(Result8*, void*, void*, void*, void*, void*, EntityRef*);
Result8* __fastcall capture(Result8* out, void* character, void* opaque3, void* attrs, void* bind, void* opaque6,
                            EntityRef* ref) {
    auto ret = capture_hook.call<Result8*>(out, character, opaque3, attrs, bind, opaque6, ref);
    Result8 result{};
    EntityRef entity{};
    if (!storage_copy(&result, out, sizeof result) || result.error || !storage_copy(&entity, ref, sizeof entity) ||
        entity.id < 1 || entity.id > 16)
        return ret;
    std::lock_guard lock(storage_mu);
    if (!enabled)
        return ret;
    uint32_t slot = entity.id - 1;
    auto p = model.players.find(slot);
    if (p == model.players.end() || (p->second.witness.flags & KIS_WITNESS_LEFT))
        return ret; // left (or never read by this Kiln): no new pair for this slot
    uint64_t w = 0;
    std::vector<Entry> entries;
    bool ok = character_witness(character, &w) &&
              (!p->second.restored || p->second.locked || freeze(p->second, attrs, bind, &entity, &entries));
    p->second.witness.step = step;
    begin_save();
    model.capture(slot, w, entries, !ok, KILN_CAP_REASON_FAULTED);
    ++capture_count;
    if (!ok)
        refused_capture_slots |= 1u << slot;
    return ret;
}
// After the game read a character inside JoinApply. The slot's previous character never survives the read: its
// holders go and it is marked left, so a new occupant can neither use its storage nor pair a capture with it.
void after_read(int slot, const void* character) {
    uint64_t w = 0, account = 0;
    uint32_t witness_flags = 0;
    std::array<uint8_t, 16> id{};
    const bool parsed = character_witness(character, &w, main_baseline, &witness_flags);
    const bool known = parsed && identity(static_cast<uint32_t>(slot), &account, &id);
    std::lock_guard lock(storage_mu);
    auto old = model.players.find(static_cast<uint32_t>(slot));
    if (old != model.players.end()) {
        for (const auto& [k, h] : old->second.holders)
            garbage.push_back(h);
        if ((!parsed || !known) && !(old->second.witness.flags & KIS_WITNESS_LEFT))
            model.leave(static_cast<uint32_t>(slot));
    }
    if (!parsed) {
        off(KILN_CAP_REASON_SYMBOL_MISSING,
            "items.storage: a character read at a join does not match characters/struct.* (inventory layout)");
        return;
    }
    if (!known) {
        items_log(KILN_LOG_ERROR, "player-storage: no account or character id for slot %d; storage stays inactive "
                                  "for this character", slot);
        report("items.storage.persistence_health", KILN_CAP_UNAVAILABLE, KILN_CAP_REASON_FAULTED,
               "a joining character has no account or character id");
        return;
    }
    model.join(static_cast<uint32_t>(slot), account, id, w, step, witness_flags);
}
Result8* __fastcall read_character(Result8* out, void* character, void* reader, void* context) {
    auto ret = read_hook.call<Result8*>(out, character, reader, context);
    Result8 r{};
    if (read_slot >= 0 && storage_copy(&r, out, sizeof r) && !r.error)
        after_read(read_slot, character);
    return ret;
}
Result8* __fastcall join_apply(Result8* out, void* blob, void* server, uint32_t slot, void* setup) {
    int previous = read_slot;
    read_slot = slot < 16 ? static_cast<int>(slot) : -1;
    auto ret = join_hook.call<Result8*>(out, blob, server, slot, setup);
    read_slot = previous;
    Result8 r{};
    if (slot < 16 && storage_copy(&r, out, sizeof r) && !r.error) {
        std::lock_guard lock(storage_mu);
        model.applied(slot, slot + 1);
    }
    return ret;
}
void capture_diagnostics() {
    bool dropped = false;
    uint32_t refused = 0;
    {
        std::lock_guard lock(storage_mu);
        dropped = dropped_capture_batch;
        refused = refused_capture_slots;
        dropped_capture_batch = false;
        refused_capture_slots = 0;
    }
    if (dropped) {
        items_log(KILN_LOG_WARN, "player-storage: dropping an untaken completed batch before the next capture");
        report("items.storage.persistence_health", KILN_CAP_UNAVAILABLE, KILN_CAP_REASON_FAULTED,
               "previous capture batch was not taken");
    }
    if (refused) {
        items_log(KILN_LOG_ERROR, "player-storage: refused freeze for slot mask 0x%x; no new pair staged", refused);
        report("items.storage.persistence_health", KILN_CAP_UNAVAILABLE, KILN_CAP_REASON_FAULTED,
               "one or more storage captures were refused");
    }
}
bool call_stage(KilnStorageStageFn fn, void* user, uint32_t kind = KIS_EVENT_BATCH) {
    __try {
        fn(kind, user);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
// Caller owns callback_mu before storage_mu. Never enter managed code holding the model mutex.
bool housekeeping(uint32_t kind, std::unique_lock<std::recursive_mutex>& lock) {
    if (!model.bound() || !stage_fn)
        return true;
    auto fn = stage_fn;
    auto user = stage_user;
    lock.unlock();
    bool ok = call_stage(fn, user, kind);
    lock.lock();
    if (!ok) {
        stage_fn = nullptr;
        stage_user = nullptr;
        off(KILN_CAP_REASON_FAULTED, "items.storage: persistence housekeeping faulted; holder retained");
    }
    return ok;
}
void __fastcall captures_done(void* save2, void* storage, void* context) {
    stage_hook.call(save2, storage, context);
    std::lock_guard callback_lock(callback_mu);
    KilnStorageStageFn fn = nullptr;
    void* user = nullptr;
    std::vector<std::pair<uint32_t, uint64_t>> left;
    {
        std::lock_guard lock(storage_mu);
        if (!enabled)
            return;
        if (!model.active_tag())
            begin_save();
        auto tag = model.active_tag();
        for (auto& [slot, p] : model.players)
            if (p.leaving && model.flag_leaving(tag, slot, p.witness.join))
                left.emplace_back(slot, p.witness.join);
        model.complete();
        if (model.bound()) {
            fn = stage_fn;
            user = stage_user;
        }
    }
    capture_diagnostics();
    bool stage_ok = fn && call_stage(fn, user);
    if (fn && !stage_ok) {
        std::lock_guard lock(storage_mu);
        stage_fn = nullptr;
        stage_user = nullptr;
        items_log(KILN_LOG_ERROR, "player-storage: the persistence stage faulted; batch remains available");
        report("items.storage.persistence_health", KILN_CAP_UNAVAILABLE, KILN_CAP_REASON_FAULTED,
               "captures-done callback faulted");
    }
    if (stage_ok)
        report("items.storage.persistence_health", KILN_CAP_AVAILABLE, KILN_CAP_REASON_NONE,
               "capture stage delivered; durability is reported by kiln.saves");
    {
        std::lock_guard lock(storage_mu);
        for (auto [slot, join] : left) {
            auto i = model.players.find(slot);
            if (i == model.players.end() || i->second.witness.join != join)
                continue;
            for (const auto& [key, h] : i->second.holders)
                garbage.push_back(h);
            model.leave(slot);
        }
    }
}
bool get_component_pair(uintptr_t world, uint32_t player, PtrSize* pi, PtrSize* used) {
    __try {
        E.get_component(pi, world, hash_name("PlayerInput"), player);
        E.get_component(used, world, hash_name("ServerConsumedPlayerInput"), player);
        return pi->ptr && used->ptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
std::set<uint32_t> unreadable_pages;
// The guard never stops guarding: a fault turns storage off once (no more moves or captures) and the scan goes on.
void guard_fault(const char* text) {
    if (enabled)
        off(KILN_CAP_REASON_FAULTED, text);
}
void guard_client_inputs(uintptr_t storage) {
    std::lock_guard lock(storage_mu);
    if (!api->on_tick_thread() || storage != api->server_world() + E.world_storage)
        return;
    std::set<uint32_t> entities;
    for (const auto& h : garbage) {
        entities.insert(h.entity);
        for (auto page : h.pages)
            entities.insert(page);
    }
    for (const auto& [slot, p] : model.players)
        for (const auto& [key, h] : p.holders) {
            entities.insert(h.entity);
            for (auto page : h.pages) {
                entities.insert(page);
                uint32_t inv[24]{};
                // A page the game already removed (a leaving player's owned entities) has no items to add; its id and
                // the holder's stay refused, and every other player stays guarded.
                if (!component_copy(api->server_world(), kHashInventory, page, inv, 96)) {
                    if (unreadable_pages.insert(page).second)
                        items_log(KILN_LOG_WARN, "player-storage: client guard cannot read storage page %u", page);
                    continue;
                }
                for (unsigned i = 0; i < 8; ++i)
                    if (inv[i * 3 + 2] && inv[i * 3 + 2] != UINT32_MAX)
                        entities.insert(inv[i * 3 + 2]);
            }
        }
    if (entities.empty())
        return;
    std::vector<uint8_t> input(input_size), used(consumed_size);
    for (uint32_t player = 1; player <= 16; ++player) {
        PtrSize pi{}, scpi{};
        if (!get_component_pair(api->server_world(), player, &pi, &scpi))
            continue;
        if (pi.size < input_size || scpi.size < consumed_size || !storage_copy(input.data(), pi.ptr, input.size()) ||
            !storage_copy(used.data(), scpi.ptr, used.size())) {
            guard_fault("items.storage: client input guard read failed");
            continue;
        }
        for (const auto& a : client_actions) {
            uint32_t version = 0, consumed = 0;
            std::memcpy(&version, input.data() + a.version, 4);
            std::memcpy(&consumed, used.data() + a.consumed, 4);
            if (version == consumed)
                continue;
            if (forbidden_action(input.data() + a.offset, a, [&](uint32_t e) { return entities.count(e) != 0; })) {
                if (!storage_copy(static_cast<uint8_t*>(scpi.ptr) + a.consumed, &version, 4)) {
                    guard_fault("items.storage: client action cancellation failed");
                    continue;
                }
                items_log(KILN_LOG_WARN, "player-storage: refused client inventory action by player %u (action +%u)",
                          player, a.offset);
            }
        }
    }
}
uintptr_t __fastcall client_guard(uintptr_t storage, uintptr_t rdx, uintptr_t r8, uintptr_t r9) {
    using Deliver = uintptr_t(__fastcall*)(uintptr_t, uintptr_t, uintptr_t, uintptr_t);
    auto result = reinterpret_cast<Deliver>(transfer_next)(storage, rdx, r8, r9);
    guard_client_inputs(storage);
    return result;
}
KilnResult public_state(KilnOwner owner, uint32_t handle, uint32_t entity, KilnStorageState* out) {
    if (!out || out->size < sizeof *out)
        return KILN_ERR_INVALID_ARGUMENT;
    auto r = model.registration(owner, handle);
    if (!r)
        return KILN_ERR_NOT_FOUND;
    *out = {};
    out->size = sizeof *out;
    std::snprintf(out->key, sizeof out->key, "%s", r->key.c_str());
    out->handle = r->handle;
    out->slots = r->slots;
    out->death = r->death;
    out->status = KIS_STATE_WAITING;
    std::fill(std::begin(out->refs), std::end(out->refs), KIS_REF_NONE);
    auto p = player_entity(entity);
    if (!p)
        return KILN_OK;
    out->source = p->source;
    out->paired = p->paired;
    out->revision = p->revision;
    if (p->locked)
        out->status = KIS_STATE_LOCKED;
    auto h = p->holders.find(r->key);
    if (h != p->holders.end()) {
        out->holder = h->second.entity;
        out->pages = static_cast<uint32_t>(h->second.pages.size());
        out->stacks = h->second.stacks;
        out->status = KIS_STATE_READY;
        std::copy(h->second.refs.begin(), h->second.refs.end(), out->refs);
    }
    for (const auto& e : p->entries)
        if (r->key == e.row.key) {
            if (e.row.kind == KIS_ENTRY_QUARANTINE)
                out->quarantined += static_cast<uint32_t>(e.records.size());
            else if (e.row.kind == KIS_ENTRY_PARKED)
                out->parked += static_cast<uint32_t>(e.records.size());
        }
    if (out->parked && h == p->holders.end())
        out->status = KIS_STATE_PARKED;
    return KILN_OK;
}
KilnResult KILN_CALL register_storage(KilnOwner owner, const KilnStorageDef* d, uint32_t* out) {
    if (!d)
        return KILN_ERR_INVALID_ARGUMENT;
    std::lock_guard lock(storage_mu);
    return model.register_def(owner, *d, out);
}
KilnResult KILN_CALL holder(KilnOwner owner, uint32_t handle, uint32_t player, uint32_t* out) {
    if (!out)
        return KILN_ERR_INVALID_ARGUMENT;
    std::lock_guard lock(storage_mu);
    *out = model.holder(owner, handle, player);
    return *out ? KILN_OK : KILN_ERR_NOT_FOUND;
}
KilnResult KILN_CALL state(KilnOwner owner, uint32_t handle, uint32_t player, KilnStorageState* out) {
    std::lock_guard lock(storage_mu);
    return public_state(owner, handle, player, out);
}
KilnResult KILN_CALL set_ref(KilnOwner owner, uint32_t handle, uint32_t player, uint32_t equip, uint32_t index) {
    std::lock_guard lock(storage_mu);
    auto r = model.registration(owner, handle);
    if (!r || !model.holder(owner, handle, player))
        return KILN_ERR_NOT_FOUND;
    if ((equip != 12 && equip != 14 && equip != 15 && equip != 16) || (index != KIS_REF_CLEAR && index >= r->slots))
        return KILN_ERR_INVALID_ARGUMENT;
    auto p = player_entity(player);
    if (!p || refs.size() >= 1024)
        return KILN_ERR_UNAVAILABLE;
    refs.push_back({p->witness.slot, p->witness.join, r->key, equip, index});
    return KILN_OK;
}
KilnResult KILN_CALL status(KilnItemsStorageStatus* out) {
    if (!out || out->size < sizeof *out)
        return KILN_ERR_INVALID_ARGUMENT;
    std::lock_guard lock(storage_mu);
    *out = {};
    out->size = sizeof *out;
    out->storage = enabled;
    out->persist = model.bound();
    out->session = api && api->server_world();
    out->stage_point = stage_point;
    out->reason = enabled ? uint32_t(KILN_CAP_REASON_NONE) : reason;
    out->registrations = static_cast<uint32_t>(model.registrations.size());
    out->batches = static_cast<uint32_t>(model.batch_count());
    out->captures = capture_count;
    out->restores = restore_count;
    for (auto& [s, p] : model.players) {
        out->holders += static_cast<uint32_t>(p.holders.size());
        for (auto& e : p.entries) {
            if (e.row.kind == KIS_ENTRY_PARKED)
                out->parked += static_cast<uint32_t>(e.records.size());
            if (e.row.kind == KIS_ENTRY_QUARANTINE)
                out->quarantined += static_cast<uint32_t>(e.records.size());
        }
    }
    std::snprintf(out->detail, sizeof out->detail, "%s", detail.c_str());
    return KILN_OK;
}
KilnResult KILN_CALL list(uint32_t player, KilnStorageState* out, uint32_t cap, uint32_t* count) {
    if (!count || (cap && !out) || (cap && out[0].size < sizeof *out))
        return KILN_ERR_INVALID_ARGUMENT;
    std::lock_guard lock(storage_mu);
    auto p = player_entity(player);
    if (!p)
        return KILN_ERR_NOT_FOUND;
    std::map<std::string, KilnStorageState> rows;
    for (const auto& [key, r] : model.registrations) {
        KilnStorageState s{};
        s.size = sizeof s;
        public_state(r.owner, r.handle, player, &s);
        rows.emplace(key, s);
    }
    for (auto& e : p->entries)
        if (!rows.count(e.row.key)) {
            KilnStorageState s{};
            s.size = sizeof s;
            s.status = p->locked ? KIS_STATE_LOCKED : KIS_STATE_PARKED;
            s.source = p->source;
            s.revision = p->revision;
            s.paired = p->paired;
            std::snprintf(s.key, sizeof s.key, "%s", e.row.key);
            std::fill(std::begin(s.refs), std::end(s.refs), KIS_REF_NONE);
            rows.emplace(e.row.key, s);
        }
    for (const auto& e : p->entries) {
        auto& s = rows.at(e.row.key);
        if (!s.handle) {
            if (e.row.kind == KIS_ENTRY_QUARANTINE)
                s.quarantined += static_cast<uint32_t>(e.records.size());
            else
                s.parked += static_cast<uint32_t>(e.records.size());
        }
    }
    std::vector<KilnStorageState> all;
    for (const auto& [key, row] : rows)
        all.push_back(row);
    for (const auto& q : returns)
        if (q.slot == p->witness.slot && q.join == p->witness.join && q.holder.entity) {
            KilnStorageState temp{};
            temp.size = sizeof temp;
            temp.status = KIS_STATE_PARKED;
            temp.holder = q.holder.entity;
            temp.slots = 64;
            temp.pages = static_cast<uint32_t>(q.holder.pages.size());
            temp.source = p->source;
            temp.paired = p->paired;
            temp.revision = p->revision;
            std::snprintf(temp.key, sizeof temp.key, "%s", q.key.c_str());
            std::fill(std::begin(temp.refs), std::end(temp.refs), KIS_REF_NONE);
            all.push_back(temp);
        }
    std::stable_sort(all.begin(), all.end(),
                     [](const auto& a, const auto& b) { return std::strcmp(a.key, b.key) < 0; });
    *count = static_cast<uint32_t>(all.size());
    if (all.size() > cap)
        return KILN_ERR_BUFFER_TOO_SMALL;
    std::copy(all.begin(), all.end(), out);
    return KILN_OK;
}
KilnResult KILN_CALL next_event(uint64_t after, KilnStorageEvent* out) {
    if (!out || out->size < sizeof *out)
        return KILN_ERR_INVALID_ARGUMENT;
    std::lock_guard lock(storage_mu);
    for (const auto& e : model.events)
        if (e.seq > after) {
            *out = e;
            return KILN_OK;
        }
    return KILN_ERR_NOT_FOUND;
}
KilnResult KILN_CALL bind(KilnOwner owner, KilnStorageStageFn fn, void* user, uint64_t* last) {
    std::lock_guard callback_lock(callback_mu);
    std::lock_guard lock(storage_mu);
    if (!enabled || !stage_point)
        return KILN_ERR_UNAVAILABLE;
    auto rc = model.bind(owner);
    if (rc != KILN_OK)
        return rc;
    stage_fn = fn;
    stage_user = user;
    if (last)
        *last = model.last_event();
    return KILN_OK;
}
KilnResult KILN_CALL take(KilnOwner owner, uint64_t tag, KilnStorageFrozenBatch* out, KilnStorageCapture* captures,
                          uint32_t cc, KilnStorageEntry* entries, uint32_t ec, KilnStorageRecord* records,
                          uint32_t rc) {
    if (!out || out->size < sizeof *out || (cc && !captures) || (ec && !entries) || (rc && !records))
        return KILN_ERR_INVALID_ARGUMENT;
    std::lock_guard lock(storage_mu);
    if (!model.owns_persist(owner))
        return KILN_ERR_UNAVAILABLE;
    Frozen f;
    auto result = model.take(tag, &f, false);
    if (result != KILN_OK)
        return result;
    *out = f.header;
    if (cc < f.captures.size() || ec < f.entries.size() || rc < f.records.size())
        return KILN_ERR_BUFFER_TOO_SMALL;
    std::copy(f.captures.begin(), f.captures.end(), captures);
    std::copy(f.entries.begin(), f.entries.end(), entries);
    std::copy(f.records.begin(), f.records.end(), records);
    model.mark_taken(tag);
    return KILN_OK;
}
KilnResult KILN_CALL release(KilnOwner owner, uint64_t tag) {
    std::lock_guard lock(storage_mu);
    return model.owns_persist(owner) ? model.release(tag) : KILN_ERR_UNAVAILABLE;
}
KilnResult KILN_CALL witness_of(KilnOwner owner, uint32_t slot, KilnStorageWitness* out) {
    if (!out || out->size < sizeof *out)
        return KILN_ERR_INVALID_ARGUMENT;
    std::lock_guard lock(storage_mu);
    if (!model.owns_persist(owner))
        return KILN_ERR_UNAVAILABLE;
    auto i = model.players.find(slot);
    if (i == model.players.end())
        return KILN_ERR_NOT_FOUND;
    *out = i->second.witness;
    return KILN_OK;
}
KilnResult KILN_CALL restore(KilnOwner owner, uint32_t slot, uint64_t join, const KilnStorageDoc* doc) {
    if (!doc)
        return KILN_ERR_INVALID_ARGUMENT;
    std::lock_guard lock(storage_mu);
    if (!model.owns_persist(owner))
        return KILN_ERR_UNAVAILABLE;
    auto result = model.restore(slot, join, *doc);
    if (result == KILN_OK)
        ++restore_count;
    return result;
}
KilnResult queue_park(KilnOwner owner, uint32_t entity, KilnStr key, uint32_t why, bool unpark) {
    if (!key.ptr || key.len >= KIS_KEY_MAX || !key_ok(std::string(key.ptr, key.len)) ||
        (!unpark && why < KIS_PARK_HELD))
        return KILN_ERR_INVALID_ARGUMENT;
    std::lock_guard lock(storage_mu);
    if (!model.owns_persist(owner))
        return KILN_ERR_UNAVAILABLE;
    auto p = player_entity(entity);
    if (!p)
        return KILN_ERR_NOT_FOUND;
    std::string k(key.ptr, key.len);
    bool has = p->holders.count(k);
    for (auto& e : p->entries)
        has = has || k == e.row.key;
    if (!has)
        return KILN_ERR_NOT_FOUND;
    if (unpark && (!p->restored || !model.registrations.count(k)))
        return KILN_ERR_UNAVAILABLE;
    if (parks.size() >= 1024)
        return KILN_ERR_UNAVAILABLE;
    parks.push_back({p->witness.slot, p->witness.join, k, why, unpark});
    return KILN_OK;
}
KilnResult KILN_CALL park(KilnOwner owner, uint32_t player, KilnStr key, uint32_t why) {
    return queue_park(owner, player, key, why, false);
}
KilnResult KILN_CALL unpark(KilnOwner owner, uint32_t player, KilnStr key) {
    return queue_park(owner, player, key, 0, true);
}
KilnResult KILN_CALL return_parked(KilnOwner owner, uint32_t player, KilnStr key, uint32_t target,
                                   KilnStorageReturnFn done, void* user, uint64_t* ticket) {
    if (ticket)
        *ticket = 0;
    if (!ticket || !key.ptr || !key.len || key.len >= KIS_KEY_MAX || !key_ok(std::string(key.ptr, key.len)) || !target)
        return KILN_ERR_INVALID_ARGUMENT;
    std::lock_guard lock(storage_mu);
    if (model.retired.count(owner))
        return KILN_ERR_REVOKED;
    auto* p = player_entity(player);
    if (!p)
        return KILN_ERR_NOT_FOUND;
    if (!enabled || !model.bound() || !p->restored || p->locked)
        return KILN_ERR_UNAVAILABLE;
    if (contains(target) || (target <= 16 && target != player))
        return KILN_ERR_INVALID_ARGUMENT;
    for (auto& q : returns)
        if (q.slot == p->witness.slot)
            return KILN_ERR_UNAVAILABLE;
    uint32_t requested = 0;
    bool found = false;
    for (auto& e : p->entries)
        if (std::string(key.ptr, key.len) == e.row.key && e.row.kind == KIS_ENTRY_PARKED) {
            found = true;
            for (auto& r : e.records)
                requested += r.count;
        }
    if (!found)
        return KILN_ERR_NOT_FOUND;
    Return q;
    q.owner = owner;
    q.ticket = next_return++;
    q.join = p->witness.join;
    q.submitted = step;
    q.slot = p->witness.slot;
    q.target = target;
    q.key.assign(key.ptr, key.len);
    q.internal_key = "~return/" + std::to_string(q.ticket);
    q.done = done;
    q.user = user;
    q.result.size = sizeof q.result;
    q.result.ticket = q.ticket;
    q.result.player = player;
    q.result.target = target;
    q.result.requested = requested;
    *ticket = q.ticket;
    returns.push_back(std::move(q));
    return KILN_OK;
}
const KilnItemsStorageApi table{{sizeof table, 1},
                                &register_storage,
                                &holder,
                                &state,
                                &set_ref,
                                &status,
                                &list,
                                &next_event,
                                &return_parked,
                                &bind,
                                &take,
                                &release,
                                &witness_of,
                                &restore,
                                &park,
                                &unpark};
bool slot_free(const Holder& h, uint32_t index) {
    if (index >= h.pages.size() * 8)
        return false;
    uint32_t slots[24]{};
    return component_copy(api->server_world(), kHashInventory, h.pages[index / 8], slots, 96) &&
           slots[(index % 8) * 3] == 0 && slots[(index % 8) * 3 + 1] == 0 && slots[(index % 8) * 3 + 2] == 0;
}
std::vector<Full> group_entries(std::vector<Entry>* entries) {
    auto full = bound_parked(entries);
    std::vector<Entry> out;
    for (auto& e : *entries) {
        auto found = std::find_if(out.begin(), out.end(), [&](const Entry& v) {
            return v.row.kind == e.row.kind && v.row.reason == e.row.reason && std::strcmp(v.row.key, e.row.key) == 0;
        });
        if (found == out.end())
            out.push_back(std::move(e));
        else {
            found->row.slots = std::max(found->row.slots, e.row.slots);
            found->records.insert(found->records.end(), e.records.begin(), e.records.end());
        }
    }
    for (auto& e : out) {
        e.row.first = 0;
        e.row.count = static_cast<uint32_t>(e.records.size());
    }
    *entries = std::move(out);
    return full;
}
// Groups a player's entries under the per-key parked bound; records that no longer fit are quarantined (event).
void settle(Player& p) {
    for (const auto& f : group_entries(&p.entries)) {
        model.event(KIS_EVENT_QUARANTINED, &p, f.key, 0, KIS_QUARANTINE_PARKED_FULL);
        model.events.back().stacks = f.stacks;
        model.events.back().items = f.items;
        items_log(KILN_LOG_WARN, "player-storage: %s: parked storage full, %u stacks quarantined (slot %u)",
                  f.key.c_str(), f.stacks, p.witness.slot);
    }
}
uint32_t parked_reason(const Player& p, const std::string& key, uint32_t reason = 0) {
    for (const auto& e : p.entries)
        if (e.row.kind == KIS_ENTRY_PARKED && key == e.row.key)
            reason = std::max(reason, e.row.reason);
    return reason;
}
void finish_return(Return& q, Player* p, uint32_t status_code, const char* message) {
    q.result.status = status_code;
    q.result.steps = static_cast<uint32_t>(step - q.submitted);
    std::snprintf(q.result.message, sizeof q.result.message, "%s", message);
    if (p) {
        q.result.kept = 0;
        for (auto& e : p->entries)
            if (q.key == e.row.key && e.row.kind == KIS_ENTRY_PARKED)
                q.result.kept += static_cast<uint32_t>(e.records.size());
        model.event(KIS_EVENT_RETURNED, p, q.key);
        auto& e = model.events.back();
        e.target = q.target;
        e.items = q.result.done;
        e.stacks = q.result.stacks;
    }
    returned.push_back({q.owner, q.done, q.user, q.result});
    q.phase = 3;
}
void return_post(const Access& a, std::unique_lock<std::recursive_mutex>& lock) {
    // By index: return_parked may queue another return while a hint runs (push_back keeps element references).
    for (size_t n = 0; n < returns.size(); ++n) {
        auto& q = returns[n];
        Player* found = joined(q.slot, q.join);
        if (!found) {
            q.holder = {};
            finish_return(q, nullptr, KILN_ITEMS_DROPPED, "the player left");
            continue;
        }
        if (q.phase == 2) {
            Entry kept;
            EntityRef ref{found->witness.player, 0, 0, 0, a.storage, nullptr};
            if (!freeze_return(q, a.attributes, bindings.data(), &ref, &kept)) {
                off(KILN_CAP_REASON_FAULTED, "items.storage: parked-return freeze failed");
                return;
            }
            model.event(KIS_EVENT_PARKED, found, q.key, 0, q.reason);
            model.events.back().handle = 0;
            model.events.back().target = q.holder.entity;
            model.events.back().stacks = static_cast<uint32_t>(kept.records.size());
            if (!housekeeping(KIS_STAGE_PARKED, lock))
                return;
            found = joined(q.slot, q.join);
            if (!found) {
                q.holder = {};
                finish_return(q, nullptr, KILN_ITEMS_DROPPED, "the player left");
                continue;
            }
            auto& p = *found;
            if (!kept.records.empty())
                p.entries.push_back(std::move(kept));
            if (!destroy_holder(a, &q.holder)) {
                off(KILN_CAP_REASON_FAULTED, "items.storage: parked-return cleanup failed");
                return;
            }
            p.holders.erase(q.internal_key);
            q.holder = {};
            settle(p);
            if (q.all_moved && q.result.done < q.result.requested && step - q.submitted < 250)
                q.phase = 0;
            else {
                finish_return(q, &p,
                              q.result.done == q.result.requested ? KILN_ITEMS_DONE
                              : q.result.done                     ? KILN_ITEMS_PARTIAL
                                                                  : KILN_ITEMS_NO_ROOM,
                              "parked items returned as far as the target has room");
                continue;
            }
        }
        if (q.phase != 0)
            continue;
        auto& p = *found;
        if (!create_holder(a, p.witness.player, 64, &q.holder)) {
            finish_return(q, &p, KILN_ITEMS_UNAVAILABLE, "cannot create a temporary return inventory");
            continue;
        }
        uint32_t inserted = 0;
        std::vector<Entry> remaining;
        for (auto& e : p.entries) {
            if (q.key != e.row.key || e.row.kind != KIS_ENTRY_PARKED) {
                remaining.push_back(std::move(e));
                continue;
            }
            Entry keep = e;
            keep.records.clear();
            q.original_slots = std::max(q.original_slots, e.row.slots);
            q.reason = std::max(q.reason, e.row.reason);
            for (auto record : e.records) {
                if (inserted >= 64) {
                    keep.records.push_back(record);
                    continue;
                }
                auto temp = record;
                temp.slot = static_cast<uint16_t>(inserted);
                temp.data[0] = static_cast<uint8_t>(inserted);
                uint32_t why = 0;
                if (insert_record(a, q.holder, temp, &why)) {
                    q.original_indices[inserted] = record.slot;
                    ++inserted;
                } else
                    keep.records.push_back(record);
            }
            if (!keep.records.empty())
                remaining.push_back(std::move(keep));
        }
        p.entries = std::move(remaining);
        if (!inserted) {
            destroy_holder(a, &q.holder);
            q.holder = {};
            finish_return(q, &p, q.result.done ? KILN_ITEMS_PARTIAL : KILN_ITEMS_NO_ROOM,
                          "remaining parked records cannot be rebuilt");
            continue;
        }
        p.holders.emplace(q.internal_key, q.holder);
        q.phase = 1;
        model.event(KIS_EVENT_RESTORED, &p, q.key);
        model.events.back().handle = 0;
        model.events.back().target = q.holder.entity;
        model.events.back().stacks = inserted;
        model.events.back().flags |= KIS_EVENT_LIVE;
        if (!housekeeping(KIS_STAGE_RESTORED, lock))
            return;
    }
    returns.erase(std::remove_if(returns.begin(), returns.end(), [](const Return& q) { return q.phase == 3; }),
                  returns.end());
}
void return_moves(const Access& a) {
    for (auto& q : returns) {
        if (q.phase != 1)
            continue;
        auto i = model.players.find(q.slot);
        if (i == model.players.end() || i->second.witness.join != q.join ||
            (i->second.witness.flags & KIS_WITNESS_LEFT))
            continue;
        PageSet target{}, source{};
        bool initialized = false;
        if (!read_pages(a, q.target, &target, &initialized) || !initialized) {
            if (step - q.submitted < 250)
                continue;
            q.phase = 2;
            continue;
        }
        if (!read_pages(a, q.holder.entity, &source, &initialized) || !initialized) {
            q.phase = 2;
            continue;
        }
        auto* tx = static_cast<uint8_t*>(_aligned_malloc(static_cast<size_t>(E.tx_size), 16));
        if (!tx) {
            off(KILN_CAP_REASON_FAULTED, "items.storage: cannot allocate return transaction");
            return;
        }
        q.all_moved = true;
        for (uint32_t page = 0; page < source.n; ++page)
            for (const auto& stack : source.p[page].s) {
                if (is_empty(stack))
                    continue;
                MoveJob job{};
                job.r.size = sizeof job.r;
                job.r.kind = KILN_MOVE_STACK;
                job.r.from = q.holder.entity;
                job.r.to = q.target;
                job.r.slot = stack.slot;
                KilnItemsMoveResult result{};
                unsigned long code = 0;
                NoteLease note;
                uint32_t holders[]{q.holder.entity, q.target};
                note.begin(a, holders, 2);
                bool ok = run_move(a, job, tx, &result, &code);
                note.end(a);
                if (!ok) {
                    abandon_tx(tx);
                    _aligned_free(tx);
                    off(KILN_CAP_REASON_FAULTED, "items.storage: parked return move faulted");
                    return;
                }
                q.result.done += result.done;
                if (result.done)
                    ++q.result.stacks;
                if (result.done != stack.count)
                    q.all_moved = false;
            }
        _aligned_free(tx);
        q.phase = 2;
    }
}
bool call_return(KilnStorageReturnFn fn, const KilnStorageReturnResult* result, void* user) {
    __try {
        fn(result, user);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
void deliver_returns() {
    std::lock_guard callback_lock(callback_mu);
    std::deque<ReturnDone> done;
    {
        std::lock_guard lock(storage_mu);
        done.swap(returned);
    }
    for (auto& r : done) {
        bool live = false;
        {
            std::lock_guard lock(storage_mu);
            live = !model.retired.count(r.owner);
        }
        if (r.fn && live && !call_return(r.fn, &r.result, r.user))
            items_log(KILN_LOG_ERROR, "player-storage: parked-return callback faulted");
    }
}
bool death_wrap_ready();
bool park_holder(const Access& a, uint32_t slot, uint64_t join, const std::string& key, uint32_t why, bool retired,
                 std::unique_lock<std::recursive_mutex>& lock) {
    Player* found = joined(slot, join, false);
    if (!found)
        return true;
    auto h = found->holders.find(key);
    if (h == found->holders.end())
        return true;
    const Holder holder = h->second;
    // a park never lowers a reason: live records that join a held bag become held with it
    why = parked_reason(*found, key, why);
    Entry e = entry_for(key, KIS_ENTRY_PARKED, holder.slots, why);
    EntityRef ref{found->witness.player, 0, 0, 0, a.storage, nullptr};
    if (!freeze_holder(holder, a.attributes, bindings.data(), &ref, &e)) {
        off(KILN_CAP_REASON_FAULTED, "items.storage: park freeze failed; holder retained");
        return false;
    }
    model.event(KIS_EVENT_PARKED, found, key, 0, why);
    // Target is the old holder; a retired owner's teardown stays internal even if the key registered again since
    if (retired)
        model.events.back().handle = 0;
    model.events.back().target = holder.entity;
    model.events.back().stacks = static_cast<uint32_t>(e.records.size());
    for (const auto& r : e.records)
        model.events.back().items += r.count;
    if (!housekeeping(KIS_STAGE_PARKED, lock))
        return false;
    found = joined(slot, join, false);
    if (!found)
        return true;
    auto& p = *found;
    h = p.holders.find(key);
    if (h == p.holders.end() || h->second.entity != holder.entity)
        return true; // gone while the hint ran (a leave took it with its own capture)
    if (!clear_holder_references(api->server_world(), p.witness.player, h->second)) {
        off(KILN_CAP_REASON_FAULTED, "items.storage: cannot clear references before parking");
        return false;
    }
    p.entries.push_back(std::move(e));
    if (!destroy_holder(a, &h->second)) {
        off(KILN_CAP_REASON_FAULTED, "items.storage: park cleanup failed");
        return false;
    }
    p.holders.erase(h);
    settle(p);
    return true;
}
// The first part of a player's build pass: unregistered live copies are parked, and the first pass announces
// every parked entry (Target 0). False: nothing to build for this player in this Post.
bool begin_build(uint32_t slot, uint64_t join) {
    Player* found = joined(slot, join);
    if (!found || !model.build_ready(slot) || !model.bound())
        return false;
    auto& p = *found;
    uint8_t setup[84]{};
    if (!component_copy(api->server_world(), kHashInventorySetup, p.witness.player, setup, 84) || !setup[83])
        return false;
    for (auto& e : p.entries)
        if (!model.registrations.count(e.row.key) && e.row.kind == KIS_ENTRY_LIVE) {
            // only before the first build: its PARKED event (Target 0) is the first restore's one below
            e.row.kind = KIS_ENTRY_PARKED;
            e.row.reason = KIS_PARK_UNREGISTERED;
        }
    if (!p.build_pass_completed)
        settle(p); // a live copy of an unregistered key joins that key's parked records
    if (!p.build_pass_completed)
        for (const auto& entry : p.entries)
            if (entry.row.kind == KIS_ENTRY_PARKED) {
                model.event(KIS_EVENT_PARKED, &p, entry.row.key, 0, entry.row.reason);
                auto& event = model.events.back();
                event.stacks = static_cast<uint32_t>(entry.records.size());
                for (const auto& record : entry.records)
                    event.items += record.count;
            }
    return true;
}
void post_body() {
    std::unique_lock lock(storage_mu);
    if (!enabled || !structural_gate() || !structural_access.storage || !death_wrap_ready())
        return;
    const Access& a = structural_access;
    if (!build_bindings(a)) {
        off(KILN_CAP_REASON_SYMBOL_MISSING, "items.storage: cannot rebuild converter component bindings");
        return;
    }
    for (const auto& h : garbage)
        if (!destroy_holder(a, &h)) {
            off(KILN_CAP_REASON_FAULTED, "items.storage: holder cleanup failed");
            return;
        }
    garbage.clear();
    while (!parks.empty()) {
        auto q = std::move(parks.front());
        parks.pop_front();
        Player* found = joined(q.slot, q.join, false);
        if (!found)
            continue;
        if (q.unpark) {
            for (auto& e : found->entries)
                if (q.key == e.row.key && e.row.kind == KIS_ENTRY_PARKED)
                    e.row.reason = KIS_PARK_UNREGISTERED;
            continue;
        }
        bool had_holder = found->holders.count(q.key) != 0;
        if (!park_holder(a, q.slot, q.join, q.key, q.reason, q.retired, lock))
            return;
        found = joined(q.slot, q.join, false);
        if (!found)
            continue;
        auto& p = *found;
        // Never lower a reason: an unload must not turn a held bag into one that restores by itself.
        for (auto& e : p.entries)
            if (q.key == e.row.key && e.row.kind == KIS_ENTRY_PARKED)
                e.row.reason = std::max(e.row.reason, q.reason);
        settle(p);
        if (!had_holder && !q.retired)
            model.event(KIS_EVENT_PARKED, &p, q.key, 0, parked_reason(p, q.key, q.reason));
    }
    std::vector<std::pair<uint32_t, uint64_t>> building;
    for (const auto& [slot, p] : model.players)
        building.emplace_back(slot, p.witness.join);
    for (const auto [slot, join] : building) {
        if (!begin_build(slot, join))
            continue;
        // Keys, not map iterators: the owner of a key may retire (or a mod register) while a hint runs.
        std::vector<std::string> keys_now;
        for (const auto& [key, r] : model.registrations)
            keys_now.push_back(key);
        for (const auto& key : keys_now) {
            Player* found = joined(slot, join);
            if (!found)
                break;
            auto& p = *found;
            auto registration = model.registrations.find(key);
            if (registration == model.registrations.end())
                continue;
            const Registration& r = registration->second;
            auto h = p.holders.find(key);
            bool created_now = h == p.holders.end();
            uint32_t restored_stacks = 0, restored_items = 0;
            if (created_now) {
                std::set<std::string> keys;
                for (const auto& e : p.entries)
                    keys.insert(e.row.key);
                for (const auto& [k, v] : p.holders)
                    if (k.rfind("~return/", 0) != 0)
                        keys.insert(k);
                if (keys.size() >= KIS_MAX_STORAGES && !keys.count(key))
                    continue;
                Holder created;
                if (!create_holder(a, p.witness.player, r.slots, &created)) {
                    off(KILN_CAP_REASON_FAULTED, "items.storage: holder/page creation failed");
                    return;
                }
                model.mark_built(slot, key, created.entity, created.pages);
                h = p.holders.find(key);
            }
            std::vector<Entry> kept;
            for (auto& e : p.entries) {
                if (key != e.row.key || e.row.kind == KIS_ENTRY_QUARANTINE ||
                    (e.row.kind == KIS_ENTRY_PARKED && e.row.reason >= KIS_PARK_HELD)) {
                    kept.push_back(std::move(e));
                    continue;
                }
                for (auto record : e.records) {
                    uint32_t why = 0;
                    uint32_t kind = KIS_ENTRY_QUARANTINE;
                    Record decoded;
                    ItemTraits info;
                    if (record.length > KIS_RECORD_MAX || !decode_record(record.data, record.length, &decoded) ||
                        decoded.slot != record.slot || decoded.item != record.item || decoded.count != record.count)
                        why = KIS_QUARANTINE_DECODE;
                    else if (record.slot >= e.row.slots)
                        why = KIS_QUARANTINE_SLOT;
                    else if (!item_traits(a.items, record.item, &info))
                        why = KIS_QUARANTINE_UNKNOWN_ITEM;
                    else if (!record.count || record.count > info.max_stack || (info.pide && record.count != 1))
                        why = KIS_QUARANTINE_COUNT;
                    else if (!r.filter.allows(record.item, info.category, info.equip, info.pide)) {
                        why = KIS_PARK_FILTER;
                        kind = KIS_ENTRY_PARKED;
                    } else {
                        uint32_t destination = record.slot;
                        if (e.row.kind == KIS_ENTRY_PARKED &&
                            (destination >= r.slots || !slot_free(h->second, destination))) {
                            destination = 0;
                            while (destination < r.slots && !slot_free(h->second, destination))
                                ++destination;
                        }
                        if (destination >= r.slots) {
                            why = KIS_PARK_OVERFLOW;
                            kind = KIS_ENTRY_PARKED;
                        } else {
                            auto moved = record;
                            moved.slot = static_cast<uint16_t>(destination);
                            moved.data[0] = static_cast<uint8_t>(destination);
                            if (insert_record(a, h->second, moved, &why)) {
                                ++restored_stacks;
                                restored_items += moved.count;
                                continue;
                            }
                        }
                    }
                    auto part = entry_for(key, kind, e.row.slots, kind == KIS_ENTRY_PARKED ? why : 0);
                    if (kind == KIS_ENTRY_QUARANTINE)
                        record.reason = static_cast<uint8_t>(why);
                    part.records.push_back(record);
                    kept.push_back(std::move(part));
                    // one event when a record changes kind, or first finds its reason after the key registered
                    if (e.row.kind != kind ||
                        (e.row.reason == KIS_PARK_UNREGISTERED && why != KIS_PARK_UNREGISTERED))
                        model.event(kind == KIS_ENTRY_PARKED ? KIS_EVENT_PARKED : KIS_EVENT_QUARANTINED, &p, key, 0,
                                    why);
                }
                if (e.row.kind == KIS_ENTRY_LIVE)
                    for (unsigned ref = 0; ref < 4; ++ref)
                        if (e.row.refs[ref] != KIS_REF_NONE && e.row.refs[ref] < r.slots) {
                            h->second.refs[ref] = e.row.refs[ref];
                            static constexpr uint32_t equipment[]{12, 14, 15, 16};
                            if (refs.size() < 1024)
                                refs.push_back({slot, p.witness.join, key, equipment[ref], e.row.refs[ref], true});
                        }
            }
            p.entries = std::move(kept);
            h->second.stacks = 0;
            for (auto page : h->second.pages) {
                uint32_t cells[24]{};
                if (component_copy(api->server_world(), kHashInventory, page, cells, 96))
                    for (unsigned cell = 0; cell < 8; ++cell)
                        if (cells[cell * 3] && cells[cell * 3 + 1])
                            ++h->second.stacks;
            }
            if (created_now || restored_stacks) {
                model.event(KIS_EVENT_RESTORED, &p, key);
                auto& e = model.events.back();
                e.target = h->second.entity;
                e.stacks = restored_stacks;
                e.items = restored_items;
                if (p.build_pass_completed)
                    e.flags |= KIS_EVENT_LIVE;
                items_log(KILN_LOG_INFO, "player-storage: %s restored for slot %u: %u stacks%s", key.c_str(), slot,
                          restored_stacks, p.paired ? " (paired)" : " (unpaired/new)");
                if (!housekeeping(KIS_STAGE_RESTORED, lock))
                    return;
            }
        }
        if (Player* found = joined(slot, join)) {
            found->build_pass_completed = true;
            settle(*found);
        }
    }
    return_post(a, lock);
}
void KILN_CALL post(const KilnTickInfo*, void*) {
    std::lock_guard callback_lock(callback_mu);
    in_post = true;
    post_body();
    in_post = false;
}
bool program_slot(int32_t program, const char* name, int32_t* list, int32_t* slot) {
    for (int32_t l : {int32_t(KILN_GAME_LIST_WRITE), int32_t(KILN_GAME_LIST_WRITE_OPT), int32_t(KILN_GAME_LIST_READ),
                      int32_t(KILN_GAME_LIST_READ_OPT), int32_t(KILN_GAME_LIST_SYS_READ)}) {
        KilnStr names[64]{};
        int32_t n = world_api->game_program_list(KILN_WORLD_SERVER, program, l, names, 64);
        if (n > 64)
            return false;
        for (int32_t i = 0; i < n; ++i)
            if (same(names[i], name)) {
                *list = l;
                *slot = i;
                return true;
            }
    }
    return false;
}
uint32_t KILN_CALL death_each(uint64_t* iter, uint32_t row, uint32_t entity, void* user) {
    auto& context = *static_cast<DeathSlice*>(user);
    auto* slice = context.slice;
    if (slice->entity_stride != 16 || row >= slice->count || !slice->entity_ids)
        return 0;
    uint32_t source = 0;
    if (!load(reinterpret_cast<const uint8_t*>(slice->entity_ids) + row * slice->entity_stride, 4, &source))
        return 0;
    uint32_t n = 0;
    uint8_t writable = 0;
    auto state =
        static_cast<const uint8_t*>(world_api->game_slice_slot(slice, iter, loot_list, loot_slot, &n, &writable));
    uint8_t value = 0;
    if (!state || n <= loot_state_offset || !load(state, loot_state_offset, &value))
        return 0;
    std::lock_guard lock(storage_mu);
    if (context.before) {
        if (value != 1 || graves_done.count(entity))
            return 0;
        auto settings = static_cast<const uint8_t*>(
            world_api->game_slice_slot(slice, iter, settings_list, settings_slot, &n, &writable));
        uint8_t mode = 0;
        if (!settings || n <= tombstone_mode_offset || !load(settings, tombstone_mode_offset, &mode) || mode > 2)
            return 0;
        grave_before[entity] = {source, mode};
    } else {
        auto before = grave_before.find(entity);
        if (before == grave_before.end())
            return 0;
        auto candidate = before->second;
        grave_before.erase(before);
        if (value != 2 && value != 3)
            return 0;
        if (!graves_done.insert(entity).second)
            return 0;
        auto* p = player_entity(candidate.player);
        if (p && p->restored && !p->locked && deaths.size() < 1024) {
            Death death{p->witness.slot, entity, candidate.mode, p->witness.join, step, {}};
            for (const auto& [key, holder] : p->holders)
                if (model.registrations.count(key))
                    death.holders.push_back(holder.entity);
            if (!death.holders.empty())
                deaths.push_back(std::move(death));
        }
    }
    return 0;
}
void KILN_CALL death_stage(KilnGameSlice* slice, void* user) {
    if (!enabled || slice->world_kind != KILN_WORLD_SERVER)
        return;
    DeathSlice ctx{slice, user == nullptr};
    world_api->game_slice_iterate(slice, &death_each, &ctx);
}
void install_death_wrap() {
    if (death_before_wrap || !world_api || !enabled)
        return;
    int32_t index = world_api->game_program_find(KILN_WORLD_SERVER, str("player_items_loot"));
    if (index < 0)
        return;
    if (!field("CreateItemLoot", "lootCreationState", &loot_state_offset, 1) ||
        !field("GameSettings", "tombstoneMode", &tombstone_mode_offset, 1) ||
        !program_slot(index, "CreateItemLoot", &loot_list, &loot_slot) ||
        (!program_slot(index, "GameSettings", &settings_list, &settings_slot) &&
         !program_slot(index, "ecs::GameSettings", &settings_list, &settings_slot)) ||
        world_api->game_program_wrap(items_owner(), KILN_WORLD_SERVER, index, KILN_WRAP_BEFORE, 0, &death_stage,
                                     nullptr, &death_before_wrap) != KILN_OK ||
        world_api->game_program_wrap(items_owner(), KILN_WORLD_SERVER, index, KILN_WRAP_AFTER, 0, &death_stage,
                                     reinterpret_cast<void*>(1), &death_after_wrap) != KILN_OK) {
        off(KILN_CAP_REASON_SYMBOL_MISSING, "items.storage: tombstone source/state hook unavailable");
    }
}
bool death_wrap_ready() {
    if (!death_before_wrap || !death_after_wrap || !world_api)
        return false;
    for (auto handle : {death_before_wrap, death_after_wrap}) {
        KilnGameWrapInfo info{};
        info.size = sizeof info;
        if (world_api->game_wrap_info(handle, &info) != KILN_OK || info.state == KILN_WRAP_REFUSED ||
            info.state == KILN_WRAP_REMOVED || info.disabled != KILN_WRAP_OK) {
            off(KILN_CAP_REASON_FAULTED, "items.storage: tombstone observation wrap refused or disabled");
            return false;
        }
        if (info.state != KILN_WRAP_ACTIVE)
            return false;
    }
    return true;
}
void death_moves(const Access& a) {
    const size_t pending = deaths.size();
    for (size_t n = 0; n < pending; ++n) {
        auto q = deaths.front();
        deaths.pop_front();
        auto i = model.players.find(q.slot);
        if (i == model.players.end() || i->second.witness.join != q.join || !i->second.restored || i->second.locked ||
            (i->second.witness.flags & KIS_WITNESS_LEFT))
            continue;
        auto& p = i->second;
        PageSet target{};
        bool initialized = false;
        if (!read_pages(a, q.grave, &target, &initialized) || !initialized) {
            if (step - q.submitted < 250)
                deaths.push_back(q);
            continue;
        }
        for (auto& [key, h] : p.holders) {
            if (std::find(q.holders.begin(), q.holders.end(), h.entity) == q.holders.end())
                continue;
            auto registration = model.registrations.find(key);
            if (registration == model.registrations.end())
                continue;
            auto rule = registration->second.death;
            uint32_t stacks = 0, items = 0;
            PageSet source{};
            if (!read_pages(a, h.entity, &source, &initialized) || !initialized)
                continue;
            auto* tx = static_cast<uint8_t*>(_aligned_malloc(static_cast<size_t>(E.tx_size), 16));
            if (!tx) {
                off(KILN_CAP_REASON_FAULTED, "items.storage: cannot allocate death transaction");
                return;
            }
            for (uint32_t page = 0; page < source.n; ++page)
                for (const auto& stack : source.p[page].s) {
                    if (is_empty(stack))
                        continue;
                    ItemTraits info;
                    if (!item_traits(a.items, stack.item, &info) ||
                        !death_takes(rule, q.mode, info.category, true, true))
                        continue;
                    MoveJob job{};
                    job.r.size = sizeof job.r;
                    job.r.kind = KILN_MOVE_STACK;
                    job.r.from = h.entity;
                    job.r.to = q.grave;
                    job.r.slot = stack.slot;
                    KilnItemsMoveResult result{};
                    unsigned long code = 0;
                    NoteLease notices;
                    KilnEntity notice_roots[]{h.entity, q.grave};
                    notices.begin(a, notice_roots, 2);
                    bool moved_ok = run_move(a, job, tx, &result, &code);
                    notices.end(a);
                    if (!moved_ok) {
                        abandon_tx(tx);
                        _aligned_free(tx);
                        off(KILN_CAP_REASON_FAULTED, "items.storage: death move faulted");
                        return;
                    }
                    if (result.done) {
                        ++stacks;
                        items += result.done;
                    }
                }
            _aligned_free(tx);
            model.event(KIS_EVENT_DEATH, &p, key, 0, rule);
            auto& e = model.events.back();
            e.target = items ? q.grave : 0;
            e.stacks = stacks;
            e.items = items;
            if (items)
                ++p.changes;
            items_log(KILN_LOG_INFO, "player-storage: death %s: %u stacks, %u items -> tombstone %u", key.c_str(),
                      stacks, items, e.target);
        }
    }
}
} // namespace
void storage_init() {
    api = items_api();
    storage_guards.store(&guards);
    S.structural_gate = &structural_gate;
    uintptr_t capture_at = 0, read_at = 0, join_at = 0, stage_at = 0, transfer_at = 0, template_at = 0;
    std::array<int64_t, 8> layout{};
    detail = "sigdb items.storage or kiln.items transactions/read unavailable";
    const char* names[]{"characters/struct.equip0", "characters/struct.equip1",     "characters/struct.equip_last",
                        "characters/struct.refs",   "characters/struct.main_pages", "characters/struct.extra_pages",
                        "characters/struct.list",   "items/record.stride"};
    bool ok = api->feature(str("items.storage")) && E.tx_ok && E.read_ok;
    for (unsigned i = 0; i < 8 && ok; ++i)
        ok = constant(names[i], &layout[i]);
    if (ok)
        ok = derive_layout(layout, &S.layout, &detail);
    if (ok)
        ok = function("items/RecordFromStack", &S.convert) && function("items/RecordWrite", &S.write) &&
             function("items/RecordRead", &S.read) && function("items/WriteComponentTick", &S.tick_write) &&
             function("items/PideFromRecord", &S.rebuild) && function("items/CreateEntity", &S.create) &&
             function("spawn/DestroyEntity", &S.destroy) && symbol("items/PageTemplateGuid", &template_at);
    uint8_t guid[16]{};
    if (ok && (!storage_copy(guid, reinterpret_cast<void*>(template_at), 16) ||
               std::memcmp(guid, default_inventory_guid, 16))) {
        ok = false;
        detail = "items/PageTemplateGuid: wrong DefaultInventory template";
    }
    const char* bn[]{"items/record.binding.dye",   "items/record.binding.durability",   "items/record.binding.fuel",
                     "items/record.binding.level", "items/record.binding.perks_legacy", "items/record.binding.perks",
                     "items/record.binding.gem"};
    for (unsigned i = 0; i < 7 && ok; ++i)
        ok = constant(bn[i], &binding_offsets[i]);
    if (ok)
        ok = constant("players/rec.stride", &player_stride) && constant("players/rec.account", &account_offset) &&
             constant("characters/rec.character", &id_offset);
    if (ok)
        ok = symbol("characters/Capture", &capture_at) && symbol("characters/Read", &read_at) &&
             symbol("characters/JoinApply", &join_at) && symbol("gameplay/DeliverEvents", &transfer_at);
    if (ok)
        ok = qualified(capture_at, "characters/Capture") && qualified(read_at, "characters/Read") &&
             qualified(join_at, "characters/JoinApply") && qualified(transfer_at, "gameplay/DeliverEvents");
    if (ok) {
        ok = capture_hook.install(api, items_owner(), capture_at, reinterpret_cast<void*>(&capture),
                                  "characters/Capture") &&
             read_hook.install(api, items_owner(), read_at, reinterpret_cast<void*>(&read_character),
                               "characters/Read") &&
             join_hook.install(api, items_owner(), join_at, reinterpret_cast<void*>(&join_apply),
                               "characters/JoinApply");
        if (!ok)
            detail = "items.storage: character hook installation failed";
    }
    stage_point =
        ok && symbol("saves/CapturesDone", &stage_at) && qualified(stage_at, "saves/CapturesDone") &&
        stage_hook.install(api, items_owner(), stage_at, reinterpret_cast<void*>(&captures_done), "saves/CapturesDone");
    if (!stage_point)
        report("items.storage.persist", KILN_CAP_UNAVAILABLE, KILN_CAP_REASON_SYMBOL_MISSING,
               "no captures-done hook on this build");
    enabled = ok;
    reason = ok ? uint32_t(KILN_CAP_REASON_NONE) : uint32_t(KILN_CAP_REASON_SYMBOL_MISSING);
    if (ok)
        detail = "waiting for services";
    else
        report("items.storage", KILN_CAP_UNAVAILABLE, reason, detail.c_str());
    KilnHandle h = 0;
    post_point = api->subscribe_tick(items_owner(), KILN_TICK_POST, &post, nullptr, &h) == KILN_OK;
    if (!post_point)
        off(KILN_CAP_REASON_SERVICE_MISSING, "items.storage: no server Post/no-jobs point");
    api->register_service(items_owner(), str(KILN_ITEMS_STORAGE_SERVICE), &table);
    report("items.storage.persistence_health", KILN_CAP_PENDING, KILN_CAP_REASON_SESSION, "waiting for a storage save");
}
namespace {
void KILN_CALL storage_owner_retiring(KilnOwner, void*);
const char* result_name(KilnResult rc) {
    switch (rc) {
    case KILN_ERR_INVALID_ARGUMENT: return "invalid argument";
    case KILN_ERR_NOT_FOUND: return "not found";
    case KILN_ERR_UNAVAILABLE: return "unavailable: kiln.log says why";
    case KILN_ERR_REVOKED: return "revoked";
    case KILN_ERR_INTERNAL: return "internal";
    default: return "error";
    }
}
// The client inventory guard: a post link on gameplay/DeliverEvents, in Kiln's hook registry. On a refusal detail says
// which check failed: the sigdb feature, the symbol, the live code, the reflected client input layout or the install.
bool install_client_guard(const KilnHooksApi* hooks) {
    uintptr_t at = 0;
    if (!api->feature(str("players.client_action_hold"))) {
        detail = "sigdb feature players.client_action_hold is off on this build";
        return false;
    }
    if (!symbol("gameplay/DeliverEvents", &at) || !qualified(at, "gameplay/DeliverEvents"))
        return false;
    ClientLayout layout;
    if (auto why = client_layout(types_api, &layout); !why.empty()) {
        detail = "client input layout: " + why;
        return false;
    }
    client_actions = std::move(layout.actions);
    input_size = layout.input_size;
    consumed_size = layout.consumed_size;
    KilnHookOrder order{};
    order.size = sizeof order;
    order.label = str("items/StorageClientGuard");
    KilnHandle handle = 0;
    const KilnResult rc = hooks->raw_detour(items_owner(), at, &order, reinterpret_cast<void*>(&client_guard),
                                            &transfer_next, &handle);
    if (rc != KILN_OK) {
        detail = "gameplay/DeliverEvents: kiln.hooks raw_detour returned " + std::to_string(rc) + " (" +
                 result_name(rc) + ")";
        if (kiln::hooks_targets_full(hooks))
            detail += "; every hook target slot is in use";
        return false;
    }
    return true;
}
}
void storage_ready() {
    world_api = static_cast<const KilnWorldApi*>(api->service(str(KILN_WORLD_SERVICE), 1));
    types_api = static_cast<const KilnTypesApi*>(api->service(str(KILN_TYPES_SERVICE), 1));
    players_api = static_cast<const KilnPlayersApi*>(api->service(str(KILN_PLAYERS_SERVICE), 1));
    saves_api = static_cast<const KilnSavesApi*>(api->service(str(KILN_SAVES_SERVICE), 1));
    if (!enabled)
        return;
    if (!world_api || world_api->header.size < offsetof(KilnWorldApi, in_program) + sizeof(world_api->in_program) ||
        !world_api->game_program_find || !world_api->game_program_list || !world_api->game_program_wrap ||
        !world_api->game_wrap_info || !types_api ||
        types_api->header.size < offsetof(KilnTypesApi, in_system_callback) + sizeof(types_api->in_system_callback) ||
        !types_api->type_by_name || !types_api->type_info || !types_api->type_field || !players_api ||
        players_api->header.size < offsetof(KilnPlayersApi, status) + sizeof(players_api->status) ||
        !players_api->get || !players_api->status || !saves_api ||
        saves_api->header.size < offsetof(KilnSavesApi, next_event) + sizeof(saves_api->next_event) ||
        !saves_api->next_event) {
        off(KILN_CAP_REASON_SERVICE_MISSING, "items.storage: world/types/players/saves service missing");
        return;
    }
    auto hooks = kiln::hooks_v2(api);
    if (!hooks ||
        hooks->header.size <
            offsetof(KilnHooksApi, subscribe_owner_retiring) + sizeof(hooks->subscribe_owner_retiring) ||
        !hooks->subscribe_owner_retiring ||
        hooks->subscribe_owner_retiring(items_owner(), &storage_owner_retiring, nullptr, &retiring_subscription) !=
            KILN_OK) {
        off(KILN_CAP_REASON_SERVICE_MISSING, "items.storage: hooks.owner_cleanup unavailable");
        return;
    }
    if (!install_client_guard(hooks)) {
        off(KILN_CAP_REASON_SYMBOL_MISSING, "items.storage: client inventory guard is unavailable: " + detail);
        return;
    }
    resolve_main_baseline();
    detail = baseline_authored ? "ready; baseline from 1_Player original InventorySetup"
                               : "ready; 5 pages: baseline from the authored default";
    report("items.storage", KILN_CAP_AVAILABLE, KILN_CAP_REASON_NONE, "player storage native service");
    if (stage_point)
        report("items.storage.persist", KILN_CAP_AVAILABLE, KILN_CAP_REASON_NONE, "captures-done stage");
}
void storage_pre(uint64_t current) {
    capture_diagnostics();
    deliver_returns();
    std::lock_guard lock(storage_mu);
    step = current;
    if (!enabled || !players_api)
        return;
    if (!baseline_authored && current % 50 == 1)
        resolve_main_baseline();
    install_death_wrap();
    for (auto& [slot, p] : model.players) {
        if (!p.applied || (p.witness.flags & KIS_WITNESS_LEFT))
            continue;
        KilnPlayer player{};
        player.size = sizeof player;
        if (players_api->get(p.witness.player, &player) != KILN_OK || player.state == KILN_PLAYER_LEAVING) {
            if (!p.leaving) {
                p.leaving = true;
                p.leave_step = step;
            }
            if (step - p.leave_step > 250) {
                // The game's leave save captures the live character (characters/Capture reads its entity). While the
                // player entity exists that capture can still come, late behind a world save that is still writing;
                // tearing the holders down first would let it publish a character whose storage was never captured
                // with it (an item moved since the last save lost, or duplicated). Only a gone entity is torn down.
                uint8_t setup[84]{};
                if (component_copy(api->server_world(), kHashInventorySetup, p.witness.player, setup, sizeof setup))
                    continue;
                for (const auto& [k, h] : p.holders)
                    garbage.push_back(h);
                model.leave(slot);
            }
        } else
            p.leaving = false;
    }
}
bool storage_work() {
    std::lock_guard lock(storage_mu);
    return enabled &&
           (!model.registrations.empty() || !refs.empty() || !garbage.empty() || !returns.empty() || !parks.empty() ||
            !deaths.empty() || std::any_of(model.players.begin(), model.players.end(), [](const auto& row) {
                const auto& player = row.second;
                return player.restored && player.applied && !player.locked && !player.build_pass_completed &&
                       !(player.witness.flags & KIS_WITNESS_LEFT);
            }));
}
void storage_program(const Access& a) {
    std::lock_guard lock(storage_mu);
    if (!enabled)
        return;
    structural_access = {};
    structural_access.storage = a.storage;
    structural_access.items = a.items;
    structural_access.attributes = a.attributes;
    death_moves(a);
    return_moves(a);
    while (!refs.empty()) {
        auto q = std::move(refs.front());
        refs.pop_front();
        auto p = model.players.find(q.slot);
        if (p == model.players.end() || p->second.witness.join != q.join)
            continue;
        auto h = p->second.holders.find(q.key);
        auto registration = model.registrations.find(q.key);
        if (h == p->second.holders.end() || registration == model.registrations.end() ||
            (q.index != KIS_REF_CLEAR && (q.index >= registration->second.slots || q.index >= h->second.slots ||
                                          q.index / 8 >= h->second.pages.size())))
            continue;
        uint64_t cell = q.index == KIS_REF_CLEAR ? 0 : KILN_SLOT_ID(h->second.pages[q.index / 8], q.index % 8);
        // clearing touches the reference only while it still points into this storage
        if (reference_write(a, p->second.witness.player, q.equip, cell, q.only_empty,
                            q.index == KIS_REF_CLEAR ? &h->second.pages : nullptr)) {
            unsigned ref = q.equip == 12 ? 0 : q.equip == 14 ? 1 : q.equip == 15 ? 2 : 3;
            h->second.refs[ref] = q.index == KIS_REF_CLEAR ? KIS_REF_NONE : static_cast<uint16_t>(q.index);
            ++p->second.changes;
        }
    }
}
namespace {
void retire_owner_locked(KilnOwner owner) {
    if (!owner || model.retired.count(owner))
        return;
    if (model.owns_persist(owner)) {
        stage_fn = nullptr;
        stage_user = nullptr;
    }
    refs.erase(std::remove_if(refs.begin(), refs.end(),
                              [&](const Ref& ref) {
                                  auto r = model.registrations.find(ref.key);
                                  return r != model.registrations.end() && r->second.owner == owner;
                              }),
               refs.end());
    for (auto& q : returns)
        if (q.owner == owner)
            q.done = nullptr;
    for (auto& r : returned)
        if (r.owner == owner)
            r.fn = nullptr;
    for (auto& [slot, p] : model.players)
        for (const auto& [key, r] : model.registrations)
            if (r.owner == owner && p.holders.count(key))
                parks.push_back({slot, p.witness.join, key, KIS_PARK_UNREGISTERED, false, true});
    model.revoke(owner);
}
void KILN_CALL storage_owner_retiring(KilnOwner owner, void*) {
    std::lock_guard callback_lock(callback_mu); // a hint or callback of this owner may be running
    std::lock_guard lock(storage_mu);
    retire_owner_locked(owner);
}
} // namespace
void storage_cancel_owner(KilnOwner owner) {
    if (!owner)
        return;
    std::lock_guard callback_lock(callback_mu);
    std::lock_guard lock(storage_mu);
    retire_owner_locked(owner);
}
void storage_reset() {
    std::lock_guard callback_lock(callback_mu);
    std::lock_guard lock(storage_mu);
    for (auto& q : returns) {
        q.result.status = KILN_ITEMS_DROPPED;
        std::snprintf(q.result.message, sizeof q.result.message, "server world closed");
        returned.push_back({q.owner, q.done, q.user, q.result});
    }
    returns.clear();
    model.reset_session();
    structural_access = {};
    refs.clear();
    parks.clear();
    garbage.clear();
    unreadable_pages.clear();
    deaths.clear();
    grave_before.clear();
    graves_done.clear();
}
} // namespace kitems
