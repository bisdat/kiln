// SPDX-License-Identifier: MIT
// Windows SEH and the actual engine adapter with a fake ECS; never starts or loads the game.
#include "../storage.cpp"
#include "../storage_engine.cpp"
#include "fake_types.h"
#include <cstdio>
#include <cstdarg>
#include <malloc.h>
#include <map>
#include <new>
#include <thread>
// Freed memory is poisoned and never reused (the heap's free-list words would cover the poison), so a container node
// used after another thread (or a callback) erased it faults at once instead of reading stale but plausible links.
void* operator new(size_t n) {
    if (void* p = std::malloc(n ? n : 1))
        return p;
    throw std::bad_alloc();
}
void operator delete(void* p) noexcept {
    if (p) {
        std::memset(p, 0xDD, _msize(p)); // the test leaks every freed block on purpose
    }
}
void operator delete(void* p, size_t) noexcept { operator delete(p); }
namespace {
using namespace kitems;
using namespace kitems::storage;
int checks = 0, failed = 0;
uint32_t next_id = 2000;
std::map<std::pair<uint32_t, uint32_t>, std::vector<uint8_t>> components;
bool fail_create = false, raise_create = false;
unsigned create_fail_at = 0;
uint32_t create_raw = 0;
bool create_fault = false, mutate_before_failure = false, destroy_fault = false;
uint8_t destroy_raw = 1;
unsigned setup_lookup_mode = 0;
void* readonly_setup = nullptr;
std::vector<std::string> storage_logs;
unsigned creates = 0, destroys = 0, writes = 0, converts = 0;
#define CHECK(...)                                                                                                     \
    do {                                                                                                               \
        ++checks;                                                                                                      \
        if (!(__VA_ARGS__)) {                                                                                          \
            ++failed;                                                                                                  \
            std::printf("FAIL %d %s\n", __LINE__, #__VA_ARGS__);                                                       \
        }                                                                                                              \
    } while (0)
uint32_t __fastcall create(void*, const CreateDesc* d) {
    ++creates;
    CHECK(d->flags == 4 && d->requested == 0 && d->count == 2);
    CHECK(std::memcmp(d->guid, default_inventory_guid, 16) == 0);
    if (raise_create)
        RaiseException(0xe0000012, 0, 0, nullptr);
    if (fail_create && creates == 2)
        return 0;
    const bool failing = creates == create_fail_at;
    if (failing && !mutate_before_failure) {
        if (create_fault)
            RaiseException(0xe0000012, 0, 0, nullptr);
        return create_raw;
    }
    auto e = next_id++;
    for (unsigned i = 0; i < d->count; ++i) {
        const auto& c = d->components[i];
        components[{e, c.hash}].assign(static_cast<const uint8_t*>(c.bytes),
                                       static_cast<const uint8_t*>(c.bytes) + c.size);
    }
    if (failing) {
        if (create_fault)
            RaiseException(0xe0000012, 0, 0, nullptr);
        return create_raw;
    }
    return e;
}
uint8_t __fastcall destroy(void*, uint32_t e, uint8_t mode, uint8_t skip) {
    CHECK(mode == 1 && skip == 0);
    ++destroys;
    if (destroy_fault)
        RaiseException(0xe0000013, 0, 0, nullptr);
    if (!destroy_raw)
        return 0;
    for (auto i = components.begin(); i != components.end();)
        i = i->first.first == e ? components.erase(i) : std::next(i);
    return destroy_raw;
}
bool gate_open = true;
unsigned gate_refusals = 0;
bool gate() {
    if (gate_refusals) {
        --gate_refusals;
        return false;
    }
    return gate_open;
}
PtrSize* __fastcall get(PtrSize* out, uintptr_t, uint32_t hash, uint32_t entity) {
    if (entity > kMaxPlayer && hash == kHashInventorySetup) {
        if (setup_lookup_mode == 1) {
            *out = {};
            return out;
        }
        if (setup_lookup_mode == 2)
            RaiseException(0xe0000014, 0, 0, nullptr);
        if (setup_lookup_mode == 3) {
            *out = {nullptr, 12};
            return out;
        }
        if (setup_lookup_mode == 4) {
            *out = {readonly_setup, 84};
            return out;
        }
    }
    auto i = components.find({entity, hash});
    *out = i == components.end() ? PtrSize{} : PtrSize{i->second.data(), i->second.size()};
    return out;
}
PtrSize* __fastcall tick_write(PtrSize* out, uintptr_t world, uint32_t hash, uint32_t entity) {
    return get(out, world, hash, entity);
}
PtrSize* __fastcall other(PtrSize* out, void*, uint64_t hash, uint32_t e) {
    return get(out, 0, static_cast<uint32_t>(hash), e);
}
PtrSize* __fastcall write_other(PtrSize* out, void* storage, void*, uint64_t hash, uint32_t e, uint8_t) {
    return other(out, storage, hash, e);
}
void* __fastcall tracker(void*) { return reinterpret_cast<void*>(1); }
uint8_t __fastcall flag(void*) { return 0; }
Result8* __fastcall convert(Result8* out, void* r, const void* stack, uint8_t slot, void*, void*, EntityRef*) {
    ++converts;
    Record v{};
    v.item = static_cast<const uint32_t*>(stack)[0];
    v.slot = slot;
    v.count = static_cast<uint16_t>(static_cast<const uint32_t*>(stack)[1]);
    memory_from_record(v, static_cast<uint8_t*>(r));
    *out = {};
    return out;
}
uint8_t item_bytes[0x800]{};
uint32_t relocate_page = 0;
std::vector<std::vector<uint8_t>> retired_pages;
void* __fastcall lookup_item(uint32_t item, void*) { return item == 123 ? item_bytes : nullptr; }
void __fastcall reader(void* out, void* stream) {
    auto b = *static_cast<uint64_t**>(stream);
    Record r;
    CHECK(decode_record(reinterpret_cast<const uint8_t*>(b[0]), static_cast<size_t>(b[1]), &r));
    memory_from_record(r, static_cast<uint8_t*>(out));
    b[3] = b[1];
}
uint32_t __fastcall rebuild(const void*, uint32_t owner, const void*, const void* context) {
    CHECK(kitems::storage::read<void*>(context, 0) == reinterpret_cast<void*>(1));
    auto& old = components[{relocate_page, kHashInventory}];
    retired_pages.push_back(std::move(old));
    old.assign(96, 0);
    components[{5000, kHashItemState}].assign(20, 0);
    components[{5000, 0xd7361788u}].resize(4);
    std::memcpy(components[{5000, 0xd7361788u}].data(), &owner, 4);
    return 5000;
}
void __fastcall writer(void* stream, const void* rec) {
    ++writes;
    Record r{};
    record_from_memory(static_cast<const uint8_t*>(rec), &r);
    auto wire = wire_record(r);
    auto b = *static_cast<uint64_t**>(stream);
    std::memcpy(reinterpret_cast<uint8_t*>(b[0]) + b[2], wire.data(), wire.size());
    b[2] += wire.size();
}
} // namespace
namespace kitems {
Engine E;
KilnApi fake_api{};
const KilnApi* items_api() { return &fake_api; }
KilnOwner items_owner() { return 7; }
void items_log(int, const char* format, ...) {
    char text[2048]{};
    va_list args;
    va_start(args, format);
    std::vsnprintf(text, sizeof text, format, args);
    va_end(args);
    storage_logs.emplace_back(text);
}
KilnResult items_item_info(uint32_t, KilnItemInfo*) { return KILN_ERR_NOT_FOUND; }
bool run_move(const Access&, const MoveJob& job, uint8_t*, KilnItemsMoveResult* out, unsigned long*) {
    auto src = kitems::storage::read<uint32_t>(components[{KILN_SLOT_INVENTORY(job.r.slot), kHashInventory}].data(),
                                               KILN_SLOT_INDEX(job.r.slot) * 12);
    auto& target = components[{101, kHashInventory}];
    auto& source = components[{KILN_SLOT_INVENTORY(job.r.slot), kHashInventory}];
    uint32_t count = kitems::storage::read<uint32_t>(source.data(), KILN_SLOT_INDEX(job.r.slot) * 12 + 4);
    std::memcpy(target.data(), source.data() + KILN_SLOT_INDEX(job.r.slot) * 12, 12);
    std::memset(source.data() + KILN_SLOT_INDEX(job.r.slot) * 12, 0, 12);
    *out = {};
    out->status = KILN_ITEMS_DONE;
    out->done = count;
    out->stacks = 1;
    CHECK(src == 123);
    return true;
}
void abandon_tx(uint8_t*) {}
NoteLease::~NoteLease() = default;
void NoteLease::begin(const Access&, const KilnEntity*, uint32_t) {}
void NoteLease::end(const Access&) {}
bool read_pages(const Access&, uint32_t entity, PageSet* out, bool* initialized) {
    *out = {};
    auto setup = components.find({entity, kHashInventorySetup});
    if (setup == components.end()) {
        if (initialized)
            *initialized = false;
        return false;
    }
    if (initialized)
        *initialized = setup->second[83] != 0;
    for (unsigned i = 0; i < 16; ++i) {
        uint32_t page = kitems::storage::read<uint32_t>(setup->second.data(), i * 4);
        if (!page)
            break;
        auto inv = components.find({page, kHashInventory});
        if (inv == components.end())
            return false;
        auto& p = out->p[out->n];
        p.entity = page;
        out->cat[out->n++] = 4;
        for (unsigned cell = 0; cell < 8; ++cell)
            p.s[cell] = {KILN_SLOT_ID(page, cell), kitems::storage::read<uint32_t>(inv->second.data(), cell * 12),
                         kitems::storage::read<uint32_t>(inv->second.data(), cell * 12 + 4),
                         kitems::storage::read<uint32_t>(inv->second.data(), cell * 12 + 8)};
    }
    return true;
}
} // namespace kitems
namespace {
uintptr_t KILN_CALL server() { return 1; }
bool KILN_CALL on_tick() { return true; }
KilnGamedataApi fake_gamedata{};
int64_t authored_slots = 40;
KilnResult KILN_CALL find_player(uint32_t section, KilnStr key, KilnGdEntry* out) {
    CHECK(section == KILN_GD_TEMPLATES && key.len == 36);
    out->id = 7;
    return KILN_OK;
}
KilnResult KILN_CALL get_default(uint32_t section, uint32_t id, KilnStr path, KilnContentValue* current,
                                 KilnContentValue* original) {
    CHECK(section == KILN_GD_TEMPLATES && id == 7 &&
          std::string(path.ptr, path.len) == "InventorySetup.genericSlotCount");
    current->type = original->type = KILN_CV_INT;
    current->i = 128;
    original->i = authored_slots;
    return KILN_OK;
}
KilnCapsApi fake_caps{};
const void* KILN_CALL service(KilnStr name, uint32_t) {
    if (fake_caps.header.size && std::string(name.ptr, name.len) == KILN_CAPS_SERVICE)
        return &fake_caps;
    return fake_gamedata.header.size && std::string(name.ptr, name.len) == KILN_GAMEDATA_SERVICE ? &fake_gamedata
                                                                                                 : nullptr;
}
void KILN_CALL stage_callback(uint32_t kind, void* user) {
    auto calls = static_cast<unsigned*>(user);
    ++*calls;
    CHECK(kind == KIS_EVENT_BATCH || kind == KIS_STAGE_PARKED || kind == KIS_STAGE_RESTORED);
    if (kind != KIS_EVENT_BATCH) {
        auto& event = kitems::model.events.back();
        CHECK(event.target && components.count({event.target, kHashInventorySetup}));
        bool unlocked = false;
        std::thread probe([&] {
            if (kitems::storage_mu.try_lock()) {
                unlocked = true;
                kitems::storage_mu.unlock();
            }
        });
        probe.join();
        CHECK(unlocked);
    }
}
unsigned returns_called = 0;
void KILN_CALL returned_callback(const KilnStorageReturnResult* result, void*) {
    ++returns_called;
    CHECK(result->status == KILN_ITEMS_DONE && result->done == 5 && result->kept == 0);
}
void service_tests() {
    using namespace kitems;
    using namespace kitems::storage;
    fake_api.size = sizeof fake_api;
    fake_api.server_world = &server;
    fake_api.on_tick_thread = &on_tick;
    fake_api.service = &service;
    kitems::api = &fake_api;
    kitems::resolve_main_baseline();
    CHECK(kitems::main_baseline == 5 && !kitems::baseline_authored);
    fake_gamedata.header = {sizeof fake_gamedata, 1};
    fake_gamedata.find = &find_player;
    fake_gamedata.get = &get_default;
    kitems::resolve_main_baseline();
    CHECK(kitems::main_baseline == 5 && kitems::baseline_authored);
    authored_slots = 57;
    kitems::resolve_main_baseline();
    CHECK(kitems::main_baseline == 8 && kitems::baseline_authored);
    authored_slots = 0;
    kitems::resolve_main_baseline();
    CHECK(kitems::main_baseline == 5 && !kitems::baseline_authored);
    fake_gamedata = {};
    kitems::model = Model{};
    kitems::enabled = true;
    kitems::stage_point = true;
    kitems::detail = "ready";
    uint32_t handle = 0;
    KilnStorageDef d{};
    d.size = sizeof d;
    d.slots = 9;
    d.key = {"mod:bag", 7};
    d.filter.size = sizeof d.filter;
    CHECK(kitems::register_storage(10, &d, &handle) == KILN_OK && handle == 1);
    uint32_t holder = 99;
    CHECK(kitems::holder(10, handle, 1, &holder) == KILN_ERR_NOT_FOUND && holder == 0);
    unsigned calls = 0;
    uint64_t last = 99;
    CHECK(kitems::bind(0, &stage_callback, &calls, &last) == KILN_OK && last == 0);
    CHECK(kitems::bind(1, nullptr, nullptr, nullptr) == KILN_ERR_UNAVAILABLE);
    auto join = kitems::model.join(0, 55, {}, 123, 1);
    KilnStorageWitness w{};
    w.size = sizeof w;
    CHECK(kitems::witness_of(1, 0, &w) == KILN_ERR_UNAVAILABLE);
    CHECK(kitems::witness_of(0, 0, &w) == KILN_OK && w.join == join && w.witness == 123);
    KilnStorageDoc doc{};
    doc.size = sizeof doc;
    doc.source = KIS_SOURCE_NEW;
    doc.account = 55;
    CHECK(kitems::restore(0, 0, join, &doc) == KILN_OK);
    CHECK(kitems::restore(0, 0, join, &doc) == KILN_ERR_INVALID_ARGUMENT);
    kitems::model.applied(0, 1);
    kitems::model.mark_built(0, "mod:bag", 2000, {2001, 2002});
    KilnStorageState s{};
    s.size = sizeof s;
    CHECK(kitems::state(11, handle, 1, &s) == KILN_ERR_NOT_FOUND);
    CHECK(kitems::state(10, handle, 1, &s) == KILN_OK && s.status == KIS_STATE_READY && s.pages == 2 &&
          s.holder == 2000);
    CHECK(kitems::set_ref(10, handle, 1, 13, 0) == KILN_ERR_INVALID_ARGUMENT);
    CHECK(kitems::set_ref(10, handle, 1, 14, 9) == KILN_ERR_INVALID_ARGUMENT);
    CHECK(kitems::set_ref(10, handle, 1, 14, 8) == KILN_OK);
    CHECK(kitems::refs.size() == 1);
    uint32_t count = 0;
    CHECK(kitems::list(1, nullptr, 0, &count) == KILN_ERR_BUFFER_TOO_SMALL && count == 1);
    CHECK(kitems::list(1, &s, 1, &count) == KILN_OK && s.handle == handle);
    auto tag = kitems::model.begin_batch(7, 8, 1);
    kitems::model.capture(0, 234, {}, false, 0);
    kitems::model.complete();
    KilnStorageFrozenBatch b{};
    b.size = sizeof b;
    CHECK(kitems::take(0, tag, &b, nullptr, 0, nullptr, 0, nullptr, 0) == KILN_ERR_BUFFER_TOO_SMALL &&
          b.capture_count == 1 && b.saves_seq == 7);
    KilnStorageCapture capture{};
    capture.size = sizeof capture;
    CHECK(kitems::take(0, tag, &b, &capture, 1, nullptr, 0, nullptr, 0) == KILN_OK && capture.witness == 234);
    CHECK(kitems::release(1, tag) == KILN_ERR_UNAVAILABLE);
    CHECK(kitems::release(0, tag) == KILN_OK);
    CHECK(kitems::take(0, tag, &b, &capture, 1, nullptr, 0, nullptr, 0) == KILN_ERR_NOT_FOUND);
    Entry parked = kitems::entry_for("mod:bag", KIS_ENTRY_PARKED, 64, KIS_PARK_UNREGISTERED);
    KilnStorageRecord record{};
    record.slot = 0;
    record.length = 8;
    record.item = 123;
    record.count = 5;
    Record raw{};
    raw.slot = 0;
    raw.item = 123;
    raw.count = 5;
    auto wire = wire_record(raw);
    std::memcpy(record.data, wire.data(), wire.size());
    parked.records.push_back(record);
    kitems::model.players.at(0).entries.push_back(parked);
    uint64_t return_ticket = 0;
    CHECK(kitems::return_parked(10, 1, {"mod:bag", 7}, 2, nullptr, nullptr, &return_ticket) ==
          KILN_ERR_INVALID_ARGUMENT);
    CHECK(kitems::return_parked(10, 1, {"mod:bag", 7}, 100, &returned_callback, nullptr, &return_ticket) == KILN_OK &&
          return_ticket);
    CHECK(kitems::return_parked(10, 1, {"mod:bag", 7}, 100, nullptr, nullptr, &return_ticket) == KILN_ERR_UNAVAILABLE);
    components[{100, kHashInventorySetup}].assign(84, 0);
    uint32_t target_page = 101;
    std::memcpy(components[{100, kHashInventorySetup}].data(), &target_page, 4);
    components[{100, kHashInventorySetup}][64] = 4;
    components[{100, kHashInventorySetup}][81] = 8;
    components[{100, kHashInventorySetup}][83] = 1;
    components[{101, kHashInventory}].assign(96, 0);
    Access access{};
    access.storage = reinterpret_cast<void*>(1);
    E.tx_size = 16;
    std::unique_lock service_lock(kitems::storage_mu);
    kitems::return_post(access, service_lock);
    CHECK(kitems::returns.size() == 1 && kitems::returns.front().phase == 1);
    auto& pending = kitems::returns.front();
    KilnStorageState listed[2]{};
    listed[0].size = sizeof listed[0];
    CHECK(kitems::list(1, listed, 2, &count) == KILN_OK && count == 2 && listed[0].status == KIS_STATE_READY &&
          listed[1].status == KIS_STATE_PARKED && listed[1].holder == pending.holder.entity && listed[1].holder != 0 &&
          listed[1].handle == 0 && !std::strcmp(listed[0].key, listed[1].key));
    kitems::return_moves(access);
    CHECK(kitems::list(1, listed, 2, &count) == KILN_OK && count == 2);
    kitems::return_post(access, service_lock);
    CHECK(kitems::returns.empty());
    CHECK(kitems::list(1, listed, 2, &count) == KILN_OK && count == 1);
    CHECK(kitems::storage::read<uint32_t>(components[{101, kHashInventory}].data(), 0) == 123);
    kitems::deliver_returns();
    kitems::deliver_returns();
    CHECK(returns_called == 1);
    KilnItemsStorageStatus status{};
    status.size = sizeof status;
    CHECK(kitems::status(&status) == KILN_OK && status.storage && status.persist && status.stage_point &&
          status.registrations == 1);
    kitems::off(KILN_CAP_REASON_SYMBOL_MISSING, "characters/struct.equip_last mismatch");
    CHECK(kitems::status(&status) == KILN_OK && !status.storage && status.reason == KILN_CAP_REASON_SYMBOL_MISSING &&
          std::strstr(status.detail, "equip_last"));
    kitems::storage_reset();
    CHECK(kitems::model.bound() && kitems::model.players.empty() && kitems::model.registrations.size() == 1);
    // Owner-revoke queues native-only retirement; actual freeze/hint/delete happens at Post.
    kitems::enabled = true;
    auto rejoin = kitems::model.join(0, 55, {}, 123, 1, KIS_WITNESS_EXTRA_MAIN_PAGES);
    kitems::model.applied(0, 1);
    CHECK(kitems::model.players.at(0).witness.flags ==
          (KIS_WITNESS_READ | KIS_WITNESS_APPLIED | KIS_WITNESS_EXTRA_MAIN_PAGES));
    CHECK(kitems::restore(0, 0, rejoin, &doc) == KILN_OK);
    Holder live;
    CHECK(create_holder(access, 1, 9, &live));
    kitems::model.mark_built(0, "mod:bag", live.entity, live.pages);
    uint32_t live_stack[]{123, 5, 0};
    std::memcpy(components[{live.pages[0], kHashInventory}].data(), live_stack, 12);
    kitems::storage_owner_retiring(10, nullptr);
    CHECK(!kitems::model.registrations.count("mod:bag") && components.count({live.entity, kHashInventorySetup}) &&
          kitems::parks.size() == 1);
    auto& parked_player = kitems::model.players.at(0);
    const auto hints_before = calls;
    CHECK(kitems::park_holder(access, 0, rejoin, "mod:bag", KIS_PARK_UNREGISTERED, true, service_lock));
    CHECK(calls == hints_before + 1 && !components.count({live.entity, kHashInventorySetup}) &&
          parked_player.holders.empty() && parked_player.entries.size() == 1 &&
          parked_player.entries[0].records.size() == 1);
    auto saved_tag = kitems::model.begin_batch(9, 9, 2);
    kitems::model.capture(0, 456, parked_player.entries, false, 0);
    CHECK(kitems::model.flag_leaving(saved_tag, 0, rejoin));
    kitems::model.complete();
    Frozen saved_copy;
    CHECK(kitems::model.take(saved_tag, &saved_copy) == KILN_OK && saved_copy.entries.size() == 1);
    CHECK(kitems::register_storage(10, &d, &handle) == KILN_ERR_REVOKED);
    kitems::parks.clear();
    CHECK(kitems::storage_work());
    // A held document copy remains parked beside a usable registered empty holder.
    kitems::parks.clear();
    kitems::refs.clear();
    CHECK(kitems::register_storage(11, &d, &handle) == KILN_OK);
    auto held_join = kitems::model.join(0, 55, {}, 789, 3);
    KilnStorageEntry held{};
    held.size = sizeof held;
    held.kind = KIS_ENTRY_PARKED;
    held.slots = 9;
    held.reason = KIS_PARK_RETURNED;
    held.count = 1;
    std::strcpy(held.key, "mod:bag");
    doc.entry_count = 1;
    doc.record_count = 1;
    doc.entries = &held;
    doc.records = &record;
    CHECK(kitems::restore(0, 0, held_join, &doc) == KILN_OK);
    kitems::model.applied(0, 1);
    KilnWorldApi fake_world{};
    fake_world.header = {sizeof fake_world, 1};
    fake_world.game_wrap_info = +[](KilnHandle, KilnGameWrapInfo* out) -> KilnResult {
        out->state = KILN_WRAP_ACTIVE;
        out->disabled = KILN_WRAP_OK;
        return KILN_OK;
    };
    kitems::world_api = &fake_world;
    kitems::death_before_wrap = 1;
    kitems::death_after_wrap = 2;
    uint8_t storage_bytes[16]{};
    uint8_t registry_bytes[16]{};
    void* registry = registry_bytes;
    std::memcpy(storage_bytes, &registry, sizeof registry);
    kitems::structural_access = access;
    kitems::structural_access.storage = storage_bytes;
    E.component_map_find = +[](void*, const uint32_t*) -> uint16_t* {
        static uint16_t index = 0;
        return &index;
    };
    service_lock.unlock();
    kitems::post(nullptr, nullptr);
    s.size = sizeof s;
    CHECK(kitems::state(11, handle, 1, &s) == KILN_OK && s.status == KIS_STATE_READY && s.holder && s.parked == 1 &&
          s.stacks == 0);
    unsigned held_events = 0;
    for (const auto& e : kitems::model.events)
        if (e.kind == KIS_EVENT_PARKED && e.reason == KIS_PARK_RETURNED) {
            ++held_events;
            CHECK(e.handle == handle && e.target == 0 && e.stacks == 1);
        }
    CHECK(held_events == 1 && kitems::model.players.at(0).entries.size() == 1 &&
          kitems::model.players.at(0).entries[0].row.reason == KIS_PARK_RETURNED);
    auto before_repeat = kitems::model.events.size();
    kitems::post(nullptr, nullptr);
    CHECK(kitems::model.events.size() == before_repeat);
    kitems::world_api = nullptr;
    kitems::storage_reset();
}

// ---- review: the per-key parked bound, held reasons, housekeeping events, the guard and callback lifetimes ----
unsigned review_hints = 0;
std::vector<KilnStorageEvent> hint_events;
void (*review_hook)(uint32_t kind) = nullptr;
void KILN_CALL review_stage(uint32_t kind, void* user) {
    ++*static_cast<unsigned*>(user);
    if (kind != KIS_EVENT_BATCH && !kitems::model.events.empty())
        hint_events.push_back(kitems::model.events.back());
    if (review_hook)
        review_hook(kind);
}
KilnStorageRecord review_record(uint16_t slot, uint32_t item, uint16_t count) {
    Record r{};
    r.slot = static_cast<uint8_t>(slot);
    r.item = item;
    r.count = count;
    auto wire = wire_record(r);
    KilnStorageRecord k{};
    k.slot = slot;
    k.item = item;
    k.count = count;
    k.length = static_cast<uint8_t>(wire.size());
    std::memcpy(k.data, wire.data(), wire.size());
    return k;
}
KilnStorageEntry review_entry(const char* key, uint32_t kind, uint32_t slots, uint32_t reason, uint32_t first,
                              uint32_t count) {
    KilnStorageEntry e{};
    e.size = sizeof e;
    e.kind = kind;
    std::snprintf(e.key, sizeof e.key, "%s", key);
    e.slots = slots;
    e.reason = reason;
    e.first = first;
    e.count = count;
    std::fill(std::begin(e.refs), std::end(e.refs), uint16_t(KIS_REF_NONE));
    return e;
}
KilnStorageDoc review_doc(uint64_t account, uint32_t source, KilnStorageEntry* entries, uint32_t entry_count,
                          KilnStorageRecord* records, uint32_t record_count) {
    KilnStorageDoc d{};
    d.size = sizeof d;
    d.source = source;
    d.account = account;
    d.entries = entries;
    d.entry_count = entry_count;
    d.records = records;
    d.record_count = record_count;
    return d;
}
uint32_t review_register(KilnOwner owner, const char* key, uint32_t slots) {
    KilnStorageDef d{};
    d.size = sizeof d;
    d.slots = slots;
    d.key = {key, std::strlen(key)};
    d.filter.size = sizeof d.filter;
    uint32_t handle = 0;
    CHECK(kitems::register_storage(owner, &d, &handle) == KILN_OK && handle);
    return handle;
}
void review_player(uint32_t entity) {
    auto& setup = components[{entity, kHashInventorySetup}];
    setup.assign(84, 0);
    setup[80] = 1;
    setup[83] = 1;
    components[{entity, kHashSlotSelection}].assign(216, 0);
}
std::vector<KilnStorageEvent> events_after(uint64_t seq, const char* key, uint32_t kind, uint32_t player = 0) {
    std::vector<KilnStorageEvent> out;
    for (const auto& e : kitems::model.events)
        if (e.seq > seq && e.kind == kind && !std::strcmp(e.key, key) && (!player || e.player == player))
            out.push_back(e);
    return out;
}
const KilnStorageState* review_row(const KilnStorageState* rows, uint32_t count, const char* key) {
    for (uint32_t i = 0; i < count; ++i)
        if (!std::strcmp(rows[i].key, key))
            return &rows[i];
    return nullptr;
}
void retire_life_one(uint32_t kind) {
    if (kind != KIS_STAGE_RESTORED || std::strcmp(kitems::model.events.back().key, "life:one"))
        return;
    review_hook = nullptr;
    // another thread may retire an owner while the model mutex is released around the hint; nothing may reuse the
    // erased registration's memory before Post continues, so a kept map iterator would fault on the poison
    kitems::storage_owner_retiring(50, nullptr);
}
void review_tests() {
    using namespace kitems;
    using namespace kitems::storage;
    kitems::storage_reset();
    kitems::model = Model{};
    kitems::enabled = true;
    kitems::stage_point = true;
    kitems::garbage.clear();
    kitems::returned.clear();
    CHECK(kitems::bind(0, &review_stage, &review_hints, nullptr) == KILN_OK);
    KilnWorldApi fake_world{};
    fake_world.header = {sizeof fake_world, 1};
    fake_world.game_wrap_info = +[](KilnHandle, KilnGameWrapInfo* out) -> KilnResult {
        out->state = KILN_WRAP_ACTIVE;
        out->disabled = KILN_WRAP_OK;
        return KILN_OK;
    };
    kitems::world_api = &fake_world;
    kitems::death_before_wrap = 1;
    kitems::death_after_wrap = 2;
    uint8_t storage_bytes[16]{};
    uint8_t registry_bytes[16]{};
    void* registry = registry_bytes;
    std::memcpy(storage_bytes, &registry, sizeof registry);
    Access access{};
    access.storage = storage_bytes;
    access.ctx = reinterpret_cast<void*>(2);
    access.job = reinterpret_cast<void*>(3);
    for (int i = 0; i < A_COUNT; ++i)
        access.acc[i] = i == A_SETUP ? kHashInventorySetup : i == A_INVENTORY ? kHashInventory : kHashSlotSelection;
    kitems::structural_access = access;
    E.component_map_find = +[](void*, const uint32_t*) -> uint16_t* {
        static uint16_t index = 0;
        return &index;
    };
    item_bytes[E.item_flags] = 0;

    // 1. A live storage of 64 stacks and a held bag of the same key; the mod unloads. The held bag keeps its reason,
    //    the live records join it in free cells up to 64, the rest is quarantined, and nothing comes back by itself.
    review_player(1);
    review_register(20, "mod:bag", 64);
    auto join0 = kitems::model.join(0, 55, {}, 1, 1);
    KilnStorageRecord held_record = review_record(0, 123, 5);
    KilnStorageEntry held_entry = review_entry("mod:bag", KIS_ENTRY_PARKED, 9, KIS_PARK_RETURNED, 0, 1);
    auto doc0 = review_doc(55, KIS_SOURCE_SERVER, &held_entry, 1, &held_record, 1);
    CHECK(kitems::restore(0, 0, join0, &doc0) == KILN_OK);
    kitems::model.applied(0, 1);
    kitems::post(nullptr, nullptr);
    CHECK(kitems::model.players.at(0).holders.count("mod:bag") == 1);
    const Holder live = kitems::model.players.at(0).holders.at("mod:bag");
    CHECK(live.pages.size() == 8);
    for (size_t page = 0; page < live.pages.size(); ++page)
        for (uint32_t cell = 0; cell < 8; ++cell) {
            uint32_t stack[]{123, static_cast<uint32_t>(1 + page * 8 + cell), 0};
            std::memcpy(components[{live.pages[page], kHashInventory}].data() + cell * 12, stack, 12);
        }
    kitems::storage_owner_retiring(20, nullptr);
    {
        // a capture between the unload and the next Post already obeys the bound
        std::vector<Entry> frozen;
        EntityRef ref{1, 0, 0, 0, storage_bytes, nullptr};
        std::lock_guard lock(kitems::storage_mu);
        CHECK(kitems::freeze(kitems::model.players.at(0), nullptr, kitems::bindings.data(), &ref, &frozen));
        unsigned parked_entries = 0, parked = 0, full = 0;
        for (const auto& e : frozen) {
            if (e.row.kind == KIS_ENTRY_PARKED && !std::strcmp(e.row.key, "mod:bag")) {
                ++parked_entries;
                parked += static_cast<unsigned>(e.records.size());
                CHECK(e.row.reason == KIS_PARK_RETURNED);
            }
            if (e.row.kind == KIS_ENTRY_QUARANTINE)
                for (const auto& r : e.records)
                    full += r.reason == 6;
        }
        CHECK(parked_entries == 1 && parked == 64 && full == 1);
    }
    kitems::post(nullptr, nullptr);
    {
        const auto& entries = kitems::model.players.at(0).entries;
        unsigned parked_entries = 0, total = 0, full = 0;
        std::set<unsigned> cells;
        for (const auto& e : entries) {
            total += static_cast<unsigned>(e.records.size());
            if (e.row.kind == KIS_ENTRY_PARKED) {
                ++parked_entries;
                CHECK(e.row.reason == KIS_PARK_RETURNED && e.records.size() == 64 && e.records[0].count == 5);
                for (const auto& r : e.records)
                    cells.insert(r.slot);
            } else
                for (const auto& r : e.records)
                    full += r.reason == 6;
        }
        CHECK(parked_entries == 1 && cells.size() == 64 && full == 1 && total == 65);
        CHECK(kitems::model.players.at(0).holders.empty() && !components.count({live.entity, kHashInventorySetup}));
    }
    KilnStorageState rows[8]{};
    rows[0].size = sizeof rows[0];
    uint32_t count = 0;
    CHECK(kitems::list(1, rows, 8, &count) == KILN_OK);
    auto bag = review_row(rows, count, "mod:bag");
    CHECK(bag && bag->status == KIS_STATE_PARKED && bag->parked == 64 && bag->quarantined == 1);
    KilnItemsStorageStatus overall{};
    overall.size = sizeof overall;
    CHECK(kitems::status(&overall) == KILN_OK && overall.parked == 64 && overall.quarantined == 1);
    auto bag_again = review_register(21, "mod:bag", 64);
    kitems::post(nullptr, nullptr);
    kitems::post(nullptr, nullptr);
    KilnStorageState state{};
    state.size = sizeof state;
    CHECK(kitems::state(21, bag_again, 1, &state) == KILN_OK && state.status == KIS_STATE_READY && state.stacks == 0 &&
          state.parked == 64 && state.quarantined == 1);

    // 2. Hot reload: the old owner retires and the key is registered again before the next Post. Tearing down the old
    //    holder is housekeeping (handle 0, Target = the old holder); the new holder's RESTORED is public.
    review_player(2);
    review_register(30, "mod:pack", 8);
    auto join1 = kitems::model.join(1, 66, {}, 5, 1);
    auto doc1 = review_doc(66, KIS_SOURCE_NEW, nullptr, 0, nullptr, 0);
    CHECK(kitems::restore(0, 1, join1, &doc1) == KILN_OK);
    kitems::model.applied(1, 2);
    kitems::post(nullptr, nullptr);
    const Holder pack = kitems::model.players.at(1).holders.at("mod:pack");
    uint32_t pack_stack[]{123, 3, 0};
    std::memcpy(components[{pack.pages[0], kHashInventory}].data(), pack_stack, 12);
    kitems::storage_owner_retiring(30, nullptr);
    auto pack_again = review_register(31, "mod:pack", 8);
    auto before = kitems::model.last_event();
    kitems::post(nullptr, nullptr);
    auto parked_events = events_after(before, "mod:pack", KIS_EVENT_PARKED, 2);
    CHECK(parked_events.size() == 1 && parked_events[0].handle == 0 && parked_events[0].target == pack.entity);
    auto restored_events = events_after(before, "mod:pack", KIS_EVENT_RESTORED, 2);
    CHECK(!restored_events.empty() && restored_events[0].handle == pack_again && restored_events[0].target &&
          restored_events[0].target != pack.entity);
    CHECK(kitems::state(31, pack_again, 2, &state) == KILN_OK && state.stacks == 1 && state.parked == 0);

    // 3. A live copy of a key no mod registered: one public PARKED (Target 0) at the first restore, never two.
    review_player(3);
    auto join2 = kitems::model.join(2, 77, {}, 9, 1);
    KilnStorageRecord gone_record = review_record(0, 123, 2);
    KilnStorageEntry gone_entry = review_entry("gone:bag", KIS_ENTRY_LIVE, 8, 0, 0, 1);
    auto doc2 = review_doc(77, KIS_SOURCE_SERVER, &gone_entry, 1, &gone_record, 1);
    CHECK(kitems::restore(0, 2, join2, &doc2) == KILN_OK);
    kitems::model.applied(2, 3);
    before = kitems::model.last_event();
    kitems::post(nullptr, nullptr);
    auto gone_events = events_after(before, "gone:bag", KIS_EVENT_PARKED);
    CHECK(gone_events.size() == 1 && gone_events[0].target == 0 && gone_events[0].handle == 0 &&
          gone_events[0].reason == KIS_PARK_UNREGISTERED && gone_events[0].stacks == 1);
    rows[0].size = sizeof rows[0];
    CHECK(kitems::list(3, rows, 8, &count) == KILN_OK);
    auto gone = review_row(rows, count, "gone:bag");
    CHECK(gone && gone->status == KIS_STATE_PARKED && gone->parked == 1 && gone->holder == 0);

    // 4. Its parked return: the temporary holder's RESTORED and PARKED are housekeeping (handle 0, Target set).
    uint64_t ticket = 0;
    CHECK(kitems::return_parked(60, 3, {"gone:bag", 8}, 100, nullptr, nullptr, &ticket) == KILN_OK && ticket);
    before = kitems::model.last_event();
    kitems::post(nullptr, nullptr);
    auto temp_restored = events_after(before, "gone:bag", KIS_EVENT_RESTORED);
    CHECK(temp_restored.size() == 1 && temp_restored[0].handle == 0 && temp_restored[0].target);
    kitems::storage_program(access);
    before = kitems::model.last_event();
    kitems::post(nullptr, nullptr);
    auto temp_parked = events_after(before, "gone:bag", KIS_EVENT_PARKED);
    CHECK(temp_parked.size() == 1 && temp_parked[0].handle == 0 && temp_parked[0].target == temp_restored[0].target);

    // 5. The guard keeps refusing when one tracked page cannot be read (the game may already have removed it).
    kitems::model.mark_built(2, "fake:bag", 9001, {9002});
    kitems::client_actions.clear();
    ClientActionLayout action{};
    action.offset = 0;
    action.version = 32;
    action.consumed = 0;
    action.size = 32;
    action.refs = {4};
    kitems::client_actions.push_back(action);
    kitems::input_size = 40;
    kitems::consumed_size = 4;
    auto& input = components[{2, kitems::hash_name("PlayerInput")}];
    input.assign(40, 0);
    uint32_t version = 5, target_page = kitems::model.players.at(1).holders.at("mod:pack").pages[0], consumed = 4;
    std::memcpy(input.data() + 32, &version, 4);
    std::memcpy(input.data() + 4, &target_page, 4);
    auto& used = components[{2, kitems::hash_name("ServerConsumedPlayerInput")}];
    used.assign(4, 0);
    std::memcpy(used.data(), &consumed, 4);
    kitems::guard_client_inputs(server() + E.world_storage);
    std::memcpy(&consumed, components[{2, kitems::hash_name("ServerConsumedPlayerInput")}].data(), 4);
    CHECK(consumed == 5 && kitems::enabled);
    kitems::enabled = true;
    kitems::model.players.at(2).holders.erase("fake:bag");
    kitems::client_actions.clear();

    // 6. A callback that retires the owner of the key being built (and registers another) must not break the pass.
    review_player(4);
    review_register(50, "life:one", 8);
    auto life_two = review_register(51, "life:two", 8);
    auto join3 = kitems::model.join(3, 88, {}, 11, 1);
    auto doc3 = review_doc(88, KIS_SOURCE_NEW, nullptr, 0, nullptr, 0);
    CHECK(kitems::restore(0, 3, join3, &doc3) == KILN_OK);
    kitems::model.applied(3, 4);
    review_hook = &retire_life_one;
    kitems::post(nullptr, nullptr);
    review_hook = nullptr;
    kitems::post(nullptr, nullptr);
    kitems::post(nullptr, nullptr);
    CHECK(kitems::state(51, life_two, 4, &state) == KILN_OK && state.status == KIS_STATE_READY);
    CHECK(!kitems::model.players.at(3).holders.count("life:one") && kitems::model.players.at(3).holders.count("life:two"));

    // 6b. Clearing a storage's reference never clears a reference the player has since pointed at another cell.
    const uint64_t bag_cell = KILN_SLOT_ID(5555, 2);
    std::memcpy(components[{4, kHashSlotSelection}].data() + 14 * 8, &bag_cell, 8);
    CHECK(kitems::set_ref(51, life_two, 4, 14, KIS_REF_CLEAR) == KILN_OK);
    kitems::storage_program(access);
    uint64_t selected = 0;
    std::memcpy(&selected, components[{4, kHashSlotSelection}].data() + 14 * 8, 8);
    CHECK(selected == bag_cell);
    CHECK(kitems::set_ref(51, life_two, 4, 14, 3) == KILN_OK);
    kitems::storage_program(access);
    std::memcpy(&selected, components[{4, kHashSlotSelection}].data() + 14 * 8, 8);
    const auto& life_holder = kitems::model.players.at(3).holders.at("life:two");
    CHECK(selected == KILN_SLOT_ID(life_holder.pages[0], 3) && life_holder.refs[KIS_REF_ARROW] == 3);
    CHECK(kitems::set_ref(51, life_two, 4, 14, KIS_REF_CLEAR) == KILN_OK);
    kitems::storage_program(access);
    std::memcpy(&selected, components[{4, kHashSlotSelection}].data() + 14 * 8, 8);
    CHECK(selected == 0 && kitems::model.players.at(3).holders.at("life:two").refs[KIS_REF_ARROW] == KIS_REF_NONE);

    // 7. A character read at a join that does not parse: storage goes off (a layout mismatch, with a caps reason) and
    //    the slot's previous character keeps nothing a new occupant of the slot could use or pair.
    std::vector<uint8_t> character(0x5500);
    character[S.layout.main_pages] = 17;
    const auto old_holder = kitems::model.players.at(1).holders.at("mod:pack").entity;
    kitems::after_read(1, character.data());
    CHECK(!kitems::enabled && (kitems::model.players.at(1).witness.flags & KIS_WITNESS_LEFT) &&
          kitems::model.players.at(1).holders.empty() && kitems::garbage_contains(old_holder) &&
          kitems::detail.find("characters/struct") != std::string::npos);

    kitems::enabled = true;
    kitems::world_api = nullptr;
    kitems::storage_reset();
}
std::vector<std::string> caps_reported;
KilnResult KILN_CALL record_caps(KilnOwner, KilnStr name, uint32_t, uint32_t, KilnStr) {
    caps_reported.emplace_back(name.ptr, name.len);
    return KILN_OK;
}
// items.storage.client belongs to the managed kiln:storage mirror; the native service must not report it as well.
KilnPlayersApi fake_players{};
bool leaving_player_gone = false;
KilnResult KILN_CALL leaving_player_get(KilnEntity entity, KilnPlayer* out) {
    if (leaving_player_gone || entity != 1)
        return KILN_ERR_NOT_FOUND;
    out->state = KILN_PLAYER_LEAVING;
    return KILN_OK;
}
// A pre-create gate denial is the only failure with proof that no native mutation ran.
void holder_retry_tests() {
    kitems::storage_reset();
    kitems::model = Model{};
    kitems::enabled = true;
    CHECK(kitems::bind(0, &review_stage, &review_hints, nullptr) == KILN_OK);
    KilnWorldApi world{};
    world.header = {sizeof world, 1};
    world.game_wrap_info = +[](KilnHandle, KilnGameWrapInfo* out) -> KilnResult {
        out->state = KILN_WRAP_ACTIVE;
        out->disabled = KILN_WRAP_OK;
        return KILN_OK;
    };
    kitems::world_api = &world;
    kitems::death_before_wrap = 1;
    kitems::death_after_wrap = 2;
    uint8_t storage_bytes[16]{};
    void* registry = storage_bytes + 8;
    std::memcpy(storage_bytes, &registry, sizeof registry);
    E.component_map_find = +[](void*, const uint32_t*) -> uint16_t* {
        static uint16_t index = 0;
        return &index;
    };
    auto prepare = [&] {
        kitems::structural_access.storage = storage_bytes;
        review_player(1);
        auto join = kitems::model.join(0, 55, {}, 19, 1);
        auto entry = review_entry("retry:bag", KIS_ENTRY_PARKED, 9, KIS_PARK_HELD, 0, 1);
        auto record = review_record(0, 123, 5);
        auto doc = review_doc(55, KIS_SOURCE_SERVER, &entry, 1, &record, 1);
        CHECK(kitems::restore(0, 0, join, &doc) == KILN_OK);
        kitems::model.applied(0, 1);
        return join;
    };
    review_register(80, "retry:bag", 9);
    prepare();
    auto before = creates;
    auto contents = entries_hash(kitems::model.players.at(0).entries);
    gate_refusals = 1;
    kitems::post(nullptr, nullptr);
    CHECK(kitems::enabled && creates == before && kitems::model.players.at(0).holders.empty());
    CHECK(entries_hash(kitems::model.players.at(0).entries) == contents);
    kitems::post(nullptr, nullptr);
    CHECK(kitems::enabled && creates == before + 3 && kitems::model.players.at(0).holders.count("retry:bag"));
    kitems::post(nullptr, nullptr);
    CHECK(creates == before + 3); // no duplicate holder

    // A second denial consumes the single retry; no busy loop or native work.
    kitems::storage_reset();
    prepare();
    before = creates;
    gate_refusals = 2;
    kitems::post(nullptr, nullptr);
    CHECK(kitems::enabled);
    kitems::post(nullptr, nullptr);
    CHECK(!kitems::enabled && creates == before);
    kitems::post(nullptr, nullptr);
    CHECK(creates == before);
    gate_refusals = 0;

    // Access/world switched under a pending retry: fail closed before any native call.
    kitems::enabled = true;
    kitems::storage_reset();
    prepare();
    before = creates;
    gate_refusals = 1;
    kitems::post(nullptr, nullptr);
    CHECK(kitems::enabled && creates == before);
    uint8_t other_bytes[16]{};
    void* other_registry = other_bytes + 8;
    std::memcpy(other_bytes, &other_registry, sizeof other_registry);
    kitems::structural_access.storage = other_bytes;
    kitems::post(nullptr, nullptr);
    CHECK(!kitems::enabled && creates == before && kitems::model.players.at(0).holders.empty());
    CHECK(kitems::detail.find("world/access changed") != std::string::npos);
    gate_refusals = 0;

    // Unload and world reset cancel pending work; a reused slot is a new join, not the old retry.
    kitems::enabled = true;
    kitems::storage_reset();
    const auto old_join = prepare();
    gate_refusals = 1;
    kitems::post(nullptr, nullptr);
    kitems::storage_cancel_owner(80);
    before = creates;
    kitems::post(nullptr, nullptr);
    CHECK(creates == before);
    kitems::storage_reset();
    CHECK(!kitems::structural_access.storage && kitems::model.players.empty());
    kitems::post(nullptr, nullptr);
    CHECK(creates == before);
    review_register(81, "retry:bag", 9);
    CHECK(prepare() != old_join);
    kitems::post(nullptr, nullptr);
    CHECK(kitems::enabled && creates == before + 3);
    // Failure after a native call is ambiguous, even when known-id cleanup reports success.
    // Keep persisted held/quarantine rows, witness/revision pairing, and another valid holder untouched.
    struct FailureCase {
        unsigned nth, lookup;
        uint32_t raw;
        bool fault, mutate, cleanup_fault;
        uint8_t cleanup_raw;
        const char* stage;
    };
    const FailureCase cases[]{
        {1, 0, 0, false, true, false, 1, "root-create"},
        {1, 0, 1, false, false, false, 1, "root-create"},
        {1, 0, UINT32_MAX, false, false, false, 1, "root-create"},
        {1, 0, 0, true, true, false, 1, "root-create"},
        {3, 0, 0, false, true, false, 7, "page-create"},
        {3, 0, 0, true, true, false, 1, "page-create"},
        {3, 0, 0, false, false, false, 0, "page-create"},
        {3, 0, 0, false, false, true, 1, "page-create"},
        {0, 1, 0, false, false, false, 1, "setup-lookup"},
        {0, 2, 0, false, false, false, 1, "setup-lookup"},
        {0, 3, 0, false, false, false, 1, "setup-lookup"},
        {0, 4, 0, false, false, false, 1, "setup-copy"},
    };
    readonly_setup = VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    CHECK(readonly_setup);
    DWORD old_protection = 0;
    CHECK(VirtualProtect(readonly_setup, 4096, PAGE_READONLY, &old_protection));
    for (const auto& c : cases) {
        std::printf("  holder failure: %s nth=%u lookup=%u raw=%u fault=%d cleanup_raw=%u cleanup_fault=%d\n",
                    c.stage, c.nth, c.lookup, c.raw, c.fault, c.cleanup_raw, c.cleanup_fault);
        kitems::storage_reset();
        kitems::enabled = true;
        prepare();
        auto& p = kitems::model.players.at(0);
        p.paired = true;
        p.revision = 61;
        auto quarantined = entry_for("retry:bag", KIS_ENTRY_QUARANTINE, 9, 0);
        auto record = review_record(2, 123, 2);
        record.reason = KIS_QUARANTINE_COUNT;
        quarantined.records.push_back(record);
        p.entries.push_back(quarantined);
        Holder unrelated;
        CHECK(create_holder(kitems::structural_access, 1, 8, &unrelated));
        p.holders.emplace("other:valid", unrelated);
        const auto unrelated_setup = components.at({unrelated.entity, kHashInventorySetup});
        const auto unrelated_page = components.at({unrelated.pages[0], kHashInventory});
        const auto witness_before = p.witness;
        const auto persisted = entries_hash(p.entries);
        const auto snapshot = components;
        before = creates;
        create_fail_at = c.nth ? creates + c.nth : 0;
        create_raw = c.raw;
        create_fault = c.fault;
        mutate_before_failure = c.mutate;
        destroy_fault = c.cleanup_fault;
        destroy_raw = c.cleanup_raw;
        setup_lookup_mode = c.lookup;
        kitems::post(nullptr, nullptr);
        CHECK(!kitems::enabled && p.holders.size() == 1 && p.holders.at("other:valid").entity == unrelated.entity);
        CHECK(entries_hash(p.entries) == persisted && p.paired && p.revision == 61 &&
              p.witness.witness == witness_before.witness && p.witness.join == witness_before.join);
        CHECK(components.at({unrelated.entity, kHashInventorySetup}) == unrelated_setup &&
              components.at({unrelated.pages[0], kHashInventory}) == unrelated_page);
        CHECK(kitems::detail.find(std::string("stage=") + c.stage) != std::string::npos);
        CHECK(kitems::detail.find("cleanup=") != std::string::npos);
        // #135: the one log line / capability text must name the native outcome and raw result
        CHECK(kitems::detail.find(" native=") != std::string::npos && kitems::detail.find(" raw=") != std::string::npos);
        CHECK(std::any_of(storage_logs.begin(), storage_logs.end(), [&](const std::string& line) {
            return line.find(std::string("stage=") + c.stage) != std::string::npos;
        }));
        const auto failed_creates = creates;
        kitems::post(nullptr, nullptr);
        CHECK(creates == failed_creates && creates == before + (c.nth ? c.nth : 3));
        create_fail_at = 0;
        create_fault = mutate_before_failure = destroy_fault = false;
        destroy_raw = 1;
        setup_lookup_mode = 0;
        components = snapshot; // only the fake ECS; never a game world
    }
    CHECK(VirtualFree(readonly_setup, 0, MEM_RELEASE));
    readonly_setup = nullptr;
    kitems::world_api = nullptr;
    kitems::storage_reset();
}

// The leave timeout: a leaving player whose entity still exists keeps its holders past 250 steps (the game's leave save
// can still capture it); once the entity is gone they go, and the slot is marked left.
void leave_timeout_tests() {
    using namespace kitems;
    using namespace kitems::storage;
    kitems::api = &fake_api;
    kitems::world_api = nullptr;
    kitems::model = Model{};
    kitems::garbage.clear();
    kitems::enabled = true;
    kitems::baseline_authored = true;
    fake_players.get = &leaving_player_get;
    kitems::players_api = &fake_players;
    uint32_t handle = 0;
    KilnStorageDef d{};
    d.size = sizeof d;
    d.slots = 8;
    d.key = {"mod:bag", 7};
    d.filter.size = sizeof d.filter;
    CHECK(kitems::model.register_def(10, d, &handle) == KILN_OK);
    kitems::model.join(0, 55, {}, 1, 1);
    kitems::model.applied(0, 1);
    kitems::model.mark_built(0, "mod:bag", 4100, {4101});
    components[{1, kHashInventorySetup}].assign(84, 0);
    kitems::storage_pre(1000); // LEAVING: the leave starts here
    CHECK(kitems::model.players.at(0).leaving && kitems::garbage.empty());
    kitems::storage_pre(1300); // past 250 steps, the entity still exists: kept
    CHECK(kitems::garbage.empty() && kitems::model.players.at(0).holders.count("mod:bag") &&
          !(kitems::model.players.at(0).witness.flags & KIS_WITNESS_LEFT));
    components.erase({1, kHashInventorySetup});
    leaving_player_gone = true;
    kitems::storage_pre(1302); // the entity is gone: torn down
    CHECK(kitems::garbage.size() == 1 && kitems::model.players.at(0).holders.empty() &&
          (kitems::model.players.at(0).witness.flags & KIS_WITNESS_LEFT));
    kitems::garbage.clear();
    kitems::players_api = nullptr;
    leaving_player_gone = false;
}
void init_caps_test() {
    fake_caps.header = {sizeof fake_caps, 1};
    fake_caps.report = &record_caps;
    fake_api.feature = +[](KilnStr) -> bool { return false; };
    fake_api.subscribe_tick = +[](KilnOwner, int32_t, KilnTickFn, void*, KilnHandle* out) -> KilnResult {
        *out = 1;
        return KILN_OK;
    };
    fake_api.register_service = +[](KilnOwner, KilnStr, const void*) -> KilnResult { return KILN_OK; };
    kitems::storage_init();
    CHECK(std::count(caps_reported.begin(), caps_reported.end(), "items.storage") == 1);
    CHECK(std::count(caps_reported.begin(), caps_reported.end(), "items.storage.client") == 0);
    fake_caps = {};
}

// ---- The client inventory guard at storage_ready (kiln#135) ----
// A catalog shaped like both approved builds' PlayerInput: the nine inventory-addressed actions with their versionData,
// their refs and their consumed versions, plus members that are not actions (an enum with two values, a bitmask
// typedef, a bool) between them, as ClientPlayerInputData has.
const char* const kGuarded[]{"inventoryTransferAction", "sortInventoryAction",   "areaDepositPermissionAction",
                             "upgradeStorageAction",    "changeEquipmentAction", "swapEquipmentSetAction",
                             "itemAction",              "weaponGemAction",       "playerCraftingAction"};
std::string consumed_name(const std::string& action) {
    std::string c = "consumed" + action;
    c[8] = static_cast<char>(c[8] - 32);
    return c;
}
void guard_catalog(fake_types::Catalog& c, const char* drop_action = nullptr, const char* drop_consumed = nullptr) {
    c.add("keen::uint8", 1, KILN_TYPE_U8);
    c.add("keen::uint32", 4, KILN_TYPE_U32);
    c.add("keen::Time", 8, KILN_TYPE_U64);
    c.add("keen::bool", 1, KILN_TYPE_BOOL);
    c.add("keen::ecs::DirectionalInputMode", 1, KILN_TYPE_ENUM, 2); // two values, no fields
    c.add("keen::ecs::ClientPlayerStateMask", 2, KILN_TYPE_TYPEDEF);
    c.add_struct("keen::VersionedData", 4, {{"version", "keen::uint32", 0}});
    c.add_struct("keen::ecs::EntityId", 4, {{"id", "keen::uint32", 0}});
    c.add_struct("keen::InventorySlotId", 8, {{"entityId", "keen::ecs::EntityId", 0}, {"slotIndex", "keen::uint32", 4}});
    std::vector<fake_types::Field> group, used;
    uint64_t at = 0;
    for (const char* name : kGuarded) {
        const std::string type = std::string("keen::ecs::T_") + name;
        c.add_struct(type, 16,
                     {{"versionData", "keen::VersionedData", 0},
                      {"targetEntityId", "keen::ecs::EntityId", 4},
                      {"slotId", "keen::InventorySlotId", 8}});
        if (!drop_action || std::strcmp(drop_action, name))
            group.push_back({name, type, at});
        if (!drop_consumed || std::strcmp(drop_consumed, name))
            used.push_back({consumed_name(name), "keen::VersionedData", 4 * used.size()});
        at += 16;
        if (!std::strcmp(name, "sortInventoryAction")) { // non-actions between the actions
            group.push_back({"movementStickInputMode", "keen::ecs::DirectionalInputMode", at});
            group.push_back({"mask", "keen::ecs::ClientPlayerStateMask", at + 2});
            group.push_back({"isRespawnChargedFully", "keen::bool", at + 4});
            at += 8;
        }
    }
    c.add_struct("keen::ecs::ClientPlayerInputData", static_cast<uint32_t>(at), group);
    c.add_struct("keen::ecs::PlayerInput", static_cast<uint32_t>(at + 8),
                 {{"serverTime", "keen::Time", 0}, {"fromClient", "keen::ecs::ClientPlayerInputData", 8}});
    c.add_struct("keen::ecs::ServerConsumedPlayerInput", static_cast<uint32_t>(4 * used.size()), used);
}
// DeliverEvents' first bytes on both approved builds, and what a Kiln detour (another owner's raw link) leaves there.
uint8_t deliver_code[32]{0x40, 0x56, 0x41, 0x55, 0x41, 0x56, 0x48, 0x83, 0xec, 0x40, 0x48, 0x8b, 0x31, 0x45, 0x33, 0xed};
uint8_t deliver_file[32]{0x40, 0x56, 0x41, 0x55, 0x41, 0x56, 0x48, 0x83, 0xec, 0x40, 0x48, 0x8b, 0x31, 0x45, 0x33, 0xed};
bool client_hold = true, deliver_known = true;
uint32_t deliver_state = KILN_CODE_MANAGED;
KilnResult detour_rc = KILN_OK;
unsigned detours = 0, continued = 0;
uintptr_t detour_target = 0;
std::string detour_label;
void* detour_fn = nullptr;
uintptr_t __fastcall deliver_next(uintptr_t a, uintptr_t, uintptr_t, uintptr_t) {
    ++continued;
    return a + 1;
}
KilnResult KILN_CALL guard_symbol(KilnStr name, uintptr_t* out) {
    if (!deliver_known || std::string(name.ptr, name.len) != "gameplay/DeliverEvents")
        return KILN_ERR_NOT_FOUND;
    *out = reinterpret_cast<uintptr_t>(deliver_code);
    return KILN_OK;
}
bool KILN_CALL guard_feature(KilnStr name) {
    return client_hold && std::string(name.ptr, name.len) == "players.client_action_hold";
}
KilnResult KILN_CALL guard_code_state(uintptr_t at, size_t n, uint32_t* state, uint64_t* generation) {
    CHECK(at == reinterpret_cast<uintptr_t>(deliver_code) && n == 16);
    *state = deliver_state;
    *generation = 1;
    return KILN_OK;
}
// SEH-guarded like kiln.hooks' read: a jump the refusal follows may lead anywhere.
bool guarded_copy(void* out, uintptr_t at, size_t n) {
    __try {
        std::memcpy(out, reinterpret_cast<const void*>(at), n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
KilnResult KILN_CALL guard_read(uintptr_t at, void* out, size_t n) {
    return guarded_copy(out, at, n) ? KILN_OK : KILN_ERR_NOT_FOUND;
}
KilnResult KILN_CALL guard_read_original(uintptr_t at, void* out, size_t n) {
    std::memcpy(out, deliver_file + (at - reinterpret_cast<uintptr_t>(deliver_code)), n);
    return KILN_OK;
}
KilnResult KILN_CALL guard_raw_detour(KilnOwner, uintptr_t target, const KilnHookOrder* order, void* fn,
                                      void** original, KilnHandle* out) {
    ++detours;
    detour_target = target;
    detour_label = order && order->label.ptr ? std::string(order->label.ptr, order->label.len) : std::string();
    detour_fn = fn;
    if (detour_rc != KILN_OK)
        return detour_rc;
    *original = reinterpret_cast<void*>(&deliver_next);
    *out = 77;
    return KILN_OK;
}
KilnResult KILN_CALL guard_retiring(KilnOwner, KilnOwnerRetiringFn, void*, KilnHandle* out) {
    *out = 5;
    return KILN_OK;
}
void unused_member() {}
template <class T> void set(T& member) { member = reinterpret_cast<T>(&unused_member); }
KilnWorldApi guard_world{};
KilnTypesApi guard_types{};
KilnPlayersApi guard_players{};
KilnSavesApi guard_saves{};
KilnHooksApi guard_hooks{};
std::vector<std::pair<std::string, std::string>> guard_caps; // name, text
KilnResult KILN_CALL guard_report(KilnOwner, KilnStr name, uint32_t, uint32_t, KilnStr text) {
    guard_caps.emplace_back(std::string(name.ptr, name.len), std::string(text.ptr, text.len));
    return KILN_OK;
}
const void* KILN_CALL guard_service(KilnStr n, uint32_t) {
    const std::string name(n.ptr, n.len);
    if (name == KILN_WORLD_SERVICE)
        return &guard_world;
    if (name == KILN_TYPES_SERVICE)
        return &guard_types;
    if (name == KILN_PLAYERS_SERVICE)
        return &guard_players;
    if (name == KILN_SAVES_SERVICE)
        return &guard_saves;
    if (name == KILN_HOOKS_SERVICE)
        return &guard_hooks;
    if (name == KILN_CAPS_SERVICE)
        return &fake_caps;
    return nullptr;
}
// One storage_ready from a storage that storage_init enabled; the detail it leaves.
std::string run_ready(fake_types::Catalog& catalog) {
    guard_types = fake_types::api(&catalog);
    set(guard_types.in_system_callback);
    guard_types.header.size = sizeof guard_types;
    kitems::enabled = true;
    kitems::detail = "waiting for services";
    kitems::reason = KILN_CAP_REASON_NONE;
    kitems::client_actions.clear();
    detours = 0;
    guard_caps.clear();
    kitems::storage_ready();
    return kitems::detail;
}
bool has(const std::string& text, const char* part) { return text.find(part) != std::string::npos; }
void guard_ready_tests() {
    using namespace kitems;
    const auto saved_api = fake_api;
    const auto saved_caps = fake_caps;
    fake_api.service = &guard_service;
    fake_api.symbol = &guard_symbol;
    fake_api.feature = &guard_feature;
    fake_caps.header = {sizeof fake_caps, 1};
    fake_caps.report = &guard_report;
    guard_world.header = {sizeof guard_world, 1};
    set(guard_world.in_program);
    set(guard_world.game_program_find);
    set(guard_world.game_program_list);
    set(guard_world.game_program_wrap);
    set(guard_world.game_wrap_info);
    guard_players.header = {sizeof guard_players, 1};
    set(guard_players.get);
    set(guard_players.status);
    guard_saves.header = {sizeof guard_saves, 1};
    set(guard_saves.next_event);
    guard_hooks.header = {sizeof guard_hooks, 2};
    guard_hooks.subscribe_owner_retiring = &guard_retiring;
    guard_hooks.code_state = &guard_code_state;
    guard_hooks.read = &guard_read;
    guard_hooks.read_original = &guard_read_original;
    guard_hooks.raw_detour = &guard_raw_detour;
    // Another Kiln owner (kiln-players) raw-linked DeliverEvents first: its bytes are the registry's detour.
    const uint8_t kiln_detour[6]{0xe9, 0x10, 0x20, 0x30, 0x40, 0x90};
    std::memcpy(deliver_code, kiln_detour, sizeof kiln_detour);

    // 1. Both builds' layout (non-action members between the actions) with DeliverEvents already in Kiln's registry:
    //    the guard joins that chain and storage is ready.
    {
        fake_types::Catalog c;
        guard_catalog(c);
        const auto why = run_ready(c);
        CHECK(kitems::enabled && detours == 1 && detour_target == reinterpret_cast<uintptr_t>(deliver_code) &&
              detour_label == "items/StorageClientGuard");
        CHECK(kitems::client_actions.size() == 9 && kitems::input_size == c.find("keen::ecs::PlayerInput")->size &&
              kitems::consumed_size == 36);
        if (!kitems::enabled)
            std::printf("  storage_ready left: %s\n", why.c_str());
        // kiln-shroudlink hooks DeliverEvents later, in front of the registry's detour: nothing checks again, and the
        // chain still reaches the rest of the chain (and the game) through the guard.
        deliver_state = KILN_CODE_FOREIGN;
        continued = 0;
        if (detour_fn) {
            auto guard = reinterpret_cast<uintptr_t(__fastcall*)(uintptr_t, uintptr_t, uintptr_t, uintptr_t)>(detour_fn);
            CHECK(guard(41, 0, 0, 0) == 42 && continued == 1 && kitems::enabled);
        } else
            CHECK(detour_fn != nullptr);
        deliver_state = KILN_CODE_MANAGED;
    }
    // 2. Each refusal says which check failed; the caps reason stays symbol-missing.
    const char* prefix = "items.storage: client inventory guard is unavailable: ";
    {
        fake_types::Catalog c;
        guard_catalog(c);
        client_hold = false;
        const auto why = run_ready(c);
        client_hold = true;
        CHECK(!kitems::enabled && detours == 0 && kitems::reason == KILN_CAP_REASON_SYMBOL_MISSING &&
              why == std::string(prefix) + "sigdb feature players.client_action_hold is off on this build");
        CHECK(!guard_caps.empty() && guard_caps.back().first == "items.storage" && guard_caps.back().second == why);
    }
    {
        fake_types::Catalog c;
        guard_catalog(c);
        deliver_known = false;
        const auto why = run_ready(c);
        deliver_known = true;
        CHECK(!kitems::enabled && detours == 0 && why == std::string(prefix) + "symbol missing: gameplay/DeliverEvents");
    }
    {
        // A Kiln detour placed over another module's patch (Kiln's E9 in front): refused, with the bytes Kiln saw and
        // what to do; the jump into memory nothing maps is followed no further.
        fake_types::Catalog c;
        guard_catalog(c);
        deliver_state = KILN_CODE_FOREIGN;
        const auto why = run_ready(c);
        deliver_state = KILN_CODE_MANAGED;
        CHECK(!kitems::enabled && detours == 0 && has(why, prefix) && has(why, "gameplay/DeliverEvents") &&
              has(why, "outside Kiln's hook registry") && has(why, "live e9 10 20 30 40 90 48 83") &&
              has(why, "file 40 56 41 55 41 56 48 83") && has(why, "(in no module") &&
              has(why, "Remove that mod or loader"));
        std::printf("  foreign: %s\n", why.c_str());
    }
    {
        // Another module's hook in retour's shape (shroudlink's own core): jmp [rip+0] to a relay in private memory,
        // whose jmp [rip+0] goes to the hook in its module (this test's exe stands in for that DLL). The refusal names
        // the module, not the relay, and says what to do.
        fake_types::Catalog c;
        guard_catalog(c);
        auto* relay = static_cast<uint8_t*>(VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
        CHECK(relay != nullptr);
        const uint8_t jmp_rip[6]{0xff, 0x25, 0, 0, 0, 0};
        const uint64_t hook = reinterpret_cast<uintptr_t>(&deliver_next), to_relay = reinterpret_cast<uintptr_t>(relay);
        uint8_t saved[16];
        std::memcpy(saved, deliver_code, 16);
        if (relay) {
            std::memcpy(relay, jmp_rip, 6);
            std::memcpy(relay + 6, &hook, 8);
            std::memcpy(deliver_code, jmp_rip, 6);
            std::memcpy(deliver_code + 6, &to_relay, 8);
        }
        deliver_state = KILN_CODE_FOREIGN;
        const auto why = run_ready(c);
        deliver_state = KILN_CODE_MANAGED;
        std::memcpy(deliver_code, saved, 16);
        if (relay)
            VirtualFree(relay, 0, MEM_RELEASE);
        CHECK(!kitems::enabled && detours == 0 && has(why, prefix) && has(why, "its jump leads into ") &&
              has(why, ".exe+0x") && !has(why, "in no module") && has(why, "Remove that mod or loader") &&
              has(why, "restart") && has(why, "shroudlink's own core") && has(why, "live ff 25 00 00 00 00") &&
              has(why, "file 40 56 41 55 41 56 48 83"));
        std::printf("  foreign relay: %s\n", why.c_str());
    }
    {
        fake_types::Catalog c;
        guard_catalog(c);
        deliver_state = KILN_CODE_UNKNOWN;
        const auto why = run_ready(c);
        deliver_state = KILN_CODE_MANAGED;
        CHECK(!kitems::enabled && detours == 0 && has(why, "gameplay/DeliverEvents") && has(why, "could not be read"));
    }
    {
        fake_types::Catalog c;
        guard_catalog(c, "itemAction");
        const auto why = run_ready(c);
        CHECK(!kitems::enabled && detours == 0 && has(why, prefix) &&
              has(why, "PlayerInput.fromClient has no itemAction"));
    }
    {
        fake_types::Catalog c;
        guard_catalog(c, nullptr, "weaponGemAction");
        const auto why = run_ready(c);
        CHECK(!kitems::enabled && detours == 0 && has(why, "weaponGemAction") &&
              has(why, "no consumedWeaponGemAction in ServerConsumedPlayerInput"));
    }
    {
        fake_types::Catalog c;
        guard_catalog(c);
        detour_rc = KILN_ERR_UNAVAILABLE;
        const auto why = run_ready(c);
        detour_rc = KILN_OK;
        CHECK(!kitems::enabled && detours == 1 && has(why, "gameplay/DeliverEvents") &&
              has(why, "raw_detour returned 3 (unavailable"));
        std::printf("  install: %s\n", why.c_str());
    }
    fake_api = saved_api;
    fake_caps = saved_caps;
    kitems::world_api = nullptr;
    kitems::types_api = nullptr;
    kitems::players_api = nullptr;
    kitems::saves_api = nullptr;
    kitems::client_actions.clear();
    kitems::enabled = true;
    fake_types::current = nullptr;
}
} // namespace
int main() {
    using namespace kitems;
    using namespace kitems::storage;
    std::setvbuf(stdout, nullptr, _IONBF, 0); // a faulting check must not lose the lines before it
    E.get_component = &get;
    E.read_other = &other;
    E.write_other = &write_other;
    E.job_tracker = &tracker;
    E.job_write_flag = &flag;
    E.setup_initialized = 83;
    S.create = &create;
    S.destroy = &destroy;
    S.convert = &convert;
    S.write = &writer;
    S.read = &reader;
    S.rebuild = &rebuild;
    S.tick_write = &tick_write;
    E.item_find = &lookup_item;
    E.item_flags = 0x20;
    E.item_max_stack = 0x24;
    uint16_t max_stack = 100;
    std::memcpy(item_bytes + E.item_max_stack, &max_stack, 2);
    item_bytes[0xcc] = 8;
    item_bytes[0x104] = 6;
    item_bytes[0x54c] = 1;
    Access a{};
    a.storage = reinterpret_cast<void*>(1);
    a.ctx = reinterpret_cast<void*>(2);
    a.job = reinterpret_cast<void*>(3);
    for (int i = 0; i < A_COUNT; ++i)
        a.acc[i] = i == A_SETUP       ? kHashInventorySetup
                   : i == A_INVENTORY ? kHashInventory
                   : i == A_OWNER     ? 0xd7361788u
                                      : kHashSlotSelection;
    S.structural_gate = &gate;
    std::puts("storage native: engine adapter cases");
    Holder h;
    gate_open = false;
    CHECK(!create_holder(a, 1, 9, &h) && creates == 0);
    gate_open = true;
    std::puts("  adapter: initial create");
    CHECK(create_holder(a, 1, 9, &h));
    CHECK(h.entity == 2000 && h.pages.size() == 2);
    CHECK(components[{h.entity, kHashInventorySetup}][81] == 16 &&
          components[{h.entity, kHashInventorySetup}][83] == 1 &&
          components[{h.entity, kHashInventorySetup}][82] == 16);
    CHECK(components[{h.entity, kHashInventorySetup}][64] == 4);
    CHECK(components[{h.entity, kHashInventorySetup}].size() == 84);
    CHECK(components[{h.pages[0], kHashInventory}].size() == 96);
    components[{1, kHashInventorySetup}].assign(84, 0);
    components[{1, kHashInventorySetup}][80] = 1;
    components[{1, kHashInventorySetup}][81] = 40;
    components[{1, kHashInventorySetup}][83] = 1;
    for (uint32_t i = 0; i < 5; ++i) {
        uint32_t page = 10001 + i;
        std::memcpy(components[{1, kHashInventorySetup}].data() + i * 4, &page, 4);
        components[{1, kHashInventorySetup}][64 + i] = 4;
        components[{page, kHashInventory}].assign(96, 0);
        components[{page, 0xd7361788u}].resize(4);
        uint32_t player = 1;
        std::memcpy(components[{page, 0xd7361788u}].data(), &player, 4);
    }
    const auto player_pages_before = components;
    auto deletes_before = destroys;
    Holder reserved;
    reserved.entity = 1;
    reserved.pages = {10001};
    CHECK(!destroy_holder(a, &reserved) && components == player_pages_before && destroys == deletes_before);
    components = player_pages_before;
    destroys = deletes_before;
    Holder wrong_owner = h;
    wrong_owner.pages = {10001};
    CHECK(!destroy_holder(a, &wrong_owner) && components == player_pages_before && destroys == deletes_before);
    components = player_pages_before;
    destroys = deletes_before;
    uint32_t stack[]{1, 5, 0};
    std::memcpy(components[{h.pages[0], kHashInventory}].data(), stack, 12);
    EntityRef ref{1, 0, 0, 0, a.storage, nullptr};
    Entry entry;
    CHECK(freeze_holder(h, nullptr, nullptr, &ref, &entry));
    CHECK(entry.records.size() == 1 && entry.records[0].count == 5 && entry.records[0].length == 8 && writes == 1 &&
          converts == 1);
    components[{1, kHashSlotSelection}].resize(216);
    CHECK(reference_write(a, 1, 14, KILN_SLOT_ID(h.pages[0], 0)));
    uint64_t cell = 0;
    std::memcpy(&cell, components[{1, kHashSlotSelection}].data() + 14 * 8, 8);
    CHECK(cell == KILN_SLOT_ID(h.pages[0], 0));
    CHECK(!reference_write(a, 1, 13, cell));
    item_bytes[E.item_flags] = 2;
    relocate_page = h.pages[1];
    Record rebuilt{};
    rebuilt.item = 123;
    rebuilt.count = 1;
    rebuilt.slot = 8;
    auto rebuilt_wire = wire_record(rebuilt);
    KilnStorageRecord saved{};
    saved.slot = 8;
    saved.item = 123;
    saved.count = 1;
    saved.length = static_cast<uint8_t>(rebuilt_wire.size());
    std::memcpy(saved.data, rebuilt_wire.data(), rebuilt_wire.size());
    uint32_t why_rebuild = 0;
    CHECK(insert_record(a, h, saved, &why_rebuild));
    uint32_t stored = 0;
    std::memcpy(&stored, components[{h.pages[1], kHashInventory}].data(), 4);
    CHECK(stored == 123);
    uint32_t item_owner = 0;
    std::memcpy(&item_owner, components[{5000, 0xd7361788u}].data(), 4);
    CHECK(item_owner == h.entity);
    item_bytes[E.item_flags] = 0;
    Record padded = rebuilt;
    padded.slot = 9;
    auto padded_wire = wire_record(padded);
    auto padded_saved = saved;
    padded_saved.slot = 9;
    padded_saved.length = static_cast<uint8_t>(padded_wire.size());
    std::memcpy(padded_saved.data, padded_wire.data(), padded_wire.size());
    const auto before_padding = components;
    CHECK(!insert_record(a, h, padded_saved, &why_rebuild) && why_rebuild == KIS_QUARANTINE_SLOT &&
          components == before_padding);
    const auto main_setup = components[{1, kHashInventorySetup}];
    CHECK(clear_holder_references(1, 1, h));
    CHECK(kitems::storage::read<uint64_t>(components[{1, kHashSlotSelection}].data(), 14 * 8) == 0 &&
          components[{1, kHashInventorySetup}] == main_setup);
    std::puts("  adapter: full cleanup");
    CHECK(destroy_holder(a, &h) && destroys == 4);
    creates = 0;
    fail_create = true;
    std::puts("  adapter: failed page cleanup");
    CHECK(!create_holder(a, 1, 9, &h));
    CHECK(!h.entity && destroys == 5);
    fail_create = false;
    raise_create = true;
    std::puts("  adapter: create fault");
    CHECK(!create_holder(a, 1, 9, &h));
    raise_create = false;
    Record rec{};
    rec.item = 123;
    rec.slot = 3;
    rec.count = 5;
    for (unsigned i = 0; i < 6; ++i) {
        rec.present[i] = true;
        rec.options[i].assign(option_sizes[i], 0);
    }
    uint8_t memory[56]{};
    CHECK(memory_from_record(rec, memory));
    Record same{};
    CHECK(record_from_memory(memory, &same) && wire_record(rec) == wire_record(same));
    CHECK(same.present[0] && same.present[5]);
    Layout layout;
    std::string why;
    CHECK(derive_layout({0x1a68, 0x1aa4, 0x1dec, 0x1e28, 0x1e38, 0x1e44, 0x1e50, 56}, &layout, &why));
    S.layout = layout;
    std::vector<uint8_t> character(0x5500);
    uint64_t hash = 0;
    Inventory empty{};
    std::vector<uint8_t> encoded;
    encode(empty, &encoded);
    CHECK(character_witness(character.data(), &hash) && hash == witness(encoded));
    uint32_t witness_flags = 0;
    character[layout.main_pages] = 5;
    CHECK(character_witness(character.data(), &hash, 5, &witness_flags) && witness_flags == KIS_WITNESS_READ);
    character[layout.main_pages] = 6;
    CHECK(character_witness(character.data(), &hash, 5, &witness_flags) &&
          witness_flags == (KIS_WITNESS_READ | KIS_WITNESS_EXTRA_MAIN_PAGES));
    character[layout.main_pages] = 8;
    CHECK(character_witness(character.data(), &hash, 5, &witness_flags) &&
          (witness_flags & KIS_WITNESS_EXTRA_MAIN_PAGES));
    CHECK(character_witness(character.data(), &hash, 8, &witness_flags) && witness_flags == KIS_WITNESS_READ);
    character[layout.main_pages] = 5;
    character[layout.extra_pages] = 16;
    CHECK(character_witness(character.data(), &hash, 5, &witness_flags) && witness_flags == KIS_WITNESS_READ);
    character[layout.extra_pages] = 0;
    character[layout.main_pages] = 17;
    CHECK(!character_witness(character.data(), &hash));
    std::puts("  adapter: read fault");
    CHECK(!character_witness(reinterpret_cast<void*>(1), &hash));
    std::puts("storage native: service_tests");
    service_tests();
    std::puts("storage native: review_tests");
    review_tests();
    std::puts("storage native: holder_retry_tests");
    holder_retry_tests();
    std::puts("storage native: leave_timeout_tests");
    leave_timeout_tests();
    std::puts("storage native: guard_ready_tests");
    guard_ready_tests();
    std::puts("storage native: init_caps_test");
    init_caps_test(); // last: storage_init replaces the structural gate the tests above use
    std::printf("storage native: %d checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
}
