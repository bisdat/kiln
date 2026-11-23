// SPDX-License-Identifier: MIT
// Windows SEH and the actual engine adapter with a fake ECS; never starts or loads the game.
#include "../storage.cpp"
#include "../storage_engine.cpp"
#include <cstdio>
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
    auto e = next_id++;
    for (unsigned i = 0; i < d->count; ++i) {
        const auto& c = d->components[i];
        components[{e, c.hash}].assign(static_cast<const uint8_t*>(c.bytes),
                                       static_cast<const uint8_t*>(c.bytes) + c.size);
    }
    return e;
}
uint8_t __fastcall destroy(void*, uint32_t e, uint8_t mode, uint8_t skip) {
    CHECK(mode == 1 && skip == 0);
    ++destroys;
    for (auto i = components.begin(); i != components.end();)
        i = i->first.first == e ? components.erase(i) : std::next(i);
    return 1;
}
bool gate_open = true;
bool gate() { return gate_open; }
PtrSize* __fastcall get(PtrSize* out, uintptr_t, uint32_t hash, uint32_t entity) {
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
void items_log(int, const char*, ...) {}
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
    Holder h;
    gate_open = false;
    CHECK(!create_holder(a, 1, 9, &h) && creates == 0);
    gate_open = true;
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
    CHECK(destroy_holder(a, &h) && destroys == 4);
    creates = 0;
    fail_create = true;
    CHECK(!create_holder(a, 1, 9, &h));
    CHECK(!h.entity && destroys == 5);
    fail_create = false;
    raise_create = true;
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
    CHECK(!character_witness(reinterpret_cast<void*>(1), &hash));
    service_tests();
    review_tests();
    init_caps_test(); // last: storage_init replaces the structural gate the tests above use
    std::printf("storage native: %d checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
}
