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
    std::printf("storage logic: %d checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
}
