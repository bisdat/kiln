// SPDX-License-Identifier: MIT
// kiln.content registry stage: the regmap (registry/regmap.h), on the host. How each Kiln field maps onto the game per game
// build: the common rows (paths, conversion, read time, back-end), the per-build record (verified, row states, alias
// deltas), the routing rule against the schema's "applies", the mapping state an edit gets, and the entries' Kiln ids
// (stable names, clashes, unnamed entries, drop tables named by their templates, pinned renames and removals).
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include "../registry/regmap.h"
#include "../registry/schema.g.h"

using namespace kc::registry;
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
        auto m3 = common(R"({"regmap": 1, "registry": "items", "entries": {"kind": "resource", "resourceType": "keen::ItemInfo"},
            "rows": {"rolled": {"paths": ["generateRarity"], "conv": "bool", "read": "unknown", "backend": "live", "state": "unproven", "reason": "x"}}})", &errs);
        CHECK(has(map::check_routing(m3), "cautious applies"));   // items.rolled applies now: an untraced read time cannot promise that
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
        rm.side = "server", rm.exe_sha256 = "f0d824be7fcd48ebfe2458ff5c28da26060c2074e3ec1fcac587e5a1d508aeb2";
        CHECK(rm.mapping("items", "maxStack", &why) == Mapping::Unproven &&
              why == "Unproven: items.maxStack is not proven for this game build (server exe f0d824be) (no verified regmap record for it yet; "
                     "a Kiln update is needed); this edit was not applied");
        rm.build.verified = true, rm.build.game_build = "1287369", rm.has_build = true;
        rm.build.rows = {{"items.maxStack", map::BuildRow::Ok}, {"items.rarity", map::BuildRow::Changed}, {"items.comfort", map::BuildRow::Ok}};
        CHECK(rm.mapping("items", "maxStack", &why) == Mapping::Mapped);
        CHECK(rm.mapping("items", "rarity", &why) == Mapping::Unmapped &&
              why == "Unmapped: items.rarity is not mapped for game build 1287369 yet (a Kiln update is needed); this edit was not applied");
        CHECK(rm.mapping("items", "comfort", &why) == Mapping::Unproven &&
              why == "Unproven: items.comfort is not proven for game build 1287369 (the read time is not traced); this edit was not applied");
        CHECK(rm.mapping("items", "manaCost", &why) == Mapping::Unmapped &&
              why == "Unmapped: items.manaCost is not mapped for game build 1287369 yet (a Kiln update is needed: not mapped yet); this edit was not applied");
        CHECK(rm.mapping("items", "power", &why) == Mapping::Unmapped);        // no row at all
        CHECK(rm.mapping("items", "levels.min", &why) == Mapping::Unmapped);
        CHECK(rm.mapping("nowhere", "x", &why) == Mapping::Unmapped);
        rm.build.verified = false;
        CHECK(rm.mapping("items", "maxStack", &why) == Mapping::Unproven);
    }
    // ---- ids: the cases native and tools/registrygen/regmap_ids.py must both give (registry_ids_cases.json)
    if (argc > 2) {
        std::ifstream f(argv[2]);
        std::stringstream ss;
        ss << f.rdbuf();
        const auto doc = map::json::parse(ss.str(), nullptr, false);
        CHECK(!doc.is_discarded() && doc["cases"].size() >= 8);
        for (const auto& c : doc["cases"]) {
            map::EntrySource src;
            const auto& s = c["src"];
            src.unnamed = s.value("unnamed", ""), src.name_from = s.value("nameFrom", ""), src.key = s.value("key", "");
            map::Aliases al;
            const std::string reg = c["registry"];
            const map::json pinned = c["aliases"].value("pinned", map::json::object()), removed = c["aliases"].value("removed", map::json::object());
            for (const auto& [g, id] : pinned.items()) al.pinned[reg + "|" + g] = id;
            for (const auto& [id, b] : removed.items()) al.removed[reg + "|" + id] = b;
            std::vector<map::GameEntry> es;
            for (const auto& e : c["entries"]) {
                map::GameEntry x{e["guid"], e["name"], e["id"].get<uint32_t>(), {}};
                const map::json named = e.value("namedBy", map::json::array());
                for (const auto& t : named) x.named_by.push_back(t);
                es.push_back(x);
            }
            std::map<uint32_t, std::string> by;
            const map::json by_item = c.value("byItemId", map::json::object());
            for (const auto& [k, v] : by_item.items()) by[static_cast<uint32_t>(std::stoul(k))] = v;
            std::vector<std::string> notes;
            const auto ids = map::assign_ids(reg, src, al, es, c.contains("byItemId") ? &by : nullptr, &notes);
            const auto want = c["expect"].get<std::vector<std::string>>();
            if (ids != want) {
                std::printf("  case \"%s\":", c["name"].get<std::string>().c_str());
                for (const auto& i : ids) std::printf(" %s", i.c_str());
                std::printf("\n");
            }
            CHECK(ids == want);
            CHECK(notes.size() == c.value("notes", 0u));
            for (const auto& i : ids) CHECK(map::is_kiln_id(i));
        }
        std::printf("ids: %zu cases\n", doc["cases"].size());
    }
    // ---- the id grammar
    for (const char* ok : {"game:x", "game:_hidden", "mymod:crate.v2", "game:twin~1a2b3c4d", "game:x~0123456789abcdef0123456789abcdef"})
        CHECK(map::is_kiln_id(ok));
    for (const char* bad : {"", "game:", ":x", "Game:x", "game:X", "game:a b", "game:x~1a2b", "game:x~zzzzzzzz", "_mod:x", "a:b:c", "game:x~1a2b3c4d~1a2b3c4d"})
        CHECK(!map::is_kiln_id(bad));
    // ---- a row that breaks the routing rule loads as unproven (load_dir)
    if (argc > 3) {
        namespace fs = std::filesystem;
        const fs::path d = fs::path(argv[3]) / "regmap-broken";
        fs::remove_all(d);
        fs::create_directories(d / "common");
        fs::create_directories(d / "server");
        std::ofstream(d / "common" / "items.json") << R"({"regmap": 1, "registry": "items", "entries": {"kind": "resource", "resourceType": "keen::ItemInfo"},
            "rows": {"maxStack": {"paths": ["maxStackSize"], "conv": "int", "read": "restart", "backend": "live"},
                     "rolled": {"paths": ["generateRarity"], "conv": "bool", "read": "now", "backend": "live"}}})";
        const std::string sha(64, 'a');
        std::ofstream(d / "server" / (sha + ".json")) << R"({"regmap": 1, "side": "server", "exeSha256": ")" + sha +
                                                             R"(", "gameBuild": "1", "verified": true, "rows": {"items.maxStack": "ok", "items.rolled": "ok"}})";
        map::RegMap rm;
        std::vector<std::string> errs;
        CHECK(map::load_dir(d.string(), "server", sha, &rm, &errs));
        CHECK(has(errs, "items.maxStack"));
        std::string why;
        CHECK(rm.mapping("items", "maxStack", &why) == Mapping::Unproven && why.find("routing rule") != std::string::npos);
        CHECK(rm.mapping("items", "rolled", &why) == Mapping::Mapped);
        fs::remove_all(d);
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
            // S6: every R1 field is mapped on this build, or explicitly unproven with its reason; none unmapped
            int mapped = 0, unproven = 0, unmapped = 0;
            for (const gen::Field& f : gen::kFields) {
                std::string why;
                const Mapping m = one.mapping(f.registry, f.path, &why);
                (m == Mapping::Mapped ? mapped : m == Mapping::Unproven ? unproven : unmapped)++;
                if (m == Mapping::Unmapped) std::printf("  unmapped on %s: %s\n", side, why.c_str());
            }
            std::printf("regmap %s: %d mapped, %d unproven, %d unmapped\n", side, mapped, unproven, unmapped);
            CHECK(unmapped == 0 && mapped + unproven == static_cast<int>(std::size(gen::kFields)));
            rm = std::move(one);
        }
        std::printf("regmap: %zu registries loaded from %s\n", rm.registries.size(), argv[1]);
    }
    std::printf("registry-regmap-test: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
