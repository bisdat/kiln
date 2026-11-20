// SPDX-License-Identifier: MIT
// kiln.content registry stage: the regmap (registry/regmap.h), on the host. How each Kiln field maps onto the game per game
// build: the common rows (paths, conversion, read time, back-end), the per-build record (verified, row states, alias
// deltas), the routing rule against the schema's "applies", the mapping state an edit gets, and the entries' Kiln ids
// (stable names, clashes, unnamed entries, drop tables named by their templates, pinned renames and removals).
#include <cstdio>
#include <filesystem>
#include <string>

#include "../registry/regmap.h"

using namespace kc::reg;
using map::Mapping;

static int g_fail = 0, g_pass = 0;
#define CHECK(c)                                                                  \
    do {                                                                          \
        if (c) ++g_pass;                                                          \
        else { ++g_fail; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } \
    } while (0)

static bool has(const std::vector<std::string>& v, const std::string& part) {
    for (const std::string& s : v)
        if (s.find(part) != std::string::npos) return true;
    return false;
}

static map::RegistryMap common(const char* text, std::vector<std::string>* errs) {
    map::RegistryMap m;
    map::load_common(map::json::parse(text), &m, errs);
    return m;
}

int main(int argc, char** argv) {
    // ---- a common file: entries and rows
    {
        std::vector<std::string> errs;
        auto m = common(R"({"regmap": 1, "registry": "items",
            "entries": {"kind": "resource", "resourceType": "keen::ItemInfo", "name": "debugName", "guid": "objectId", "id": "itemId", "unnamed": "item_%08x"},
            "rows": {
              "maxStack": {"paths": ["maxStackSize"], "conv": "int", "read": "worldLoad", "backend": "live", "clientNeeds": true},
              "rarity": {"paths": ["rarity"], "conv": "enum", "values": {"common": "Common", "uncommon": "Uncommon", "rare": "Rare", "epic": "Epic", "legendary": "Legendary"}, "read": "now", "backend": "live"},
              "comfort": {"paths": ["comfortSetup.comfortAmount"], "conv": "int", "read": "unknown", "backend": "live", "state": "unproven", "reason": "the read time is not traced"}
            }})", &errs);
        CHECK(errs.empty());
        CHECK(m.registry == "items" && m.entries.kind == map::EntryKind::Resource && m.entries.resource_type == "keen::ItemInfo");
        CHECK(m.rows.size() == 3 && m.rows.at("maxStack").paths == std::vector<std::string>({"maxStackSize"}));
        CHECK(m.rows.at("maxStack").read == map::Read::WorldLoad && m.rows.at("maxStack").backend == map::Backend::Live && m.rows.at("maxStack").client_needs);
        CHECK(m.rows.at("rarity").values.at("epic") == "Epic");
        CHECK(m.rows.at("comfort").state == map::RowState::Unproven);
        // the routing rule against the generated schema: none of these breaks it
        CHECK(map::check_routing(m).empty());
    }
    // ---- malformed rows are refused with the field named
    {
        std::vector<std::string> errs;
        common(R"({"regmap": 1, "registry": "items", "entries": {"kind": "resource", "resourceType": "keen::ItemInfo"},
            "rows": {"maxStack": {"paths": [], "conv": "int", "read": "now", "backend": "live"},
                     "rarity": {"paths": ["rarity"], "conv": "teleport", "read": "now", "backend": "live"},
                     "rolled": {"paths": ["generateRarity"], "conv": "bool", "read": "sometimes", "backend": "live"},
                     "nonsense": {"paths": ["x"], "conv": "int", "read": "now", "backend": "live"},
                     "socket": {"paths": ["gemSlotOverride.isSet"], "conv": "bool", "read": "now", "backend": "live", "offset": 1944}}})", &errs);
        CHECK(has(errs, "items.maxStack") && has(errs, "items.rarity") && has(errs, "items.rolled"));
        CHECK(has(errs, "items.nonsense") && has(errs, "items.socket"));
    }
    // ---- routing: what the schema promises (applies) and what the game does (read, back-end) must agree
    {
        std::vector<std::string> errs;
        auto m = common(R"({"regmap": 1, "registry": "items", "entries": {"kind": "resource", "resourceType": "keen::ItemInfo"},
            "rows": {"maxStack": {"paths": ["maxStackSize"], "conv": "int", "read": "restart", "backend": "live"},
                     "rarity": {"paths": ["rarity"], "conv": "enum", "values": {"common": "Common"}, "read": "now", "backend": "boot"},
                     "fuel.burnTime": {"paths": ["fuelSetup.burnTime"], "conv": "time", "read": "unknown", "backend": "live"},
                     "rolled": {"paths": ["generateRarity"], "conv": "bool", "read": "now", "backend": "live"}}})", &errs);
        CHECK(errs.empty());
        const auto r = map::check_routing(m);
        CHECK(has(r, "items.maxStack") && has(r, "restart"));   // promised worldLoad, the game reads it at start only
        CHECK(has(r, "items.rarity") && has(r, "boot"));         // a boot write cannot act "now"
        CHECK(has(r, "items.fuel.burnTime") && has(r, "unproven"));
        CHECK(!has(r, "items.rolled"));
        auto m2 = common(R"({"regmap": 1, "registry": "balance", "entries": {"kind": "single", "resourceType": "keen::BalancingTable", "resourceGuid": "82706b40-61b1-4b8f-8b23-dcec6971bda1"},
            "rows": {"levelCap": {"paths": ["playerLevelCap"], "conv": "int", "read": "restart", "backend": "live"}}})", &errs);
        CHECK(has(map::check_routing(m2), "balance.levelCap"));  // restart fields go through the boot back-end
    }
    // ---- a per-build record
    {
        std::vector<std::string> errs;
        map::BuildRecord b;
        map::load_record(map::json::parse(R"({"regmap": 1, "side": "server", "exeSha256": "f0d824be7fcd48ebfe2458ff5c28da26060c2074e3ec1fcac587e5a1d508aeb2",
            "gameBuild": "1287369", "steamBuild": "26430253", "verified": true, "proof": "test",
            "layouts": {"keen::ItemInfo": "0x07009d37"},
            "rows": {"items.maxStack": "ok", "items.rarity": "changed"},
            "aliases": {"renames": [{"registry": "items", "guid": "00000000-0000-0000-0000-0000000000a1", "id": "game:old_name", "since": "1287369"}],
                        "removals": [{"registry": "items", "id": "game:gone", "removedIn": "1287369"}],
                        "suffixes": [{"registry": "items", "guid": "00000000-0000-0000-0000-0000000000b2", "id": "game:twin~00000000"}]}})"), &b, &errs);
        CHECK(errs.empty());
        CHECK(b.verified && b.side == "server" && b.game_build == "1287369" && b.layouts.at("keen::ItemInfo") == 0x07009d37u);
        CHECK(b.aliases.pinned.size() == 2 && b.aliases.removed.at("items|game:gone") == "1287369");
        map::BuildRecord bad;
        map::load_record(map::json::parse(R"({"regmap": 1, "side": "moon", "exeSha256": "xyz", "verified": "yes", "rows": {"items.maxStack": "fine"}})"), &bad, &errs);
        CHECK(has(errs, "side") && has(errs, "exeSha256") && has(errs, "verified") && has(errs, "items.maxStack"));
    }
    // ---- the mapping state an edit gets on this build
    {
        std::vector<std::string> errs;
        map::RegMap rm;
        rm.registries["items"] = common(R"({"regmap": 1, "registry": "items", "entries": {"kind": "resource", "resourceType": "keen::ItemInfo"},
            "rows": {"maxStack": {"paths": ["maxStackSize"], "conv": "int", "read": "worldLoad", "backend": "live"},
                     "rarity": {"paths": ["rarity"], "conv": "enum", "values": {"common": "Common"}, "read": "now", "backend": "live"},
                     "comfort": {"paths": ["comfortSetup.comfortAmount"], "conv": "int", "read": "unknown", "backend": "live", "state": "unproven", "reason": "the read time is not traced"},
                     "manaCost": {"state": "unmapped", "reason": "not mapped yet"}}})", &errs);
        CHECK(errs.empty());
        std::string why;
        // no record for this exe: nothing is proven
        CHECK(rm.mapping("items", "maxStack", &why) == Mapping::Unproven && why.find("no verified regmap record") != std::string::npos);
        rm.build.verified = true, rm.build.game_build = "1287369", rm.has_build = true;
        rm.build.rows = {{"items.maxStack", map::BuildRow::Ok}, {"items.rarity", map::BuildRow::Changed}, {"items.comfort", map::BuildRow::Ok}};
        CHECK(rm.mapping("items", "maxStack", &why) == Mapping::Mapped);
        CHECK(rm.mapping("items", "rarity", &why) == Mapping::Unmapped && why == "Unmapped: items.rarity is not mapped for game build 1287369 yet (a Kiln update is needed)");
        CHECK(rm.mapping("items", "comfort", &why) == Mapping::Unproven && why.find("not traced") != std::string::npos);
        CHECK(rm.mapping("items", "manaCost", &why) == Mapping::Unmapped);
        CHECK(rm.mapping("items", "power", &why) == Mapping::Unmapped);        // no row at all
        CHECK(rm.mapping("items", "levels.min", &why) == Mapping::Unmapped);
        CHECK(rm.mapping("nowhere", "x", &why) == Mapping::Unmapped);
        rm.build.verified = false;
        CHECK(rm.mapping("items", "maxStack", &why) == Mapping::Unproven);
    }
    // ---- ids: stable names, clashes, unnamed entries, pins and removals
    {
        map::EntrySource src;
        src.kind = map::EntryKind::Resource, src.unnamed = "item_%08x";
        map::Aliases none;
        std::vector<map::GameEntry> e = {
            {"11111111-0000-0000-0000-000000000001", "Material_T1_Wood_Logs", 0x1},
            {"22222222-0000-0000-0000-000000000002", "Twin", 0x2},
            {"33333333-0000-0000-0000-000000000003", "Twin", 0x3},
            {"44444444-0000-0000-0000-000000000004", "TWIN", 0x4},
            {"55555555-0000-0000-0000-000000000005", "", 0xabcdef12},
        };
        auto ids = map::assign_ids("items", src, none, e, nullptr);
        CHECK(ids.size() == 5);
        CHECK(ids[0] == "game:material_t1_wood_logs");
        CHECK(ids[1] == "game:twin");                     // first in archive order keeps the name
        CHECK(ids[2] == "game:twin~33333333");
        CHECK(ids[3] == "game:twin~44444444");            // case-only clash too
        CHECK(ids[4] == "game:item_abcdef12");
        // a suffix that is itself taken grows
        std::vector<map::GameEntry> e2 = {{"aaaaaaaa-1111-0000-0000-000000000001", "X", 1}, {"aaaaaaaa-2222-0000-0000-000000000002", "X", 2},
                                          {"aaaaaaaa-3333-0000-0000-000000000003", "x", 3}};
        auto ids2 = map::assign_ids("items", src, none, e2, nullptr);
        CHECK(ids2[1] == "game:x~aaaaaaaa" && ids2[2] == "game:x~aaaaaaaa3333");
        // pinned by the record: a rename keeps the old id, a pinned suffix holds even when the order changes
        map::Aliases pins;
        pins.pinned["items|22222222-0000-0000-0000-000000000002"] = "game:old_twin";
        pins.pinned["items|44444444-0000-0000-0000-000000000004"] = "game:twin";
        auto ids3 = map::assign_ids("items", src, pins, e, nullptr);
        CHECK(ids3[1] == "game:old_twin" && ids3[3] == "game:twin" && ids3[2] == "game:twin~33333333");
        pins.removed["items|game:gone"] = "1287369";
        std::string build;
        CHECK(pins.is_removed("items", "game:gone", &build) && build == "1287369" && !pins.is_removed("items", "game:twin", &build));
    }
    // ---- drop tables: named after the templates that use them
    {
        map::EntrySource src;
        src.kind = map::EntryKind::Resource, src.name_from = "keen::ecs::DefaultInventory.defaultInventory", src.unnamed = "drops_%.8s";
        std::vector<map::GameEntry> e = {
            {"a20e8eae-7833-462e-9ea9-59d1e17a1b98", "", 0, {"LootContainer_Enemy_Scavenger_Melee01", "LootContainer_Enemy_Scavenger_Ranged01"}},
            {"b30e8eae-0000-0000-0000-000000000000", "", 0, {"Scatter_X"}},
            {"c40e8eae-0000-0000-0000-000000000000", "", 0, {}},
        };
        auto ids = map::assign_ids("drops", src, map::Aliases{}, e, nullptr);
        CHECK(ids[0] == "game:lootcontainer_enemy_scavenger_melee01");
        CHECK(ids[1] == "game:scatter_x");
        CHECK(ids[2] == "game:drops_c40e8eae");
    }
    // ---- loot: the id of the item with the same 32-bit id
    {
        map::EntrySource src;
        src.kind = map::EntryKind::Array, src.key = "items", src.unnamed = "item_%08x";
        std::map<uint32_t, std::string> items = {{7, "game:material_t1_wood_logs"}};
        std::vector<map::GameEntry> e = {{"", "", 7}, {"", "", 9}};
        auto ids = map::assign_ids("loot", src, map::Aliases{}, e, &items);
        CHECK(ids[0] == "game:material_t1_wood_logs" && ids[1] == "game:item_00000009");
    }
    // ---- the repository's regmap: every file loads, every row routes as the schema says, the records are whole
    if (argc > 1) {
        map::RegMap rm;
        std::vector<std::string> errs;
        for (const char* side : {"server", "client"}) {
            const std::string sha = std::string(side) == "server" ? "f0d824be7fcd48ebfe2458ff5c28da26060c2074e3ec1fcac587e5a1d508aeb2"
                                                                  : "5d9ef2f140b8ed49e30dbee655841b89d08baec20f799a1817e171a18a850c77";
            map::RegMap one;
            const bool ok = map::load_dir(argv[1], side, sha, &one, &errs);
            CHECK(ok);
            for (const std::string& e : errs) std::printf("  regmap: %s\n", e.c_str());
            CHECK(errs.empty());
            CHECK(one.has_build && one.build.verified && one.build.side == side);
            CHECK(one.registries.size() == 5);
            for (const auto& [name, reg] : one.registries) {
                const auto r = map::check_routing(reg);
                for (const std::string& e : r) std::printf("  routing: %s\n", e.c_str());
                CHECK(r.empty());
            }
            rm = std::move(one);
        }
        std::printf("regmap: %zu registries loaded from %s\n", rm.registries.size(), argv[1]);
    }
    std::printf("registry-regmap-test: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
