// SPDX-License-Identifier: MIT
// kiln.stationmenus, host test: the store (menulayout.cpp: the one parser and serializer of the normalized record, layers,
// generation, remote collection, the C table with its reports and sizing queries) and the materializer (own_publish over
// a made-up crafting-menu collection in one arena: the bytes of a layout's tree, every allocation and every header write
// failed in turn with both collection headers back as the game's, icons, the 16-per-workshop cap, the pending hold).
// menulayout_test.sh builds and runs it.
#include <algorithm>
#include "../menulayout.cpp"

#include <cstdio>
#include <cstring>
#include <set>
#include <string>
#include <vector>

#include "nlohmann/json.hpp"

namespace {
using kc::ml::TextMap;
using ojson = nlohmann::ordered_json;
using json = nlohmann::json;

int g_checks = 0, g_failed = 0;
void check(bool yes, const std::string& name) {
    ++g_checks;
    if (!yes) {
        ++g_failed;
        std::fprintf(stderr, "FAIL %s\n", name.c_str());
    }
}

// ---- helpers independent of the code under test ----
uint32_t fnv32(const uint8_t* p, size_t n) {
    uint32_t h = 0x811c9dc5u;
    for (size_t i = 0; i < n; ++i) h = (h ^ p[i]) * 0x01000193u;
    return h;
}
uint64_t fnv64(const std::string& s) {
    uint64_t h = 0xcbf29ce484222325ull;
    for (unsigned char c : s) h = (h ^ c) * 0x100000001b3ull;
    return h;
}
// "xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx" -> the game's (and .NET Guid.TryWriteBytes') byte order
void guid_bytes(const std::string& s, uint8_t out[16]) {
    std::string h;
    for (char c : s)
        if (c != '-') h += c;
    uint8_t raw[16];
    for (int i = 0; i < 16; ++i) raw[i] = static_cast<uint8_t>(std::stoul(h.substr(i * 2, 2), nullptr, 16));
    const int order[16] = {3, 2, 1, 0, 5, 4, 7, 6, 8, 9, 10, 11, 12, 13, 14, 15};
    for (int i = 0; i < 16; ++i) out[i] = raw[order[i]];
}
uint32_t workshop_of(const std::string& g) {
    uint8_t b[16];
    guid_bytes(g, b);
    return fnv32(b, 16);
}
std::string gid(uint32_t n, uint32_t kind = 0) {
    char b[40];
    std::snprintf(b, sizeof b, "%08x-0000-4000-8000-%012x", kind, n);
    return b;
}

const char* kWorkshopGuid = "66b9e41a-34d5-51c4-8cec-36c189ab26cc";  // chapter 16a's fixture: workshopId 2947643217
constexpr uint32_t kWorkshop = 2947643217u, kBase = 1302892403u;      // 0x4da88f73: the workbench in the chapter

// The design's example, keys in authored (unsorted) order, a second set with a heading.
ojson fixture() {
    ojson r;
    r["format"] = 1;
    r["origin"] = {{"mod", "menu-example"}, {"stationKey", "forge"}, {"workshopGuid", kWorkshopGuid}, {"workshopId", kWorkshop}};
    r["definition"] = {{"mod", "menu-example"}, {"key", "forge.menu"}, {"id", "724d7b7b-a362-566b-b0a7-5088f219266f"}};
    r["baseWorkshopId"] = kBase;
    ojson set0 = {{"key", "main"}, {"id", "90598091-ecc6-50ab-bcb9-0941439c2e80"}, {"heading", nullptr},
                  {"recipes", ojson::array({{{"key", "menu-example:reclaim"}, {"recipeId", 3112649734u}},
                                            {{"key", "menu-example:bolts"}, {"recipeId", 3696751007u}}})}};
    ojson set1 = {{"key", "fine"}, {"id", "90598091-ecc6-50ab-bcb9-0941439c2e81"}, {"heading", {{"*", "Fine work"}}},
                  {"recipes", ojson::array()}};
    r["tabs"] = ojson::array({{{"key", "metal"}, {"id", "27d00d31-a7ce-56d9-ab60-e2c50c02427d"},
                               {"name", {{"en", "Metal"}, {"de", "Metall"}}}, {"iconFromTab", 0},
                               {"sets", ojson::array({set0, set1})}}});
    return r;
}
const char* kCanonical =
    R"({"baseWorkshopId":1302892403,"definition":{"id":"724d7b7b-a362-566b-b0a7-5088f219266f","key":"forge.menu","mod":"menu-example"},)"
    R"("format":1,"origin":{"mod":"menu-example","stationKey":"forge","workshopGuid":"66b9e41a-34d5-51c4-8cec-36c189ab26cc","workshopId":2947643217},)"
    R"("tabs":[{"iconFromTab":0,"id":"27d00d31-a7ce-56d9-ab60-e2c50c02427d","key":"metal","name":{"de":"Metall","en":"Metal"},"sets":[)"
    R"({"heading":null,"id":"90598091-ecc6-50ab-bcb9-0941439c2e80","key":"main","recipes":[{"key":"menu-example:reclaim","recipeId":3112649734},)"
    R"({"key":"menu-example:bolts","recipeId":3696751007}]},{"heading":{"*":"Fine work"},"id":"90598091-ecc6-50ab-bcb9-0941439c2e81","key":"fine","recipes":[]}]}]})";

// A record of definition n for workshop GUID w (n distinct per layer), tabs as given.
ojson record_for(uint32_t n, const std::string& wguid, ojson tabs = ojson::array(), uint32_t base = kBase) {
    ojson r;
    r["format"] = 1;
    r["origin"] = {{"mod", "m"}, {"stationKey", "st"}, {"workshopGuid", wguid}, {"workshopId", workshop_of(wguid)}};
    r["definition"] = {{"mod", "m"}, {"key", "d" + std::to_string(n)}, {"id", gid(n, 0xd)}};
    r["baseWorkshopId"] = base;
    r["tabs"] = std::move(tabs);
    return r;
}

const KilnStationMenusApi* T() { return kc::ml::table(); }

struct Call {
    KilnResult rc = -1;
    KilnHandle handle = 0;
    uint64_t generation = 0;
    json report;
};
Call define(KilnOwner owner, const std::string& text) {
    Call c;
    char rep[4096];
    size_t len = 0;
    c.rc = T()->define(owner, KilnStr{text.data(), text.size()}, &c.handle, &c.generation, rep, sizeof rep, &len);
    try {
        c.report = json::parse(std::string(rep, std::min(len, sizeof rep - 1)));
    } catch (...) {
    }
    if (!c.report.is_object()) c.report = json::object();
    return c;
}
Call define(KilnOwner owner, const ojson& j) { return define(owner, j.dump()); }
bool refused(const Call& c) {
    return c.rc == KILN_ERR_INVALID_ARGUMENT && c.handle == 0 && c.report.is_object() && c.report.value("ok", true) == false &&
           c.report.contains("error") && c.report["error"].is_string() && !c.report["error"].get<std::string>().empty();
}
std::string export_text() {
    size_t len = 0;
    if (T()->export_layouts(nullptr, 0, &len) != KILN_ERR_BUFFER_TOO_SMALL) return "<sizing query did not answer BUFFER_TOO_SMALL>";
    std::string s(len + 1, '\0');
    size_t len2 = 0;
    if (T()->export_layouts(s.data(), s.size(), &len2) != KILN_OK || len2 != len) return "<export failed>";
    s.resize(len);
    return s;
}
KilnResult remote(KilnOwner owner, const std::string& snapshot, json* report = nullptr) {
    char rep[4096];
    size_t len = 0;
    const KilnResult rc = T()->remote_replace(owner, KilnStr{snapshot.data(), snapshot.size()}, rep, sizeof rep, &len);
    if (report) {
        try {
            *report = json::parse(std::string(rep, std::min(len, sizeof rep - 1)));
        } catch (...) {
        }
        if (!report->is_object()) *report = json::object();
    }
    return rc;
}
KilnStationMenusInfo info() {
    KilnStationMenusInfo i{};
    i.size = sizeof i;
    T()->info(&i);
    return i;
}
std::string drain() {
    size_t len = 0;
    T()->drain_applied(nullptr, 0, &len);
    std::string s(len + 1024, '\0');
    if (T()->drain_applied(s.data(), s.size(), &len) != KILN_OK) return "<drain failed>";
    s.resize(len);
    return s;
}
std::set<KilnOwner> g_revoked;

// ============================================================================================= the store ==========
void store_tests() {
    kc::ml::reset_for_tests();
    kc::ml::set_owner_probe([](KilnOwner o) { return g_revoked.count(o) > 0; });
    check(T() && T()->header.size == sizeof(KilnStationMenusApi) && T()->header.version == KILN_STATION_MENUS_VERSION, "table header");
    check(workshop_of(kWorkshopGuid) == kWorkshop, "fixture: FNV-1a over the GUID in .NET byte order");

    // define -> canonical bytes (sorted keys, compact), the same in export
    const Call a = define(7, fixture().dump(2));
    check(a.rc == KILN_OK && a.handle != 0 && a.generation == 1, "define: ok, generation 1");
    check(a.report.value("state", "") == "defined" && a.report.value("ok", false), "define: report state defined");
    const std::string e1 = export_text();
    check(e1 == std::string(R"({"format":1,"generation":1,"menus":[)") + kCanonical + "]}", "export: canonical record, container");
    // the exported record defined again (another spelling of the same bytes) changes nothing
    const std::string rec = json::parse(e1)["menus"][0].dump();
    check(rec == kCanonical, "canonical bytes = nlohmann dump of the export");
    const Call a2 = define(7, rec);
    check(a2.rc == KILN_OK && a2.handle == a.handle && a2.generation == 1 && a2.report.value("state", "") == "unchanged",
          "identical replay: same handle, same generation, unchanged");
    // remote_replace of the export: accepted, the remote collection holds the same bytes
    json rep;
    check(remote(900, e1, &rep) == KILN_OK && rep.value("state", "") == "replaced", "remote_replace(export): ok");
    {
        const auto eff = kc::ml::effective();
        check(eff.size() == 1 && eff[0].from_server && eff[0].rec->canonical == kCanonical && eff[0].generation == 1,
              "round trip: the remote record's bytes equal define's");
    }
    check(remote(900, e1, &rep) == KILN_OK && rep.value("state", "") == "unchanged", "remote_replace: identical snapshot unchanged");
    check(export_text() == e1, "export: local only, unchanged by the remote collection");
    check(T()->remote_clear(901) == KILN_ERR_INVALID_ARGUMENT, "remote_clear by another owner refused");
    check(T()->remote_clear(900) == KILN_OK && info().remote == 0, "remote_clear");

    // replacement: same handle, generation +1
    ojson f2 = fixture();
    f2["tabs"][0]["name"]["en"] = "Metals";
    const Call b = define(7, f2);
    check(b.rc == KILN_OK && b.handle == a.handle && b.generation == 2 && b.report.value("state", "") == "replaced",
          "replacement keeps the handle, bumps the generation");
    // wrong owner, second remove
    uint64_t g = 0;
    check(T()->remove(8, a.handle, &g) == KILN_ERR_INVALID_ARGUMENT, "remove by another owner refused");
    check(T()->remove(7, a.handle, &g) == KILN_OK && g == 3, "remove: generation +1");
    check(T()->remove(7, a.handle, &g) == KILN_ERR_NOT_FOUND, "second remove: NOT_FOUND");
    check(export_text() == R"({"format":1,"generation":3,"menus":[]})", "export after remove: empty");
    check(define(0, fixture()).rc == KILN_ERR_INVALID_ARGUMENT, "owner 0 refused");

    // reveal: two owners, one workshop; the newest wins, removing it shows the other again
    {
        kc::ml::reset_for_tests();
        const std::string w = gid(1, 0xa);
        const Call x = define(1, record_for(1, w));
        const Call y = define(2, record_for(2, w));
        check(x.rc == KILN_OK && y.rc == KILN_OK && y.generation == x.generation + 1, "two owners defined");
        auto menus = json::parse(export_text())["menus"];
        check(menus.size() == 1 && menus[0]["definition"]["key"] == "d2", "newest layer is effective");
        check(T()->remove(2, y.handle, &g) == KILN_OK, "remove the newest");
        menus = json::parse(export_text())["menus"];
        check(menus.size() == 1 && menus[0]["definition"]["key"] == "d1", "removal reveals the layer below");
        // a changed record of the older owner is accepted anew: it becomes the newest
        const Call y2 = define(2, record_for(2, w));
        const Call x2 = define(1, record_for(1, w, ojson::array({{{"key", "t"}, {"id", gid(5, 0x7)}, {"name", {{"*", "T"}}},
                                                                 {"iconFromTab", 0}, {"sets", ojson::array()}}})));
        check(y2.rc == KILN_OK && x2.rc == KILN_OK && x2.handle == x.handle && x2.report.value("state", "") == "replaced",
              "older owner's change replaces in place");
        menus = json::parse(export_text())["menus"];
        check(menus.size() == 1 && menus[0]["definition"]["key"] == "d1", "a replaced layer becomes the newest of its workshop");
        // two workshops: sorted by workshop id
        const std::string w2 = gid(2, 0xa);
        define(3, record_for(3, w2));
        menus = json::parse(export_text())["menus"];
        check(menus.size() == 2 && menus[0]["origin"]["workshopId"].get<uint32_t>() < menus[1]["origin"]["workshopId"].get<uint32_t>(),
              "export sorted by workshopId");
        // remote above local per workshop; remote_clear shows local again
        const std::string snap = json{{"format", 1}, {"generation", 41}, {"menus", json::array({json::parse(record_for(9, w).dump())})}}.dump();
        check(remote(900, snap) == KILN_OK, "remote snapshot");
        auto eff = kc::ml::effective();
        bool remote_w = false, local_w2 = false;
        for (auto& e : eff) {
            if (e.workshop == workshop_of(w)) remote_w = e.from_server && e.rec->def_key == "d9" && e.generation == 41;
            if (e.workshop == workshop_of(w2)) local_w2 = !e.from_server && e.rec->def_key == "d3";
        }
        check(remote_w && local_w2 && eff.size() == 2, "remote above local per workshop, local elsewhere");
        KilnStationMenusInfo i = info();
        check(i.remote == 1 && i.remote_generation == 41 && i.effective == 2 && i.layers == 3, "info: remote, counts");
        // a snapshot with a duplicate workshop or definition is refused whole: the remote collection stays
        const std::string dup_ws = json{{"format", 1}, {"generation", 42},
                                        {"menus", json::array({json::parse(record_for(10, w2).dump()), json::parse(record_for(11, w2).dump())})}}.dump();
        json r2;
        check(remote(900, dup_ws, &r2) == KILN_ERR_INVALID_ARGUMENT && r2.value("ok", true) == false && r2.contains("error"),
              "snapshot with a duplicate workshop refused");
        const std::string dup_def = json{{"format", 1}, {"generation", 42},
                                         {"menus", json::array({json::parse(record_for(10, w2).dump()), json::parse(record_for(10, w).dump())})}}.dump();
        check(remote(900, dup_def) == KILN_ERR_INVALID_ARGUMENT, "snapshot with a duplicate definition id refused");
        check(remote(900, R"({"format":1,"generation":43,"menus":[{"format":2}]})") == KILN_ERR_INVALID_ARGUMENT, "snapshot with a bad record refused");
        check(remote(900, R"({"format":2,"generation":43,"menus":[]})") == KILN_ERR_INVALID_ARGUMENT, "snapshot format 2 refused");
        check(remote(900, R"({"format":1,"generation":43,"menus":[],"x":1})") == KILN_ERR_INVALID_ARGUMENT, "snapshot unknown member refused");
        check(info().remote_generation == 41, "refused snapshots changed nothing");
        // menus [] is the tombstone: still a remote collection; local layers show
        check(remote(900, R"({"format":1,"generation":44,"menus":[]})") == KILN_OK && info().remote == 1 && info().remote_generation == 44,
              "tombstone: an empty remote collection");
        eff = kc::ml::effective();
        check(eff.size() == 2 && !eff[0].from_server && !eff[1].from_server, "tombstone: local layers show");
        check(T()->remote_clear(900) == KILN_OK && info().remote == 0, "remote_clear after the tombstone");
        // revoked owner: its layers go, the generation moves once
        const uint64_t before = info().generation;
        g_revoked.insert(1);
        i = info();
        check(i.layers == 2 && i.generation == before + 1, "revoked owner's layers dropped");
        check(define(1, record_for(1, w)).rc == KILN_ERR_REVOKED, "define by a revoked owner: REVOKED");
        g_revoked.clear();
    }

    // canonicalization of an unsorted, spaced input
    {
        kc::ml::reset_for_tests();
        const std::string spaced = "  \n" + fixture().dump(4) + "\n\t ";
        check(define(5, spaced).rc == KILN_OK && json::parse(export_text())["menus"][0].dump() == kCanonical, "spaced unsorted input canonicalized");
    }

    // refusals: each with a report error, nothing stored
    kc::ml::reset_for_tests();
    auto bad = [](const std::string& name, const std::function<void(ojson&)>& mut) {
        ojson r = fixture();
        mut(r);
        check(refused(define(9, r)), "refused: " + name);
    };
    bad("unknown top member", [](ojson& r) { r["extra"] = 1; });
    bad("unknown origin member", [](ojson& r) { r["origin"]["x"] = 1; });
    bad("unknown definition member", [](ojson& r) { r["definition"]["x"] = 1; });
    bad("unknown tab member", [](ojson& r) { r["tabs"][0]["iconSourceGroup"] = "8c91ca52-be38-468c-ae74-bc6a5ffab28e"; });
    bad("unknown set member", [](ojson& r) { r["tabs"][0]["sets"][0]["x"] = 1; });
    bad("unknown recipe member", [](ojson& r) { r["tabs"][0]["sets"][0]["recipes"][0]["x"] = 1; });
    bad("missing member", [](ojson& r) { r.erase("baseWorkshopId"); });
    bad("missing heading", [](ojson& r) { r["tabs"][0]["sets"][0].erase("heading"); });
    bad("format 2", [](ojson& r) { r["format"] = 2; });
    bad("format as text", [](ojson& r) { r["format"] = "1"; });
    bad("format as float", [](ojson& r) { r["format"] = 1.0; });
    bad("workshop id not FNV of the GUID", [](ojson& r) { r["origin"]["workshopId"] = kWorkshop + 1; });
    bad("workshop id negative", [](ojson& r) { r["origin"]["workshopId"] = -1; });
    bad("workshop id past u32", [](ojson& r) { r["origin"]["workshopId"] = 4294967296ull; });
    bad("upper-case GUID", [](ojson& r) { r["origin"]["workshopGuid"] = "66B9E41A-34D5-51C4-8CEC-36C189AB26CC"; });
    bad("GUID without dashes", [](ojson& r) { r["definition"]["id"] = "724d7b7ba362566bb0a75088f219266f"; });
    bad("nil tab id", [](ojson& r) { r["tabs"][0]["id"] = "00000000-0000-0000-0000-000000000000"; });
    bad("set 0 with a heading", [](ojson& r) { r["tabs"][0]["sets"][0]["heading"] = {{"en", "No"}}; });
    bad("heading as a string", [](ojson& r) { r["tabs"][0]["sets"][1]["heading"] = "Fine"; });
    bad("duplicate recipe id", [](ojson& r) { r["tabs"][0]["sets"][1]["recipes"].push_back({{"key", "m:x"}, {"recipeId", 3112649734u}}); });
    bad("recipe id 0", [](ojson& r) { r["tabs"][0]["sets"][0]["recipes"][0]["recipeId"] = 0; });
    bad("duplicate set key in a tab", [](ojson& r) { r["tabs"][0]["sets"][1]["key"] = "main"; });
    bad("set id = tab id", [](ojson& r) { r["tabs"][0]["sets"][1]["id"] = "27d00d31-a7ce-56d9-ab60-e2c50c02427d"; });
    bad("duplicate set id", [](ojson& r) { r["tabs"][0]["sets"][1]["id"] = "90598091-ecc6-50ab-bcb9-0941439c2e80"; });
    bad("duplicate tab key", [](ojson& r) {
        ojson t = r["tabs"][0];
        t["id"] = gid(77);
        t["sets"] = ojson::array();
        r["tabs"].push_back(t);
    });
    bad("duplicate tab id", [](ojson& r) {
        ojson t = r["tabs"][0];
        t["key"] = "other";
        t["sets"] = ojson::array();
        r["tabs"].push_back(t);
    });
    bad("iconFromTab 15", [](ojson& r) { r["tabs"][0]["iconFromTab"] = 15; });
    bad("iconFromTab -1", [](ojson& r) { r["tabs"][0]["iconFromTab"] = -1; });
    bad("key with a space", [](ojson& r) { r["tabs"][0]["key"] = "bad key"; });
    bad("key of 65", [](ojson& r) { r["tabs"][0]["key"] = std::string(65, 'k'); });
    bad("empty key", [](ojson& r) { r["tabs"][0]["sets"][0]["key"] = ""; });
    bad("definition key with a colon", [](ojson& r) { r["definition"]["key"] = "a:b"; });
    bad("recipe key without a mod", [](ojson& r) { r["tabs"][0]["sets"][0]["recipes"][0]["key"] = "reclaim"; });
    bad("recipe key a:b:c", [](ojson& r) { r["tabs"][0]["sets"][0]["recipes"][0]["key"] = "a:b:c"; });
    bad("recipe key empty mod", [](ojson& r) { r["tabs"][0]["sets"][0]["recipes"][0]["key"] = ":b"; });
    bad("mod with a slash", [](ojson& r) { r["origin"]["mod"] = "a/b"; });
    bad("language EN", [](ojson& r) { r["tabs"][0]["name"] = {{"EN", "Metal"}}; });
    bad("language e", [](ojson& r) { r["tabs"][0]["name"] = {{"e", "Metal"}}; });
    bad("language en-", [](ojson& r) { r["tabs"][0]["name"] = {{"en-", "Metal"}}; });
    bad("language en-x", [](ojson& r) { r["tabs"][0]["name"] = {{"en-x", "Metal"}}; });
    bad("no language", [](ojson& r) { r["tabs"][0]["name"] = ojson::object(); });
    bad("name as a string", [](ojson& r) { r["tabs"][0]["name"] = "Metal"; });
    bad("empty text", [](ojson& r) { r["tabs"][0]["name"]["en"] = ""; });
    bad("text of 4097 bytes", [](ojson& r) { r["tabs"][0]["name"]["en"] = std::string(4097, 'x'); });
    bad("36 languages", [](ojson& r) {
        ojson n = ojson::object();
        for (int i = 0; i < 36; ++i) n[std::string("l") + char('a' + i / 26) + char('a' + i % 26)] = "x";
        r["tabs"][0]["name"] = n;
    });
    bad("16 tabs", [](ojson& r) {
        r["tabs"] = ojson::array();
        for (int i = 0; i < 16; ++i)
            r["tabs"].push_back({{"key", "t" + std::to_string(i)}, {"id", gid(i, 0x1)}, {"name", {{"*", "T"}}}, {"iconFromTab", 0}, {"sets", ojson::array()}});
    });
    bad("29 sets", [](ojson& r) {
        ojson sets = ojson::array();
        for (int i = 0; i < 29; ++i)
            sets.push_back({{"key", "s" + std::to_string(i)}, {"id", gid(i, 0x2)}, {"heading", nullptr}, {"recipes", ojson::array()}});
        r["tabs"][0]["sets"] = sets;
    });
    bad("513 recipes in a tab (two sets)", [](ojson& r) {
        ojson a = ojson::array(), b = ojson::array();
        for (uint32_t i = 0; i < 513; ++i) (i < 300 ? a : b).push_back({{"key", "m:r" + std::to_string(i)}, {"recipeId", 1000 + i}});
        r["tabs"][0]["sets"][0]["recipes"] = a;
        r["tabs"][0]["sets"][1]["recipes"] = b;
    });
    check(refused(define(9, std::string("{\"format\":1")) ), "refused: not JSON");
    check(refused(define(9, fixture().dump() + " x")), "refused: trailing text");
    {
        std::string s = fixture().dump();
        s.replace(s.find("Metall"), 6, "Metal\xff");
        check(refused(define(9, s)), "refused: bad UTF-8 in a text");
        std::string d = fixture().dump();
        d.insert(1, "\"format\":1,");
        check(refused(define(9, d)), "refused: a member twice");
        std::string nested = std::string(40, '[') + std::string(40, ']');
        check(refused(define(9, nested)), "refused: deep nesting");
    }
    // the limits that are accepted: 15 tabs, 28 sets, 512 recipes in a tab, 35 languages, 4096 bytes
    {
        ojson r = fixture();
        r["tabs"] = ojson::array();
        uint32_t rid = 1;
        for (int t = 0; t < 15; ++t) {
            ojson sets = ojson::array();
            for (int s = 0; s < 28; ++s) {
                ojson rs = ojson::array();
                const int n = t == 0 ? (s < 27 ? 18 : 512 - 27 * 18) : 1;
                for (int k = 0; k < n; ++k) rs.push_back({{"key", "m:r"}, {"recipeId", rid++}});
                sets.push_back({{"key", "s" + std::to_string(s)}, {"id", gid(t * 100 + s, 0x2)}, {"heading", s ? ojson({{"*", "H"}}) : ojson(nullptr)}, {"recipes", rs}});
            }
            ojson name = ojson::object();
            for (int i = 0; i < 35; ++i) name[std::string("l") + char('a' + i / 26) + char('a' + i % 26)] = std::string(t == 0 && i == 0 ? 4096 : 3, 'x');
            r["tabs"].push_back({{"key", "t" + std::to_string(t)}, {"id", gid(t, 0x1)}, {"name", name}, {"iconFromTab", t % 15}, {"sets", sets}});
        }
        const Call c = define(9, r);
        check(c.rc == KILN_OK, "accepted: 15 tabs, 28 sets, 512 recipes in a tab, 35 languages, 4096 bytes");
    }
    check(info().layers == 1, "refusals stored nothing");

    // limits of the store: 256 layers; 64 MiB in all
    {
        kc::ml::reset_for_tests();
        const std::string w = gid(3, 0xa);
        KilnResult last = KILN_OK;
        int n = 0;
        for (; n < 300 && last == KILN_OK; ++n) last = define(100 + n, record_for(1000 + n, w)).rc;
        check(n == 257 && last == KILN_ERR_INVALID_ARGUMENT, "the 257th layer refused");
        const Call c = define(100, record_for(1000, w));
        check(c.rc == KILN_OK && c.report.value("state", "") == "unchanged", "a layer's identical replay still answers at the limit");
    }
    {
        kc::ml::reset_for_tests();
        const std::string w = gid(4, 0xa);
        ojson tabs = ojson::array();
        for (int t = 0; t < 15; ++t) {
            ojson name = ojson::object();
            for (int i = 0; i < 16; ++i) name[std::string("l") + char('a' + i / 26) + char('a' + i % 26)] = std::string(4096, char('a' + t));
            tabs.push_back({{"key", "t" + std::to_string(t)}, {"id", gid(t, 0x1)}, {"name", name}, {"iconFromTab", 0}, {"sets", ojson::array()}});
        }
        const size_t size = json::parse(record_for(1, w, tabs).dump()).dump().size();
        check(size < kc::ml::kMaxRecordBytes, "a record under 1 MiB");
        const int fit = static_cast<int>(kc::ml::kMaxAllBytes / size);
        int n = 0;
        KilnResult last = KILN_OK;
        Call c;
        for (; n < fit + 5 && last == KILN_OK; ++n) {
            c = define(200 + n, record_for(2000 + n, w, tabs));
            last = c.rc;
        }
        check(n == fit + 1 && refused(c), "64 MiB of records in all: the next one refused");
        ojson big = record_for(5, w, tabs);
        for (int i = 16; i < 35; ++i) big["tabs"][0]["name"][std::string("l") + char('a' + i / 26) + char('a' + i % 26)] = std::string(4096, 'z');
        for (int t = 1; t < 15; ++t)
            for (int i = 16; i < 35; ++i) big["tabs"][t]["name"][std::string("l") + char('a' + i / 26) + char('a' + i % 26)] = std::string(4096, 'z');
        kc::ml::reset_for_tests();
        check(refused(define(1, big)), "a record over 1 MiB refused");
    }

    // generation: never 0 after the first define; exhaustion refuses
    {
        kc::ml::reset_for_tests();
        check(info().generation == 0, "generation 0 before the first define");
        kc::ml::set_generation_for_tests(UINT64_MAX - 1);
        const Call c = define(1, record_for(1, gid(5, 0xa)));
        check(c.rc == KILN_OK && c.generation == UINT64_MAX, "the last generation");
        check(refused(define(1, record_for(2, gid(5, 0xa)))), "generation exhausted: refused");
    }

    // sizing queries, info size, pending
    {
        kc::ml::reset_for_tests();
        size_t len = 0;
        check(T()->export_layouts(nullptr, 0, &len) == KILN_ERR_BUFFER_TOO_SMALL && len == std::strlen(R"({"format":1,"generation":0,"menus":[]})"),
              "export sizing query");
        std::string buf(len, 'x');
        check(T()->export_layouts(buf.data(), len, &len) == KILN_ERR_BUFFER_TOO_SMALL, "export: capacity = length is too small");
        buf.assign(len + 1, 'x');
        check(T()->export_layouts(buf.data(), len + 1, &len) == KILN_OK && buf[len] == '\0', "export: length + 1 fits, NUL");
        KilnStationMenusInfo i{};
        i.size = sizeof i - 8;  // an older caller's shorter struct (review N2): filled as far as it reaches
        check(T()->info(&i) == KILN_OK && i.size == sizeof i - 8 && i.store == 1, "info: a shorter struct gets what fits");
        i = KilnStationMenusInfo{};
        i.size = 4;
        check(T()->info(&i) == KILN_ERR_INVALID_ARGUMENT, "info: a struct without store refused");
        i = info();
        check(i.size == sizeof i && i.store == 1 && i.render == 0 && i.detail[0] != 0, "info: store 1, render 0 with a reason (no probe)");
        kc::ml::set_render_probe([](uint32_t* reason, std::string* detail) {
            *reason = 0;
            *detail = "draws";
            return true;
        });
        check(info().render == 1, "info: render from the probe");
        check(T()->set_pending(1) == KILN_OK && info().pending == 1 && kc::ml::pending(), "set_pending(1)");
        check(T()->set_pending(2) == KILN_ERR_INVALID_ARGUMENT && info().pending == 1, "set_pending(2) refused");
        check(T()->set_pending(0) == KILN_OK && info().pending == 0, "set_pending(0)");
        KilnHandle h = 0;
        uint64_t gen = 0;
        check(T()->define(1, KilnStr{nullptr, 5}, &h, &gen, nullptr, 0, nullptr) == KILN_ERR_INVALID_ARGUMENT, "define: null text refused");
    }

    // the applied queue: JSON, at most 256, oldest dropped, drained once
    {
        kc::ml::reset_for_tests();
        check(drain() == "[]", "drain: empty array");
        for (uint32_t i = 0; i < 300; ++i)
            kc::ml::applied_push({gid(i), i, 5, i % 2 == 0, i % 3 == 0, i % 3 == 0 ? "" : "IconSourceMissing: x"});
        size_t len = 0;
        check(T()->drain_applied(nullptr, 0, &len) == KILN_ERR_BUFFER_TOO_SMALL && len > 0, "drain sizing query keeps the queue");
        const json d = json::parse(drain());
        check(d.is_array() && d.size() == 256 && d[0]["workshop"] == 44 && d[255]["workshop"] == 299, "drain: 256, oldest dropped");
        check(d[0].size() == 6 && d[0]["definition"] == gid(44) && d[0]["generation"] == 5 && d[0]["fromServer"] == true &&
                  d[0]["ok"] == false && d[0]["reason"] == "IconSourceMissing: x",
              "drain: entry members");
        check(drain() == "[]", "drain: drained");
    }
}

// ============================================================================================= the materializer ===
alignas(16) uint8_t g_arena[8 << 20];
constexpr size_t kGame = 1 << 20;  // the made-up game data below this offset; near memory above
uintptr_t A(size_t off) { return reinterpret_cast<uintptr_t>(g_arena) + off; }
std::vector<uint8_t> g_game;  // the game's data as set up, restored before every case

void put_header(uintptr_t field, uintptr_t data, uint32_t count) {
    const int32_t off = data ? static_cast<int32_t>(static_cast<int64_t>(data) - static_cast<int64_t>(field)) : 0;
    std::memcpy(reinterpret_cast<void*>(field), &off, 4);
    std::memcpy(reinterpret_cast<void*>(field + 4), &count, 4);
}
std::pair<uintptr_t, uint32_t> header_at(uintptr_t field) {
    int32_t off = 0;
    uint32_t n = 0;
    std::memcpy(&off, reinterpret_cast<void*>(field), 4);
    std::memcpy(&n, reinterpret_cast<void*>(field + 4), 4);
    return {off ? field + static_cast<intptr_t>(off) : 0, n};
}
uint64_t raw8(uintptr_t a) {
    uint64_t v;
    std::memcpy(&v, reinterpret_cast<void*>(a), 8);
    return v;
}
const uintptr_t kColl = A(0x100);
const uint32_t kIds[3] = {kBase, 0x11111111u, 0x22222222u};
uint8_t icon_byte(uint32_t tab, int i) { return static_cast<uint8_t>(0x40 + tab * 16 + i); }

// The collection: three workshops; the base has three tabs of two sets of two recipes; the others one tab each.
void setup_game() {
    std::memset(g_arena, 0, sizeof g_arena);
    put_header(kColl, A(0x1000), 3);
    put_header(kColl + 8, A(0x2000), 3);
    std::memcpy(reinterpret_cast<void*>(A(0x1000)), kIds, sizeof kIds);
    for (uint32_t t = 0; t < 3; ++t) {
        uint8_t* grp = reinterpret_cast<uint8_t*>(A(0x3000 + t * 0x38));
        std::memset(grp, 0xA0 + t, 16);                                 // groupId
        for (int i = 0; i < 16; ++i) grp[0x10 + i] = icon_byte(t, i);   // icon
        std::memset(grp + 0x20, 0xB0 + t, 16);                          // label
        put_header(A(0x3000 + t * 0x38 + 0x30), A(0x4000 + t * 0x100), 2);
        for (uint32_t s = 0; s < 2; ++s) {
            uint8_t* set = reinterpret_cast<uint8_t*>(A(0x4000 + t * 0x100 + s * 0x28));
            std::memset(set, 0xC0 + t * 2 + s, 16);
            std::memset(set + 0x10, 0xD0 + t * 2 + s, 16);
            const uint32_t ids[2] = {900 + t * 10 + s * 2, 901 + t * 10 + s * 2};
            std::memcpy(reinterpret_cast<void*>(A(0x6000 + (t * 2 + s) * 0x40)), ids, 8);
            put_header(A(0x4000 + t * 0x100 + s * 0x28 + 0x20), A(0x6000 + (t * 2 + s) * 0x40), 2);
        }
    }
    put_header(A(0x2000), A(0x3000), 3);
    put_header(A(0x2008), A(0x8000), 1);
    put_header(A(0x2010), A(0x8100), 1);
    g_game.assign(g_arena, g_arena + kGame);
}
void restore_game() { std::memcpy(g_arena, g_game.data(), kGame); }
bool game_intact() { return std::memcmp(g_arena, g_game.data(), kGame) == 0; }
bool headers_original() { return raw8(kColl) == raw8(reinterpret_cast<uintptr_t>(g_game.data()) + 0x100) && raw8(kColl + 8) == raw8(reinterpret_cast<uintptr_t>(g_game.data()) + 0x108); }

struct FakeIo final : kc::ml::MenuIo {
    long allocs = 0, fail_alloc = -1;
    uintptr_t next = A(kGame);
    std::set<uintptr_t> fail_write;  // writes to these fail
    bool transient = false;          // ... only the first time (a batch that did not land; single writes later do)
    long writes = 0;
    std::map<uint32_t, TextMap> texts_set;
    bool read(uintptr_t a, void* o, size_t n) override {
        if (a < A(0) || a + n > A(sizeof g_arena)) return false;
        std::memcpy(o, reinterpret_cast<void*>(a), n);
        return true;
    }
    void* alloc(uintptr_t, size_t n) override {
        if (allocs++ == fail_alloc) return nullptr;
        const uintptr_t p = (next + 15) & ~uintptr_t(15);
        if (p + n > A(sizeof g_arena)) return nullptr;
        next = p + n;
        std::memset(reinterpret_cast<void*>(p), 0xCD, n);  // what the code leaves unset shows
        return reinterpret_cast<void*>(p);
    }
    size_t write(std::vector<kc::Write>& w, uintptr_t* failed) override {
        size_t done = 0;
        for (const kc::Write& x : w) {
            ++writes;
            if (fail_write.count(x.addr)) {
                if (failed && !*failed) *failed = x.addr;
                if (transient) fail_write.erase(x.addr);
                continue;
            }
            std::memcpy(reinterpret_cast<void*>(x.addr), x.bytes, x.n);
            ++done;
        }
        return done;
    }
    void guid(const std::string& key, uint8_t out[16]) override {
        const uint64_t a = fnv64(key), b = fnv64(key + "#");
        std::memcpy(out, &a, 8);
        std::memcpy(out + 8, &b, 8);
    }
    void texts(uint32_t tag, const TextMap& t) override { texts_set[tag] = t; }
};
kc::ml::Geometry geo() {
    kc::ml::Geometry g;
    g.g_id = 0;
    g.g_label = 0x20;
    g.s_id = 0x10;
    g.s_label = 0;
    return g;
}
kc::ml::OwnState fresh_state() {
    kc::ml::OwnState st;
    st.orig[0] = raw8(kColl);
    st.orig[1] = raw8(kColl + 8);
    for (int i = 0; i < 3; ++i) st.trees.push_back(raw8(A(0x2000 + i * 8)));
    return st;
}
std::shared_ptr<const kc::ml::Record> parse(const ojson& j) {
    auto r = std::make_shared<kc::ml::Record>();
    std::string err;
    if (!kc::ml::parse_record(j.dump(), r.get(), &err)) std::fprintf(stderr, "fixture refused: %s\n", err.c_str());
    return r;
}
// The test layout of the own workshop: tab "metal" (icon of base tab 1; sets: main [11,12], fine with a heading [13], empty),
// tab "wood" (icon of base tab 0, one empty set).
ojson layout_json(uint32_t metal_icon = 1, const std::string& metal_en = "Metal") {
    ojson sets = ojson::array({{{"key", "main"}, {"id", gid(1, 0x5)}, {"heading", nullptr},
                                {"recipes", ojson::array({{{"key", "m:a"}, {"recipeId", 11}}, {{"key", "m:b"}, {"recipeId", 12}}})}},
                               {{"key", "fine"}, {"id", gid(2, 0x5)}, {"heading", {{"*", "Fine work"}}},
                                {"recipes", ojson::array({{{"key", "m:c"}, {"recipeId", 13}}})}},
                               {{"key", "empty"}, {"id", gid(3, 0x5)}, {"heading", nullptr}, {"recipes", ojson::array()}}});
    ojson tabs = ojson::array({{{"key", "metal"}, {"id", gid(1, 0x4)}, {"name", {{"en", metal_en}, {"de", "Metall"}}}, {"iconFromTab", metal_icon}, {"sets", sets}},
                               {{"key", "wood"}, {"id", gid(2, 0x4)}, {"name", {{"*", "Wood"}}}, {"iconFromTab", 0},
                                 {"sets", ojson::array({{{"key", "main"}, {"id", gid(4, 0x5)}, {"heading", nullptr}, {"recipes", ojson::array()}}})}}});
    return record_for(1, kWorkshopGuid, tabs);
}
kc::ml::OwnWorkshop own(std::shared_ptr<const kc::ml::Record> rec, uint64_t generation = 1) {
    kc::ml::OwnWorkshop o;
    o.workshop = rec->workshop;
    o.layout = {rec->workshop, rec, false, generation};
    return o;
}
bool guid_at(uintptr_t a, const uint8_t g[16]) { return std::memcmp(reinterpret_cast<void*>(a), g, 16) == 0; }
bool zero16(uintptr_t a) {
    static const uint8_t z[16] = {};
    return guid_at(a, z);
}
std::vector<uint32_t> ids_at(uintptr_t field) {
    const auto [d, n] = header_at(field);
    std::vector<uint32_t> v(n);
    if (n) std::memcpy(v.data(), reinterpret_cast<void*>(d), n * 4ull);
    return v;
}
// The own workshop's tree (groups, count) in the published collection, or {0, ~0} when it is not listed.
std::pair<uintptr_t, uint32_t> tree_of(uint32_t workshop) {
    const auto ids = ids_at(kColl);
    const auto [trees, n] = header_at(kColl + 8);
    for (uint32_t i = 0; i < ids.size() && i < n; ++i)
        if (ids[i] == workshop) return header_at(trees + i * 8);
    return {0, ~0u};
}
uint32_t label_tag(uintptr_t a) {
    uint8_t g[16];
    std::memcpy(g, reinterpret_cast<void*>(a), 16);
    return fnv32(g, 16);
}

void tree_bytes() {
    setup_game();
    kc::ml::reset_for_tests();
    FakeIo io;
    auto st = fresh_state();
    const auto rec = parse(layout_json());
    const auto res = kc::ml::own_publish(kColl, {own(rec)}, geo(), io, st);
    check(res.ok && res.published && st.on && res.own == 1, "layout tree published");
    // the collection: the game's three workshops (rebased, same data) and the own one
    const auto ids = ids_at(kColl);
    check(ids.size() == 4 && ids[0] == kIds[0] && ids[1] == kIds[1] && ids[2] == kIds[2] && ids[3] == kWorkshop, "workshops: game's, then own");
    const auto [trees, tn] = header_at(kColl + 8);
    check(tn == 4, "trees: 4");
    for (int i = 0; i < 3; ++i)
        check(header_at(trees + i * 8) == header_at(A(0x2000 + i * 8)), "tree " + std::to_string(i) + " rebased to the game's");
    const auto [G, gn] = header_at(trees + 3 * 8);
    check(gn == 2 && G >= A(kGame), "own tree: 2 tabs in near memory");
    // tab 0: icon of base tab 1, the layout's id and label, 3 sets
    const auto& t0 = rec->tabs[0];
    bool icon0 = true;
    for (int i = 0; i < 16; ++i) icon0 = icon0 && reinterpret_cast<uint8_t*>(G)[0x10 + i] == icon_byte(1, i);
    check(icon0, "tab 0: icon of base tab 1");
    check(guid_at(G + 0, t0.id), "tab 0: groupId = the tab's id");
    uint8_t want_id[16];
    guid_bytes(gid(1, 0x4), want_id);
    check(guid_at(G + 0, want_id), "tab 0: id in the game's byte order");
    const uint32_t tag0 = label_tag(G + 0x20);
    check(!zero16(G + 0x20) && io.texts_set.count(tag0) && io.texts_set[tag0] == TextMap{{"de", "Metall"}, {"en", "Metal"}}, "tab 0: label answers the name");
    const auto [S, sn] = header_at(G + 0x30);
    check(sn == 3 && S >= A(kGame), "tab 0: 3 sets");
    check(zero16(S + 0) && guid_at(S + 0x10, t0.sets[0].id) && ids_at(S + 0x20) == std::vector<uint32_t>{11, 12}, "set 0: no label, id, [11, 12]");
    const uint32_t tag1 = label_tag(S + 0x28);
    check(!zero16(S + 0x28) && io.texts_set[tag1] == TextMap{{"*", "Fine work"}} && guid_at(S + 0x28 + 0x10, t0.sets[1].id) &&
              ids_at(S + 0x28 + 0x20) == std::vector<uint32_t>{13},
          "set 1 (stride 0x28): heading label, id, [13]");
    check(zero16(S + 0x50) && guid_at(S + 0x50 + 0x10, t0.sets[2].id) && raw8(S + 0x50 + 0x20) == 0, "set 2: no label, no entries (header 0)");
    // tab 1 (stride 0x38): icon of base tab 0, one empty set
    bool icon1 = true;
    for (int i = 0; i < 16; ++i) icon1 = icon1 && reinterpret_cast<uint8_t*>(G + 0x38)[0x10 + i] == icon_byte(0, i);
    const auto [S1, sn1] = header_at(G + 0x38 + 0x30);
    check(icon1 && guid_at(G + 0x38, rec->tabs[1].id) && sn1 == 1 && zero16(S1) && raw8(S1 + 0x20) == 0,
          "tab 1 at +0x38: icon of base tab 0, id, one set without label or entries");
    check(io.texts_set[label_tag(G + 0x38 + 0x20)] == TextMap{{"*", "Wood"}}, "tab 1: label answers its name");
    // the game's data: only the two headers changed
    std::vector<uint8_t> now(g_arena, g_arena + kGame);
    std::memcpy(now.data() + 0x100, g_game.data() + 0x100, 16);
    check(std::memcmp(now.data(), g_game.data(), kGame) == 0, "the game's data untouched but the two headers");
    // applied: one entry, ok
    const json d = json::parse(drain());
    check(d.size() == 1 && d[0]["ok"] == true && d[0]["workshop"] == kWorkshop && d[0]["definition"] == rec->def_id_text && d[0]["generation"] == 1 &&
              d[0]["fromServer"] == false && d[0]["reason"] == "",
          "applied: ok entry");
    // the same layout again: the cache, no new memory, no new entry
    const long before = io.allocs;
    const auto res2 = kc::ml::own_publish(kColl, {own(rec)}, geo(), io, st);
    check(res2.ok && tree_of(kWorkshop).first == G && io.allocs == before + 2, "same layout: its tree reused (only the two lists are new)");
    check(drain() == "[]", "same layout: no new applied entry");
    // no own workshop left: both headers back
    const auto res3 = kc::ml::own_publish(kColl, {}, geo(), io, st);
    check(res3.ok && res3.restored && headers_original() && !st.on, "none left: both headers back to the game's");
    // zero tabs, or a tab of no set (review S2): never handed to the game; the workshop is not listed (no placements here)
    drain();
    restore_game();
    auto st2 = fresh_state();
    const auto empty = parse(record_for(2, kWorkshopGuid));
    const auto res4 = kc::ml::own_publish(kColl, {own(empty)}, geo(), io, st2);
    check(tree_of(kWorkshop).second == ~0u && res4.own == 0, "zero tabs: refused, not listed");
    restore_game();
    auto st3 = fresh_state();
    const auto nosets = parse(record_for(3, kWorkshopGuid, ojson::array({{{"key", "t"}, {"id", gid(6, 0x7)}, {"name", {{"*", "T"}}}, {"iconFromTab", 0}, {"sets", ojson::array()}}})));
    kc::ml::own_publish(kColl, {own(nosets)}, geo(), io, st3);
    check(tree_of(kWorkshop).second == ~0u, "a tab of no set: refused, not listed");
    const json dz = json::parse(drain());
    bool emptyWord = dz.size() == 2;
    for (const auto& e : dz) emptyWord = emptyWord && e["ok"] == false && e["reason"].get<std::string>().rfind("EmptyLayout", 0) == 0;
    check(emptyWord, "applied: EmptyLayout for both");
}

void faults() {
    setup_game();
    kc::ml::reset_for_tests();
    const auto rec = parse(layout_json());
    long total = 0;
    {
        FakeIo io;
        auto st = fresh_state();
        kc::ml::own_publish(kColl, {own(rec)}, geo(), io, st);
        total = io.allocs;
    }
    check(total == 7, "the layout build allocates 7 times (lists 2, tabs 1, sets 2, entries 2)");
    for (long i = 0; i < total; ++i) {
        restore_game();
        FakeIo io;
        io.fail_alloc = i;
        auto st = fresh_state();
        const auto res = kc::ml::own_publish(kColl, {own(rec)}, geo(), io, st);
        check(!res.ok && !res.published && io.writes == 0 && game_intact() && !st.on, "allocation " + std::to_string(i) + " fails: nothing written");
    }
    for (int h = 0; h < 2; ++h) {
        restore_game();
        FakeIo io;
        io.fail_write = {kColl + 8ull * h};
        auto st = fresh_state();
        const auto res = kc::ml::own_publish(kColl, {own(rec)}, geo(), io, st);
        check(!res.ok && !res.published && res.failed == kColl + 8ull * h && headers_original() && !st.on,
              "header " + std::to_string(h) + " write fails: both headers back to the game's");
    }
    // a rebuild over a published menu: a failed allocation leaves the published pair; a failed write puts the game's back
    const auto rec2 = parse(layout_json(2, "Metals"));
    for (long i = 0; i < total; ++i) {
        restore_game();
        FakeIo io;
        auto st = fresh_state();
        kc::ml::own_publish(kColl, {own(rec)}, geo(), io, st);
        const uint64_t h0 = raw8(kColl), h1 = raw8(kColl + 8);
        io.fail_alloc = io.allocs + i;
        const long w = io.writes;
        const auto res = kc::ml::own_publish(kColl, {own(rec2)}, geo(), io, st);
        check(!res.ok && io.writes == w && raw8(kColl) == h0 && raw8(kColl + 8) == h1 && st.on,
              "rebuild, allocation " + std::to_string(i) + " fails: the published pair stays");
    }
    for (int h = 0; h < 2; ++h) {
        restore_game();
        FakeIo io;
        auto st = fresh_state();
        kc::ml::own_publish(kColl, {own(rec)}, geo(), io, st);
        io.fail_write = {kColl + 8ull * h};
        io.transient = true;
        const auto res = kc::ml::own_publish(kColl, {own(rec2)}, geo(), io, st);
        check(!res.ok && headers_original() && !st.on, "rebuild, header " + std::to_string(h) + " write fails once: both back to the game's");
    }
    for (int h = 0; h < 2; ++h) {
        restore_game();
        FakeIo io;
        auto st = fresh_state();
        kc::ml::own_publish(kColl, {own(rec)}, geo(), io, st);
        const uint64_t h0 = raw8(kColl), h1 = raw8(kColl + 8);
        io.fail_write = {kColl + 8ull * h};
        const auto res = kc::ml::own_publish(kColl, {own(rec2)}, geo(), io, st);
        check(!res.ok && raw8(kColl) == h0 && raw8(kColl + 8) == h1 && st.on,
              "rebuild, header " + std::to_string(h) + " cannot be written: both stay the pair shown before");
        const auto res2 = kc::ml::own_publish(kColl, {}, geo(), io, st);
        check(!res2.ok && raw8(kColl) == h0 && raw8(kColl + 8) == h1 && st.on, "none left, header " + std::to_string(h) + " cannot be written: the pair stays");
        io.fail_write.clear();
        const auto res3 = kc::ml::own_publish(kColl, {}, geo(), io, st);
        check(res3.ok && res3.restored && headers_original() && !st.on, "none left: both back once the header can be written");
    }
    const json d = json::parse(drain());
    bool all = !d.empty();
    for (const auto& e : d) all = all && (e["ok"] == true || e["reason"].get<std::string>().rfind("PublishFailed", 0) == 0);
    check(all, "applied: failed builds report PublishFailed");
}

void icons_and_caps() {
    setup_game();
    kc::ml::reset_for_tests();
    // icon past the base tree, nothing before: not listed, IconSourceMissing
    {
        FakeIo io;
        auto st = fresh_state();
        const auto bad = parse(layout_json(3));
        const auto res = kc::ml::own_publish(kColl, {own(bad)}, geo(), io, st);
        check(res.ok && tree_of(kWorkshop).second == ~0u && headers_original(), "icon 3 of 3 tabs: refused, not listed, headers the game's");
        const json d = json::parse(drain());
        check(d.size() == 1 && d[0]["ok"] == false && d[0]["reason"].get<std::string>().rfind("IconSourceMissing: ", 0) == 0, "applied: IconSourceMissing");
        // with placements: the old way
        auto o = own(bad);
        o.menu = kBase;
        o.places = {{0, 1, 77}};
        const auto res2 = kc::ml::own_publish(kColl, {o}, geo(), io, st);
        const auto [G, gn] = tree_of(kWorkshop);
        check(res2.ok && gn == 1, "refused layout with placements: the placements' tree");
        bool icon = true;
        for (int i = 0; i < 16; ++i) icon = icon && reinterpret_cast<uint8_t*>(G)[0x10 + i] == icon_byte(0, i);
        const auto [S, sn] = header_at(G + 0x30);
        check(icon && sn == 1 && ids_at(S + 0x20) == std::vector<uint32_t>{77} && reinterpret_cast<uint8_t*>(S)[0] == 0xC1, "placements: base tab 0, its set 1, [77]");
        check(drain() == "[]", "the same refusal is reported once");
    }
    // icon missing with a good tree before: the good tree stays
    {
        restore_game();
        FakeIo io;
        auto st = fresh_state();
        kc::ml::own_publish(kColl, {own(parse(layout_json()))}, geo(), io, st);
        const auto good = tree_of(kWorkshop);
        const auto res = kc::ml::own_publish(kColl, {own(parse(layout_json(9)), 2)}, geo(), io, st);
        check(res.ok && tree_of(kWorkshop) == good, "icon 9: the last good tree stays");
        const json d = json::parse(drain());
        check(d.size() == 2 && d[1]["ok"] == false && d[1]["generation"] == 2 && d[1]["reason"].get<std::string>().rfind("IconSourceMissing", 0) == 0,
              "applied: ok, then IconSourceMissing");
    }
    // a base workshop the menu lacks
    {
        restore_game();
        FakeIo io;
        auto st = fresh_state();
        ojson j = layout_json();
        j["baseWorkshopId"] = 0x99999999u;
        const auto res = kc::ml::own_publish(kColl, {own(parse(j))}, geo(), io, st);
        const json d = json::parse(drain());
        check(res.ok && tree_of(kWorkshop).second == ~0u && d.size() == 1 && d[0]["reason"].get<std::string>().rfind("IconSourceMissing", 0) == 0,
              "base workshop not in the menu: IconSourceMissing");
    }
    // labels unknown: LayoutUnknown
    {
        restore_game();
        FakeIo io;
        auto st = fresh_state();
        auto g = geo();
        g.g_label = -1;
        kc::ml::own_publish(kColl, {own(parse(layout_json()))}, g, io, st);
        const json d = json::parse(drain());
        check(d.size() == 1 && d[0]["reason"].get<std::string>().rfind("LayoutUnknown", 0) == 0 && tree_of(kWorkshop).second == ~0u, "labels unknown: LayoutUnknown");
    }
    // 16 distinct menus per workshop, then RestartRequired (the 16th stays); a known one is reused
    {
        restore_game();
        kc::ml::reset_for_tests();
        FakeIo io;
        auto st = fresh_state();
        std::pair<uintptr_t, uint32_t> sixteenth, third;
        for (int i = 0; i < 16; ++i) {
            kc::ml::own_publish(kColl, {own(parse(layout_json(1, "Metal " + std::to_string(i))), i + 1)}, geo(), io, st);
            if (i == 2) third = tree_of(kWorkshop);
        }
        sixteenth = tree_of(kWorkshop);
        drain();
        const long before = io.allocs;
        kc::ml::own_publish(kColl, {own(parse(layout_json(1, "Metal 16")), 17)}, geo(), io, st);
        const json d = json::parse(drain());
        check(tree_of(kWorkshop) == sixteenth && io.allocs == before + 2 && d.size() == 1 &&
                  d[0]["reason"].get<std::string>().rfind("RestartRequired", 0) == 0,
              "the 17th distinct menu: RestartRequired, the 16th stays, no tree memory");
        kc::ml::own_publish(kColl, {own(parse(layout_json(1, "Metal 2")), 18)}, geo(), io, st);
        check(tree_of(kWorkshop) == third && json::parse(drain())[0]["ok"] == true, "a menu built before: reused, ok");
        check(info().materialized == 16, "info: 16 materialized");
    }
}

void hold() {
    kc::ml::reset_for_tests();
    check(kc::ml::absent_id({}) == 1 && kc::ml::absent_id({1, 2, 3}) == 4 && kc::ml::absent_id({2, 5}) == 1 && kc::ml::absent_id({3, 1, 1}) == 2,
          "absent_id: the smallest id above 0 the list lacks");
    const std::vector<uint32_t> game = {kBase, 5}, live = {kBase, 5, 1, kWorkshop};
    check(kc::ml::hold_workshop(kWorkshop, game, live, false) == 0, "not pending: no hold");
    kc::ml::set_pending(true);
    check(kc::ml::hold_workshop(kBase, game, live, false) == 0, "pending: a game workshop is not held");
    check(kc::ml::hold_workshop(kWorkshop, game, live, false) == 2, "pending: an own workshop asks for an id the live list lacks");
    check(kc::ml::hold_workshop(77, {}, live, true) == 2, "pending, no game list: a definition's own workshop is held");
    check(kc::ml::hold_workshop(77, {}, live, false) == 0, "pending, no game list, nothing says it is own: no hold");
    define(1, record_for(1, gid(9, 0xa)));
    check(kc::ml::hold_workshop(workshop_of(gid(9, 0xa)), {}, live, false) == 2, "pending: a workshop with a layout is held");
    check(kc::ml::hold_workshop(0, game, live, true) == 0, "workshop 0 is never held");
    kc::ml::set_pending(false);
}
}  // namespace

