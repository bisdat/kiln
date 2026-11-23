// SPDX-License-Identifier: MIT
// Windows SEH and the actual engine adapter with a fake ECS; never starts or loads the game.
#include "../storage.cpp"
#include "../storage_engine.cpp"
#include <cstdio>
#include <map>
#include <thread>
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
    v.item = 1;
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
const void* KILN_CALL service(KilnStr name, uint32_t) {
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
    CHECK(kitems::park_holder(access, parked_player, "mod:bag", KIS_PARK_UNREGISTERED, service_lock));
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
} // namespace
int main() {
    using namespace kitems;
    using namespace kitems::storage;
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
    std::printf("storage native: %d checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
}
