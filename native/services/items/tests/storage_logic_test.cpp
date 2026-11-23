// SPDX-License-Identifier: MIT
#include "../storage_client_guard.h"
#include "../storage_logic.h"
#include "nlohmann/json.hpp"
#include <cstdio>
#include <fstream>
using namespace kitems::storage;
using nlohmann::json;
int checks = 0, failed = 0;
#define CHECK(x)                                                                                                       \
    do {                                                                                                               \
        ++checks;                                                                                                      \
        if (!(x)) {                                                                                                    \
            ++failed;                                                                                                  \
            std::printf("FAIL %d: %s\n", __LINE__, #x);                                                                \
        }                                                                                                              \
    } while (0)
Record from_json(const json& e) {
    Record r{};
    r.slot = e.at("slot");
    r.item = e.at("itemId");
    r.count = e.at("count");
    const char* names[][2] = {{"level", nullptr},      {"durability", "maxDurability"}, {"liquid", "maxLiquid"},
                              {"appearance", nullptr}, {"maxUpgrades", "upgrades"},     {"socket", nullptr}};
    const unsigned widths[][2] = {{1, 0}, {4, 4}, {4, 4}, {4, 0}, {1, 1}, {5, 0}};
    for (unsigned i = 0; i < 6; ++i)
        if (e.contains(names[i][0])) {
            r.present[i] = true;
            for (unsigned j = 0; j < 2 && names[i][j]; ++j) {
                if (e.at(names[i][j]).is_string()) {
                    auto s = e.at(names[i][j]).get<std::string>();
                    for (size_t k = 0; k < s.size(); k += 2)
                        r.options[i].push_back(static_cast<uint8_t>(std::stoul(s.substr(k, 2), nullptr, 16)));
                } else
                    append_le(r.options[i], e.at(names[i][j]).get<uint64_t>(), widths[i][j]);
            }
        }
    return r;
}
KilnStorageRecord stored(uint16_t slot, uint32_t item, uint16_t count) {
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
KilnStorageEntry stored_entry(const char* key, uint32_t kind, uint32_t slots, uint32_t reason, uint32_t first,
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
// A key keeps one PARKED entry of at most 64 records in distinct cells; held records first, the highest reason wins;
// what does not fit goes to the key's quarantine unchanged ("parked storage full", reason 6). Nothing is dropped.
void parked_bound_tests() {
    Model m;
    auto join = m.join(0, 55, {}, 1, 1);
    std::vector<KilnStorageRecord> records;
    for (uint16_t i = 0; i < 64; ++i)
        records.push_back(stored(i, 100 + i, 1));
    records.push_back(stored(0, 7, 5));
    // document order on purpose: the unregistered live copy first, the held bag second
    KilnStorageEntry rows[]{stored_entry("mod:bag", KIS_ENTRY_PARKED, 64, KIS_PARK_UNREGISTERED, 0, 64),
                            stored_entry("mod:bag", KIS_ENTRY_PARKED, 9, KIS_PARK_RETURNED, 64, 1)};
    KilnStorageDoc doc{};
    doc.size = sizeof doc;
    doc.source = KIS_SOURCE_SERVER;
    doc.account = 55;
    doc.entry_count = 2;
    doc.record_count = static_cast<uint32_t>(records.size());
    doc.entries = rows;
    doc.records = records.data();
    CHECK(m.restore(0, join, doc) == KILN_OK);
    const auto& entries = m.players.at(0).entries;
    unsigned parked_entries = 0, quarantined = 0, total = 0;
    for (const auto& e : entries) {
        total += static_cast<unsigned>(e.records.size());
        if (e.row.kind == KIS_ENTRY_PARKED) {
            ++parked_entries;
            CHECK(e.row.reason == KIS_PARK_RETURNED && e.records.size() == 64 && e.row.slots == 64);
            CHECK(e.records.size() == 64 && e.records[0].item == 7 && e.records[0].slot == 0);
            std::set<unsigned> cells;
            for (const auto& r : e.records) {
                cells.insert(r.slot);
                CHECK(r.slot < 64 && r.data[0] == r.slot);
                Record decoded{};
                CHECK(decode_record(r.data, r.length, &decoded) && decoded.slot == r.slot && decoded.item == r.item);
            }
            CHECK(cells.size() == e.records.size());
            for (size_t i = 1; i < e.records.size(); ++i)
                CHECK(e.records[i].item == 100 + i - 1 && e.records[i].slot == i);
        } else if (e.row.kind == KIS_ENTRY_QUARANTINE) {
            for (const auto& r : e.records) {
                ++quarantined;
                const auto original = stored(63, 163, 1);
                CHECK(r.reason == 6 && r.slot == 63 && r.item == 163 && r.length == original.length &&
                      !std::memcmp(r.data, original.data, original.length));
            }
        }
    }
    CHECK(parked_entries == 1 && quarantined == 1 && total == 65);
    // An already bounded entry stays exactly as it was.
    Model same;
    auto same_join = same.join(0, 55, {}, 1, 1);
    KilnStorageEntry one[]{stored_entry("mod:bag", KIS_ENTRY_PARKED, 9, KIS_PARK_FILTER, 0, 2)};
    KilnStorageRecord two[]{stored(5, 9, 1), stored(2, 8, 1)};
    doc.entry_count = 1;
    doc.record_count = 2;
    doc.entries = one;
    doc.records = two;
    CHECK(same.restore(0, same_join, doc) == KILN_OK);
    const auto& kept = same.players.at(0).entries;
    CHECK(kept.size() == 1 && kept[0].row.reason == KIS_PARK_FILTER && kept[0].row.slots == 9 &&
          kept[0].records.size() == 2 && kept[0].records[0].slot == 5 && kept[0].records[1].slot == 2 &&
          !std::memcmp(kept[0].records[0].data, two[0].data, two[0].length));
    // Two held bags with a colliding cell: the highest held reason, the second record moves to the lowest free cell.
    Model held;
    auto held_join = held.join(0, 55, {}, 1, 1);
    KilnStorageEntry bags[]{stored_entry("mod:bag", KIS_ENTRY_PARKED, 8, KIS_PARK_REQUESTED, 0, 1),
                            stored_entry("mod:bag", KIS_ENTRY_PARKED, 8, KIS_PARK_STALE, 1, 1)};
    KilnStorageRecord colliding[]{stored(0, 4, 1), stored(0, 5, 1)};
    doc.entry_count = 2;
    doc.entries = bags;
    doc.records = colliding;
    CHECK(held.restore(0, held_join, doc) == KILN_OK);
    const auto& merged = held.players.at(0).entries;
    CHECK(merged.size() == 1 && merged[0].row.reason == KIS_PARK_STALE && merged[0].records.size() == 2 &&
          merged[0].records[0].item == 4 && merged[0].records[0].slot == 0 && merged[0].records[1].item == 5 &&
          merged[0].records[1].slot == 1 && merged[0].records[1].data[0] == 1);
}
// A character that left keeps no holders: a later capture of that slot must not stage a pair without them.
void left_capture_tests() {
    Model m;
    uint32_t handle = 0;
    KilnStorageDef def{};
    def.size = sizeof def;
    def.slots = 8;
    def.key = {"mod:bag", 7};
    def.filter.size = sizeof def.filter;
    CHECK(m.register_def(1, def, &handle) == KILN_OK);
    auto join = m.join(0, 55, {}, 1, 1);
    KilnStorageDoc doc{};
    doc.size = sizeof doc;
    doc.source = KIS_SOURCE_NEW;
    doc.account = 55;
    CHECK(m.restore(0, join, doc) == KILN_OK);
    m.applied(0, 1);
    m.mark_built(0, "mod:bag", 4000, {4001});
    m.leave(0);
    auto tag = m.begin_batch(1, 1, 1);
    Entry parked;
    parked.row = stored_entry("old:bag", KIS_ENTRY_PARKED, 8, KIS_PARK_UNREGISTERED, 0, 0);
    m.capture(0, 77, {parked}, false, 0);
    m.complete();
    Frozen copy;
    CHECK(m.take(tag, &copy) == KILN_OK && copy.captures.empty() && copy.entries.empty());
}
int main(int argc, char** argv) {
    if (argc != 2)
        return 2;
    std::ifstream in(argv[1]);
    json j;
    in >> j;
    for (const auto& v : j.at("vectors")) {
        Inventory inv{};
        const auto& f = v.at("fields");
        for (unsigned i = 0; i < 16; ++i)
            if (!f["equipment"][i].is_null())
                inv.equipment[i] = from_json(f["equipment"][i]);
        for (unsigned i = 0; i < 4; ++i)
            if (!f["references"][i].is_null())
                inv.refs[i] = f["references"][i].get<uint16_t>();
        inv.main_pages = f.at("mainPages");
        inv.extra_pages = f.at("backpackPages");
        for (const auto& e : f.at("main"))
            inv.main.push_back(from_json(e));
        for (const auto& e : f.at("extra"))
            inv.extra.push_back(from_json(e));
        std::vector<uint8_t> bytes;
        CHECK(encode(inv, &bytes));
        CHECK(hex(bytes) == v.at("encodedHex").get<std::string>());
        CHECK(witness_text(bytes) == v.at("witness").get<std::string>());
        for (const auto& r : inv.main) {
            auto wire = wire_record(r);
            Record decoded{};
            CHECK(decode_record(wire.data(), wire.size(), &decoded));
            CHECK(wire_record(decoded) == wire);
        }
    }
    Layout l{};
    std::string why;
    CHECK(derive_layout({0x1a68, 0x1aa4, 0x1dec, 0x1e28, 0x1e38, 0x1e44, 0x1e50, 0x38}, &l, &why));
    CHECK(l.equip_stride == 0x3c);
    CHECK(!derive_layout({0x1a68, 0x1aa4, 0x1ded, 0x1e28, 0x1e38, 0x1e44, 0x1e50, 0x38}, &l, &why));
    CHECK(why.find("equip_last") != std::string::npos);
    CHECK(!derive_layout({0x1a68, 0x1a68, 0x1dec, 0x1e28, 0x1e38, 0x1e44, 0x1e50, 0x38}, &l, &why));
    CHECK(why.find("equip1") != std::string::npos);
    CHECK(key_ok("mod:quiver") && !key_ok("Mod:quiver") && !key_ok("mod:a:b") && !key_ok("mod:") &&
          !key_ok(std::string(65, 'a') + ":q"));
    Filter filter;
    uint32_t ids[]{3, 9};
    KilnStorageFilter f{sizeof f, 1u << 9, KIS_FILTER_NO_PIDE, 2, ids};
    CHECK(filter.assign(f));
    CHECK(filter.allows(3, 9, 14, false));
    CHECK(!filter.allows(4, 9, 14, false));
    CHECK(!filter.allows(3, 12, 14, false));
    CHECK(!filter.allows(3, 9, 14, true));
    f.categories = 0;
    f.flags = KIS_FILTER_AMMO_ARROW | KIS_FILTER_AMMO_SPELL;
    f.id_count = 0;
    CHECK(filter.assign(f));
    CHECK(filter.allows(4, 2, 6, false) && filter.allows(4, 2, 7, true) && !filter.allows(4, 2, 14, false) &&
          !filter.allows(4, 2, 12, false));
    f.id_count = 2;
    uint32_t bad[]{9, 3};
    f.ids = bad;
    CHECK(!filter.assign(f));
    f.flags = 8;
    CHECK(!filter.assign(f));
    for (unsigned mode = 0; mode < 3; ++mode) {
        CHECK(!death_takes(KIS_DEATH_KEEP, mode, 9, true, true));
        CHECK(death_takes(KIS_DEATH_GRAVE, mode, 9, true, true));
        CHECK(!death_takes(KIS_DEATH_GRAVE, mode, 9, false, true));
        CHECK(!death_takes(KIS_DEATH_GRAVE, mode, 9, true, false));
    }
    CHECK(death_takes(KIS_DEATH_FOLLOW_GAME, 1, 2, true, true));
    CHECK(death_takes(KIS_DEATH_FOLLOW_GAME, 0, 9, true, true));
    CHECK(death_takes(KIS_DEATH_FOLLOW_GAME, 0, 12, true, true));
    CHECK(!death_takes(KIS_DEATH_FOLLOW_GAME, 0, 2, true, true));
    CHECK(!death_takes(KIS_DEATH_FOLLOW_GAME, 2, 9, true, true));
    Model m;
    uint32_t handle = 0;
    KilnStorageDef def{};
    def.size = sizeof def;
    def.slots = 9;
    def.key = {"mod:bag", 7};
    def.filter.size = sizeof def.filter;
    CHECK(m.register_def(10, def, &handle) == KILN_OK && handle);
    CHECK(m.register_def(11, def, &handle) == KILN_ERR_INVALID_ARGUMENT);
    auto join = m.join(0, 55, {}, 123, 1);
    CHECK(join);
    CHECK(m.holder(10, 1, 1) == 0);
    CHECK(m.bind(0) == KILN_OK && m.bind(20) == KILN_ERR_UNAVAILABLE);
    KilnStorageDoc doc{};
    doc.size = sizeof doc;
    doc.source = KIS_SOURCE_NEW;
    doc.account = 55;
    CHECK(m.restore(0, join + 1, doc) == KILN_ERR_NOT_FOUND);
    CHECK(m.restore(0, join, doc) == KILN_OK);
    CHECK(m.restore(0, join, doc) == KILN_ERR_INVALID_ARGUMENT);
    CHECK(m.pending_build(0));
    CHECK(!m.build_ready(0));
    m.applied(0, 1);
    CHECK(m.build_ready(0));
    m.mark_built(0, "mod:bag", 2000, {2001, 2002});
    CHECK(m.holder(10, 1, 1) == 2000);
    CHECK(m.entity_owner(2002) == 1);
    CHECK(m.find_kind(2000, 0) == false && m.find_kind(2000, 4) == false && m.find_kind(2000, 16));
    CHECK(m.transfer_denied(2002, 88) && m.transfer_denied(88, 2000) && !m.transfer_denied(1, 2));
    CHECK(m.actor_allowed(2000, 1) && !m.actor_allowed(2000, 2));
    auto tag = m.begin_batch(7, 8, 1);
    m.capture(0, 999, {}, false, 0);
    m.complete();
    Frozen copy{};
    CHECK(m.take(tag, &copy) == KILN_OK && copy.header.saves_seq == 7 && copy.header.revision == 8 &&
          copy.header.flags == KIS_BATCH_COMPLETE);
    CHECK(copy.captures.size() == 1 && copy.captures[0].witness == 999);
    CHECK(m.release(tag) == KILN_OK && m.take(tag, &copy) == KILN_ERR_NOT_FOUND);
    m.join(1, 66, {}, 456, 2);
    auto second = m.begin_batch(8, 9, 2);
    m.capture(1, 456, {}, false, 0);
    m.complete();
    CHECK(m.take(second, &copy) == KILN_OK && copy.captures[0].flags == KIS_CAPTURE_UNRESTORED && copy.entries.empty());
    auto third = m.begin_batch(9, 10, 3);
    m.capture(0, 111, {}, true, 7);
    m.complete();
    CHECK(m.take(third, &copy) == KILN_OK && copy.captures[0].flags == KIS_CAPTURE_REFUSED && copy.entries.empty() &&
          (copy.header.flags & KIS_BATCH_REFUSED));
    CHECK(third != second);
    for (unsigned i = 0; i < 20; ++i) {
        m.begin_batch(100 + i, 100 + i, i);
        m.complete();
    }
    CHECK(m.batch_count() <= 16 && m.take(second, &copy) == KILN_ERR_NOT_FOUND);
    m.leave(0);
    CHECK(!m.build_ready(0) && !m.entity_owner(2000));
    CHECK(m.restore(0, join, doc) == KILN_ERR_NOT_FOUND);
    Model late;
    late.register_def(10, def, &handle);
    auto lj = late.join(0, 55, {}, 1, 1);
    late.applied(0, 1);
    CHECK(late.restore(0, lj, doc) == KILN_OK && late.build_ready(0));
    KilnStorageEntry entry{};
    entry.size = sizeof entry;
    entry.kind = KIS_ENTRY_LIVE;
    std::snprintf(entry.key, sizeof entry.key, "mod:bag");
    entry.slots = 9;
    entry.count = 1;
    entry.first = 2;
    Model invalid;
    auto ij = invalid.join(0, 55, {}, 1, 1);
    doc.entry_count = 1;
    doc.entries = &entry;
    CHECK(invalid.restore(0, ij, doc) == KILN_ERR_INVALID_ARGUMENT);
    Model quarantined;
    auto quarantine_join = quarantined.join(0, 55, {}, 1, 1, KIS_WITNESS_EXTRA_MAIN_PAGES);
    entry.first = 0;
    entry.count = 0;
    entry.kind = KIS_ENTRY_QUARANTINE;
    CHECK(quarantined.restore(0, quarantine_join, doc) == KILN_OK);
    quarantined.applied(0, 1);
    quarantined.mark_built(0, "mod:bag", 2000, {2001, 2002});
    CHECK((quarantined.players.at(0).witness.flags & KIS_WITNESS_EXTRA_MAIN_PAGES) != 0);
    quarantined.leave(0);
    CHECK((quarantined.players.at(0).witness.flags & KIS_WITNESS_EXTRA_MAIN_PAGES) != 0);
    quarantined.join(0, 55, {}, 2, 2);
    CHECK(quarantined.players.at(0).witness.flags == KIS_WITNESS_READ);
    Model batches;
    auto held = batches.begin_batch(1, 1, 1);
    batches.complete();
    Frozen held_copy;
    CHECK(batches.take(held, &held_copy) == KILN_OK);
    auto untaken = batches.begin_batch(2, 2, 2);
    batches.complete();
    CHECK(batches.drop_complete() == 1);
    CHECK(batches.take(held, &held_copy) == KILN_OK && batches.take(untaken, &held_copy) == KILN_ERR_NOT_FOUND);
    CHECK(batches.release(held) == KILN_OK);
    Model leaving;
    leaving.register_def(1, def, &handle);
    auto leave_join = leaving.join(0, 55, {}, 1, 1);
    KilnStorageDoc leave_doc{};
    leave_doc.size = sizeof leave_doc;
    leave_doc.source = KIS_SOURCE_NEW;
    leave_doc.account = 55;
    CHECK(leaving.restore(0, leave_join, leave_doc) == KILN_OK);
    leaving.applied(0, 1);
    leaving.mark_built(0, "mod:bag", 4000, {4001});
    auto leave_tag = leaving.begin_batch(3, 3, 3);
    leaving.capture(0, 2, {}, false, 0);
    CHECK(leaving.flag_leaving(leave_tag, 0, leave_join));
    CHECK(leaving.entity_owner(4001) == 1);
    leaving.complete();
    CHECK(leaving.take(leave_tag, &held_copy) == KILN_OK && (held_copy.captures[0].flags & KIS_CAPTURE_LEFT));
    leaving.leave(0);
    CHECK(!leaving.entity_owner(4001));
    Model locked;
    auto locked_join = locked.join(0, 55, {}, 1, 1);
    KilnStorageDoc lockdoc{};
    lockdoc.size = sizeof lockdoc;
    lockdoc.account = 55;
    lockdoc.source = KIS_SOURCE_SERVER;
    lockdoc.flags = KIS_DOC_LOCKED;
    CHECK(locked.restore(0, locked_join, lockdoc) == KILN_OK);
    locked.applied(0, 1);
    CHECK(!locked.build_ready(0));
    auto locked_tag = locked.begin_batch(3, 3, 3);
    locked.capture(0, 3, {}, false, 0);
    locked.complete();
    CHECK(locked.take(locked_tag, &held_copy) == KILN_OK && held_copy.captures[0].flags == KIS_CAPTURE_LOCKED &&
          held_copy.entries.empty());
    Model quota;
    for (unsigned i = 0; i < 4; ++i) {
        std::string key = "m:" + std::to_string(i);
        def.key = {key.data(), key.size()};
        def.slots = 64;
        CHECK(quota.register_def(1, def, &handle) == KILN_OK);
    }
    def.key = {"m:extra", 7};
    CHECK(quota.register_def(1, def, &handle) == KILN_ERR_UNAVAILABLE);
    Model rounded;
    for (unsigned i = 0; i < 16; ++i) {
        std::string key = "round" + std::to_string(i / 8) + ":s" + std::to_string(i);
        def.key = {key.data(), key.size()};
        def.slots = 9;
        CHECK(rounded.register_def(100 + i / 8, def, &handle) == KILN_OK);
    }
    def.key = {"round2:extra", 12};
    CHECK(rounded.register_def(102, def, &handle) == KILN_ERR_UNAVAILABLE);
    for (unsigned kind = 0; kind < 8; ++kind)
        for (unsigned field = 0; field < 4; ++field) {
            ClientActionLayout action{};
            action.size = 32;
            action.refs = {4, 8, 12, 20};
            uint8_t payload[64]{};
            payload[28] = static_cast<uint8_t>(kind);
            auto contains = [](uint32_t e) { return e == 2000 || e == 2001; };
            CHECK(!forbidden_action(payload, action, contains));
            uint32_t forged = 2001;
            std::memcpy(payload + action.refs[field], &forged, 4);
            CHECK(forbidden_action(payload, action, contains));
        }
    ClientActionLayout malformed{};
    malformed.size = 4;
    malformed.refs = {8};
    uint8_t safe[64]{};
    CHECK(forbidden_action(safe, malformed, [](uint32_t) { return false; }));
    parked_bound_tests();
    left_capture_tests();
    std::printf("storage logic: %d checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
}