void run(const char* name, void (*fn)()) {
    try {
        fn();
    } catch (const std::exception& e) {
        check(false, std::string(name) + ": exception " + e.what());
    }
}

// review N1 and N3: a new server forgets the last good menus; a game workshop id never gets a second entry
void review_fixes() {
    setup_game();
    kc::ml::reset_for_tests();
    FakeIo io;
    auto st = fresh_state();
    const auto rec = parse(layout_json());
    kc::ml::own_publish(kColl, {own(rec)}, geo(), io, st);
    const auto bad = parse(layout_json(14));  // a base tab the game's menu does not have: IconSourceMissing
    kc::ml::own_publish(kColl, {own(bad)}, geo(), io, st);
    check(tree_of(kWorkshop).second == 2, "N1: a refused layout keeps the last good one (same server)");
    check(remote(900, R"({"format":1,"generation":1,"menus":[]})") == KILN_OK &&
              T()->remote_clear(900) == KILN_OK, "N1: remote collection set and cleared");
    kc::ml::own_publish(kColl, {own(bad)}, geo(), io, st);
    check(tree_of(kWorkshop).second == ~0u, "N1: after another server, the old good menu is not shown");
    drain();
    restore_game();
    auto st2 = fresh_state();
    auto o = own(rec);
    o.workshop = kIds[0];
    kc::ml::own_publish(kColl, {o}, geo(), io, st2);
    const auto ids = ids_at(kColl);
    check(std::count(ids.begin(), ids.end(), kIds[0]) == 1, "N3: a game workshop id is listed once (the game's)");
    const json d = json::parse(drain());
    check(d.size() == 1 && d[0]["ok"] == false && d[0]["reason"].get<std::string>().rfind("GameWorkshop", 0) == 0, "N3: applied says GameWorkshop");
}

int main() {
    run("store", &store_tests);
    run("review fixes", &review_fixes);
    run("tree bytes", &tree_bytes);
    run("faults", &faults);
    run("icons and caps", &icons_and_caps);
    run("hold", &hold);
    std::printf("menulayout-test: %d check(s), %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
