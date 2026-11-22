// SPDX-License-Identifier: MIT
// Compatibility rules and the overlap report (core/src/compat_rules.cpp, compat_overlaps.cpp) under Wine, no game:
//   compat_test.exe <schema/fixtures dir>
// The rule cases are schema/fixtures/compat-rules/cases.json, the file the launcher's Rust reader is tested with too.
#include <windows.h>

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

#include "chain_status.h"
#include "compat.h"
#include "compat_rules.h"
#include "hook_owner.h"
#include "module_place.h"
#include "nlohmann/json.hpp"

namespace {

using json = nlohmann::ordered_json;
using namespace kiln;
using namespace kiln::compat;

int g_checks = 0, g_failed = 0;

void check(bool ok, const std::string& what) {
    ++g_checks;
    if (!ok) {
        ++g_failed;
        std::printf("FAIL %s\n", what.c_str());
    }
}

std::string slurp(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

Rule one_rule(const json& j, const char* source = "test") {
    json doc = {{"format", 1}, {"rules", json::array({j})}};
    ParseResult r = parse_rules(doc.dump(), source);
    return r.rules.empty() ? Rule{} : r.rules[0];
}

std::vector<std::string> strings(const json& j) {
    std::vector<std::string> out;
    if (j.is_array())
        for (const auto& s : j) out.push_back(s.get<std::string>());
    return out;
}

void rule_cases(const std::string& dir) {
    const json cases = json::parse(slurp(dir + "/compat-rules/cases.json"));
    for (const auto& c : cases["glob"])
        check(glob_match(c["pattern"].get<std::string>(), c["name"].get<std::string>()) == c["match"].get<bool>(),
              "glob " + c["pattern"].get<std::string>() + " vs " + c["name"].get<std::string>());
    for (const auto& c : cases["parse"]) {
        const std::string name = "parse: " + c["name"].get<std::string>();
        const std::string text = c["input"].is_string() ? c["input"].get<std::string>() : c["input"].dump();
        const ParseResult r = parse_rules(text, "test");
        check(r.ok == c["ok"].get<bool>(), name + " (ok)");
        std::vector<std::string> ids;
        for (const Rule& rule : r.rules) ids.push_back(rule.id);
        check(ids == strings(c["rules"]), name + " (kept ids)");
        check(r.dropped == c["dropped"].get<int>(), name + " (dropped " + std::to_string(r.dropped) + ")");
        // the problem lines: one per dropped rule unless the case says otherwise (the rules past the limit give one summary)
        const size_t problems = c.contains("problems") ? c["problems"].get<size_t>() : static_cast<size_t>(c["dropped"].get<int>());
        check(!r.ok || r.problems.size() == problems, name + " (" + std::to_string(r.problems.size()) + " problem lines)");
        if (c.contains("lastProblem"))
            check(!r.problems.empty() && r.problems.back() == c["lastProblem"].get<std::string>(),
                  name + " (last problem: " + (r.problems.empty() ? std::string("none") : r.problems.back()) + ")");
    }
    for (const auto& c : cases["merge"]) {
        const std::string name = "merge: " + c["name"].get<std::string>();
        auto load = [](const json& list) {
            std::vector<Rule> out;
            if (list.is_array())
                for (const auto& j : list) out.push_back(one_rule(j));
            return out;
        };
        std::vector<std::string> replaced;
        const auto merged = merge_rules(load(c["shipped"]), load(c["user"]), &replaced);
        std::vector<std::string> ids;
        for (const Rule& r : merged) ids.push_back(r.id);
        check(ids == strings(c["ids"]), name + " (ids)");
        check(replaced == strings(c["replaced"]), name + " (replaced)");
        if (c.contains("messages"))
            for (const auto& [id, message] : c["messages"].items())
                for (const Rule& r : merged)
                    if (r.id == id) check(r.message == message.get<std::string>(), name + " (message of " + id + ")");
    }
    for (const auto& c : cases["evaluate"]) {
        const std::string name = "evaluate: " + c["name"].get<std::string>();
        std::vector<Rule> rules;
        for (const auto& j : c["rules"]) rules.push_back(one_rule(j));
        Facts f;
        f.modules = strings(c["facts"]["modules"]);
        f.engines = strings(c["facts"]["engines"]);
        f.mods = strings(c["facts"]["mods"]);
        f.symbols = strings(c["facts"]["symbols"]);
        std::vector<std::string> fired;
        for (const Rule* r : evaluate(rules, f)) fired.push_back(r->id);
        check(fired == strings(c["fires"]), name);
    }
}

// ---- overlaps ----

ChainRow row(const char* file, const char* status, bool late = false, const char* source = "chain") {
    ChainRow r;
    r.path = std::string("Z:\\game\\kiln\\chain\\") + file;
    r.file = file;
    r.source = source;
    r.status = status;
    r.late = late;
    return r;
}

// Each link is labelled like the target's symbol (a service hooks a symbol under its own name).
json target(const char* rva, const char* symbol, std::vector<const char*> owners, json before = nullptr, json over = nullptr) {
    json links = json::array();
    for (const char* o : owners) links.push_back({{"owner", o}, {"kind", "raw"}, {"priority", 0}, {"label", symbol ? symbol : "x"}});
    return {{"rva", rva}, {"symbol", symbol ? json(symbol) : json(nullptr)}, {"label", symbol ? symbol : "rva"}, {"links", links},
            {"foreignBefore", before}, {"foreignOver", over}};
}

json dest(const char* module, const char* offset, bool own = false) {
    return {{"module", module}, {"offset", offset}, {"address", "0x7ff000001000"}, {"own", own}};
}

void overlap_cases() {
    check(normalize_owner("service:kiln-chat.dll") == "chat", "owner service:kiln-chat.dll");
    check(normalize_owner("plugin:kiln-shroudforge.dll") == "shroudforge", "owner plugin:kiln-shroudforge.dll");
    check(normalize_owner("emf:coolmod") == "emf:coolmod", "owner emf:coolmod");
    check(normalize_owner("native:fps-unlock") == "native:fps-unlock", "owner native:fps-unlock");
    check(normalize_owner("kiln") == "kiln", "owner kiln");
    check(normalize_owner("kiln-") == "kiln-", "owner kiln-");

    // a) foreign-before from an early chain entry: fix late
    json map = {{"targets", json::array({target("0x1000", "chat/ChatLogAdd", {"service:kiln-chat.dll", "plugin:kiln-emf.dll", "service:kiln-chat.dll"},
                                                dest("xhl-core.dll", "0x2a0"))})},
                {"foreign", json::array()}};
    std::vector<ChainRow> chain = {row("xhl-core.dll", "loaded")};
    json o = compute_overlaps(map, chain, {});
    check(o.size() == 1, "foreign-before: one overlap");
    if (o.size() == 1) {
        check(o[0]["kind"] == "foreign-before" && o[0]["symbol"] == "chat/ChatLogAdd" && o[0]["rva"] == "0x1000", "foreign-before: kind, symbol, rva");
        check(o[0]["foreign"]["module"] == "xhl-core.dll" && o[0]["foreign"]["offset"] == "0x2a0", "foreign-before: the foreign side");
        check(strings(o[0]["owners"]) == std::vector<std::string>({"chat", "emf"}), "foreign-before: owners are short and unique");
        check(o[0]["fix"] == "late" && o[0]["chainEntry"]["status"] == "loaded" && o[0]["chainEntry"]["late"] == false, "foreign-before: fix late, chain entry");
        const std::string msg = o[0]["message"].get<std::string>();
        check(msg.find("xhl-core.dll+0x2a0 hooked the function before Kiln did; Kiln's chat/ChatLogAdd (kiln-chat), "
                       "chat/ChatLogAdd (kiln-emf) chained over it.") != std::string::npos &&
                  msg.find("[late]") != std::string::npos,
              "foreign-before: the message names both sides (each hook with its owner, once) and the fix: " + msg);
    }
    // the same module already late: nothing to move
    o = compute_overlaps(map, {row("XHL-Core.dll", "loaded", true)}, {});
    check(o.size() == 1 && o[0]["fix"] == "none" && o[0]["chainEntry"]["late"] == true, "foreign-before from a late entry: fix none");
    // not a chain entry: a rule
    o = compute_overlaps(map, {row("other.dll", "loaded")}, {});
    check(o.size() == 1 && o[0]["fix"] == "rule" && o[0]["chainEntry"].is_null(), "foreign-before from a module the chain does not know: fix rule");
    // b) foreign-over, and both on one target; Kiln's own module is no overlap
    map = {{"targets", json::array({target("0x2000", "gameplay/InventoryTransfer", {"service:kiln-items.dll", "emf:coolmod"},
                                           dest("kiln-emf.dll", "0x10", true), dest("xhl-core.dll", "0x4410")),
                                    target("0x3000", nullptr, {"native:fps"}, dest("kiln-emf.dll", "0x20", true)),
                                    target("0x4000", "time/Tick", {}, dest("xhl-core.dll", "0x1"))})},
           {"foreign", json::array()}};
    o = compute_overlaps(map, chain, {});
    check(o.size() == 1, "foreign-over: own modules and link-less targets make no overlap (" + std::to_string(o.size()) + ")");
    if (o.size() == 1) {
        check(o[0]["kind"] == "foreign-over" && o[0]["fix"] == "none", "foreign-over: kind, fix none");
        check(strings(o[0]["owners"]) == std::vector<std::string>({"items", "emf:coolmod"}), "foreign-over: owners");
        const std::string msg = o[0]["message"].get<std::string>();
        check(msg.rfind("xhl-core.dll+0x4410 hooked over Kiln's gameplay/InventoryTransfer (kiln-items), "
                        "gameplay/InventoryTransfer (emf:coolmod). Kiln's detour stays armed", 0) == 0,
              "foreign-over: the message names each hook with its owner: " + msg);
    }
    map["targets"].push_back(target("0x5000", "a/B", {"service:kiln-a.dll"}, dest("one.dll", "0x1"), dest("two.dll", "0x2")));
    o = compute_overlaps(map, chain, {});
    check(o.size() == 3 && o[1]["kind"] == "foreign-before" && o[2]["kind"] == "foreign-over", "before and over on one target are two overlaps");
    // an unnamed jump target
    map = {{"targets", json::array({target("0x6000", "a/B", {"service:kiln-a.dll"}, {{"module", ""}, {"offset", "0x0"}, {"address", "0x1b2c3d4e0000"}, {"own", false}})})}};
    o = compute_overlaps(map, chain, {});
    check(o.size() == 1 && o[0]["fix"] == "rule" && o[0]["message"].get<std::string>().find("unnamed") != std::string::npos, "an unnamed foreign jump: fix rule");
    // no overlaps in a quiet map; a map without sections
    check(compute_overlaps(json::object(), chain, {}).empty(), "an empty document has no overlaps");
    check(compute_overlaps({{"targets", json::array({target("0x1", "a/B", {"service:kiln-a.dll"})})}}, chain, {}).empty(), "a clean target has none");

    // c) refused (ShroudForge)
    const char* sf = R"j({"mode":"hosted","runtime":"running","native":"types=3/3{reflected_ecs_types=3} game_thread=unavailable(game-thread:original-bytes-mismatch) entity_hooks{}"})j";
    const auto refused = parse_sf_status(sf);
    check(refused.size() == 1 && refused[0].hook == "game-thread" && refused[0].reason == "original-bytes-mismatch", "SF status: game_thread=unavailable(hook:reason)");
    check(sf_running(sf), "SF status: hosted counts as running");
    check(parse_sf_status(R"j({"mode":"hosted","native":"game_thread=ready(thread=5)"})j").empty(), "SF status: ready is no refusal");
    check(parse_sf_status(R"j({"mode":"hosted","native":"game_thread=unavailable"})j").empty(), "SF status: bare unavailable is no refusal");
    check(parse_sf_status("not json").empty() && !sf_running("not json") && !sf_running(R"j({"mode":"off"})j"), "SF status: garbage and off");
    const auto plain = parse_sf_status(R"j({"mode":"hosted","native":"game_thread=unavailable(signature-not-unique-or-missing)"})j");
    check(plain.size() == 1 && plain[0].hook == "game-thread" && plain[0].reason == "signature-not-unique-or-missing", "SF status: a reason without a hook name");

    map = {{"targets", json::array()},
           {"foreign", json::array({{{"symbol", "a/B"}, {"rva", "0x1"}, {"module", "xhl-core.dll"}, {"offset", "0x5"}, {"own", false}},
                                    {{"symbol", "c/D"}, {"rva", "0x2"}, {"module", "xhl-core.dll"}, {"offset", "0x9"}, {"own", false}},
                                    {{"symbol", "e/F"}, {"rva", "0x3"}, {"module", "kiln-emf.dll"}, {"offset", "0x9"}, {"own", true}}})}};
    o = compute_overlaps(map, chain, refused);
    check(o.size() == 1 && o[0]["kind"] == "refused", "refused: one overlap");
    if (o.size() == 1) {
        check(o[0]["symbol"].is_null() && o[0]["rva"].is_null() && strings(o[0]["owners"]) == std::vector<std::string>({"shroudforge"}), "refused: no symbol, owner shroudforge");
        check(o[0]["hook"] == "game-thread" && o[0]["reason"] == "original-bytes-mismatch", "refused: hook and reason");
        check(o[0]["foreign"]["module"] == "xhl-core.dll" && o[0]["fix"] == "late" && o[0]["chainEntry"]["status"] == "loaded", "refused: the only patching module is the suspect, fix late");
        check(strings(o[0]["suspects"]) == std::vector<std::string>({"xhl-core.dll"}), "refused: suspects");
    }
    map["foreign"].push_back({{"symbol", "g/H"}, {"rva", "0x4"}, {"module", "reshade64.dll"}, {"offset", "0x1"}, {"own", false}});
    o = compute_overlaps(map, chain, refused);
    check(o.size() == 1 && o[0]["foreign"]["module"].is_null() && o[0]["fix"] == "rule" && o[0]["suspects"].size() == 2, "refused with two suspects: module unknown, fix rule");
    o = compute_overlaps(json::object(), chain, refused);
    check(o.size() == 1 && o[0]["fix"] == "none" && o[0]["suspects"].empty() && o[0]["foreign"]["module"].is_null() &&
          o[0]["message"].get<std::string>().find("cannot tell") != std::string::npos, "refused with no suspect: fix none");
    o = compute_overlaps(json::object(), chain, plain);
    check(o.size() == 1 && o[0]["fix"] == "none", "refused for another reason: fix none");
    // deterministic: same input, same output
    check(compute_overlaps(map, chain, refused) == compute_overlaps(map, chain, refused), "compute_overlaps is deterministic");

    // GitHub issue 39: three sigdb symbols name ItemRegistryFind, and the map's label is the alphabetically first
    // (build/ItemFind), while the only hook on it is kiln-content's content/ItemRegistryFind. The line names that hook.
    map = {{"targets", json::array({{{"rva", "0xe94470"}, {"symbol", "build/ItemFind"}, {"label", "build/ItemFind"},
                                     {"links", json::array({{{"owner", "service:kiln-content.dll"}, {"kind", "raw"}, {"priority", 0},
                                                             {"label", "content/ItemRegistryFind"}}})},
                                     {"foreignBefore", dest("quick-deposit.dll", "0x16100")}, {"foreignOver", nullptr}}})},
           {"foreign", json::array()}};
    o = compute_overlaps(map, {}, {});
    check(o.size() == 1 && o[0]["symbol"] == "build/ItemFind" && strings(o[0]["owners"]) == std::vector<std::string>({"content"}),
          "issue 39: the row keeps the sigdb symbol and the short owners (rules and the launcher match on them)");
    if (o.size() == 1) {
        const std::string msg = o[0]["message"].get<std::string>();
        check(msg.rfind("quick-deposit.dll+0x16100 hooked the function before Kiln did; Kiln's content/ItemRegistryFind "
                        "(kiln-content) chained over it.", 0) == 0 && msg.find("build/ItemFind") == std::string::npos,
              "issue 39: the line names the hook and its owner, not the first sigdb symbol: " + msg);
    }
    // the owner as the lines show it (hook_owner.h)
    check(hook_owner_text("service:kiln-content.dll") == "kiln-content" && hook_owner_text("plugin:kiln-emf.DLL") == "kiln-emf" &&
              hook_owner_text("emf:coolmod") == "emf:coolmod" && hook_owner_text("native:fps-unlock") == "native:fps-unlock" &&
              hook_owner_text("kiln") == "kiln" && hook_owner_text("") == "",
          "hook_owner_text: service and plugin DLLs by name, other owners as they are");
    check(hook_link_text("content/ItemRegistryFind", "service:kiln-content.dll") == "content/ItemRegistryFind (kiln-content)" &&
              hook_link_text("a/B", "") == "a/B",
          "hook_link_text: <label> (<owner>), the label alone without an owner");
}

// The reports are built from rows the core wrote itself: `refused` has null symbol, rva, module and offset. Nothing in
// them may throw (nlohmann's value() does on a null), or hooks.json loses its compat sections.
void null_tolerance_cases() {
    const char* sf = R"j({"mode":"hosted","native":"game_thread=unavailable(game-thread:original-bytes-mismatch)"})j";
    // no single suspect: two modules patched game code, so the refused row has foreign.module null
    json map = {{"targets", json::array()},
                {"foreign", json::array({{{"symbol", "a/B"}, {"rva", "0x1"}, {"module", "xhl-core.dll"}, {"offset", "0x5"}, {"own", false}},
                                         {{"symbol", "g/H"}, {"rva", "0x4"}, {"module", "reshade64.dll"}, {"offset", "0x1"}, {"own", false}}})}};
    const json overlaps = compute_overlaps(map, {row("xhl-core.dll", "loaded")}, parse_sf_status(sf));
    check(overlaps.size() == 1 && overlaps[0]["kind"] == "refused" && overlaps[0]["foreign"]["module"].is_null() &&
          overlaps[0]["foreign"]["offset"].is_null() && overlaps[0]["symbol"].is_null() && overlaps[0]["rva"].is_null(),
          "refused with two suspects: every field the launcher parses is null");
    bool threw = false;
    std::string key;
    try {
        for (const auto& o : overlaps) key = overlap_key(o);
    } catch (const std::exception& e) {
        threw = true;
        std::printf("overlap_key threw: %s\n", e.what());
    }
    check(!threw && key == "refused|game-thread||", "overlap_key of a refused row without a culprit does not throw (" + key + ")");
    // the key of every other kind, and of rows with odd members
    check(overlap_key({{"kind", "foreign-before"}, {"symbol", "chat/ChatLogAdd"}, {"foreign", {{"module", "x.dll"}, {"offset", "0x2a0"}}}}) ==
              "foreign-before|chat/ChatLogAdd|x.dll|0x2a0", "overlap_key: foreign-before");
    check(overlap_key({{"kind", "foreign-over"}, {"symbol", nullptr}, {"hook", nullptr}, {"foreign", nullptr}}) == "foreign-over|||" &&
          overlap_key(json::object()) == "|||" && overlap_key(json(nullptr)) == "|||" &&
          overlap_key({{"kind", 5}, {"foreign", {{"module", 7}, {"offset", true}}}}) == "|||", "overlap_key: null and non-string members read as empty");
    check(json_text({{"a", nullptr}, {"b", 1}, {"c", "x"}}, "a").empty() && json_text({{"b", 1}}, "b").empty() && json_text({{"c", "x"}}, "c") == "x" &&
          json_text(json::array(), "c").empty() && !json_flag({{"own", nullptr}}, "own") && json_flag({{"own", true}}, "own"),
          "json_text and json_flag: only a string / a true boolean count");
    // other modules' files: a null member must not stop the report either
    check(!sf_running(R"j({"mode":null})j") && sf_running(R"j({"mode":"self-hosted"})j") && parse_sf_status(R"j({"mode":null,"native":null})j").empty(),
          "SF status: null members are read as absent");
    map = {{"targets", json::array({{{"rva", nullptr}, {"symbol", nullptr}, {"label", nullptr},
                                     {"links", json::array({{{"owner", nullptr}}, {{"owner", "service:kiln-a.dll"}}})},
                                     {"foreignBefore", {{"module", nullptr}, {"offset", nullptr}, {"own", nullptr}}}, {"foreignOver", nullptr}}})},
           {"foreign", json::array({json(nullptr), {{"module", nullptr}, {"own", nullptr}}})}};
    json odd;
    try {
        odd = compute_overlaps(map, {}, {});
    } catch (const std::exception& e) {
        std::printf("compute_overlaps threw: %s\n", e.what());
    }
    check(odd.is_array() && odd.size() == 1 && odd[0]["rva"].is_null() && odd[0]["foreign"]["offset"].is_null(),
          "compute_overlaps survives null label, rva, module, offset, own and owner");
}

// The status names and the late phase's final status of a chain entry: one helper for boot.json and hooks.json.
struct FakeLate {
    size_t chain_index;
    std::string status;
};
void chain_status_cases() {
    const char* names[] = {"loaded", "failed", "missing", "refused", "duplicate", "skipped", "late"};
    bool all = true;
    for (uint32_t i = 0; i < 7; ++i) all = all && std::string(chain_status_name(i)) == names[i];
    check(all && std::string(chain_status_name(7)) == "failed" && std::string(chain_status_name(99)) == "failed",
          "chain_status_name: the seven names, an unknown value is failed");
    KilnChainEntry plain{}, late{};
    plain.status = KILN_CHAIN_REFUSED;
    late.status = KILN_CHAIN_LATE;
    const std::vector<FakeLate> rows = {{1, "pending"}, {2, "failed"}, {3, kLateAlreadyLoaded}, {4, "skipped"}, {5, "loaded"}};
    check(chain_final_status(plain, 2, rows, "skipped") == "refused", "chain_final_status: a proxy status stays");
    check(chain_final_status(late, 0, rows, "skipped") == "skipped" && chain_final_status(late, 0, rows, "late") == "late",
          "chain_final_status: no row yet shows what the writer asks for (boot.json: skipped, hooks.json: late)");
    check(chain_final_status(late, 1, rows, "skipped") == "skipped" && chain_final_status(late, 1, rows, "late") == "late",
          "chain_final_status: a pending row is no answer");
    check(chain_final_status(late, 2, rows, "late") == "failed" && chain_final_status(late, 4, rows, "late") == "skipped" &&
          chain_final_status(late, 5, rows, "late") == "loaded", "chain_final_status: the late phase's status");
    check(chain_final_status(late, 3, rows, "late") == kLateAlreadyLoaded && chain_reported_status(kLateAlreadyLoaded) == "loaded" &&
          chain_reported_status("failed") == "failed", "an already loaded DLL is reported as loaded to readers of the six statuses");
}

ChainRow late_row(const char* file, const char* source = "chain") {
    ChainRow r = row(file, "late", true, source);
    return r;
}

// Rules and overlaps for the DLLs [late] cannot move (finding: EMF's loader, a DLL another loader started first).
void late_limit_cases() {
    ChainRow loader = row("emf-dinput8.dll", "loaded");
    loader.loader = true;
    ChainRow ignored = row("emf-x.dll", "loaded", true, "emf");
    ignored.late_ignored = true;
    ChainRow started = row("emf-y.dll", "loaded", false, "emf");
    started.started_by_loader = true;
    ChainRow plain = row("xhl-core.dll", "loaded");

    // overlaps: `late` is only recommended for a DLL Kiln's proxy starts and the core can defer
    json map = {{"targets", json::array({target("0x1000", "chat/ChatLogAdd", {"service:kiln-chat.dll"}, dest("emf-x.dll", "0x2a0"))})},
                {"foreign", json::array()}};
    json o = compute_overlaps(map, {ignored}, {});
    check(o.size() == 1 && o[0]["fix"] == "rule" && o[0]["chainEntry"]["status"] == "loaded" && o[0]["chainEntry"]["late"] == false &&
          o[0]["chainEntry"]["lateIgnored"] == true, "a late DLL another loader started: fix rule, chainEntry.late false, lateIgnored");
    std::string msg = o[0]["message"].get<std::string>();
    check(msg.find("already loads late") == std::string::npos && msg.find("already a late entry") == std::string::npos &&
          msg.find("another loader started it first") != std::string::npos, "that overlap never says it already loads late: " + msg);
    map["targets"][0]["foreignBefore"] = dest("emf-y.dll", "0x2a0");
    o = compute_overlaps(map, {started}, {});
    check(o.size() == 1 && o[0]["fix"] == "rule" && o[0]["message"].get<std::string>().find("started by EMF's loader") != std::string::npos,
          "a dinput8-chain.ini DLL while EMF's loader is chained: fix rule, not late");
    map["targets"][0]["foreignBefore"] = dest("emf-dinput8.dll", "0x2a0");
    o = compute_overlaps(map, {loader}, {});
    check(o.size() == 1 && o[0]["fix"] == "rule" && o[0]["message"].get<std::string>().find("starts EMF itself") != std::string::npos,
          "EMF's loader itself: fix rule, not late");
    map["targets"][0]["foreignBefore"] = dest("xhl-core.dll", "0x2a0");
    o = compute_overlaps(map, {plain}, {});
    check(o.size() == 1 && o[0]["fix"] == "late", "an ordinary chain entry still gets fix late");
    o = compute_overlaps(map, {late_row("xhl-core.dll")}, {});
    check(o.size() == 1 && o[0]["fix"] == "none" && o[0]["chainEntry"]["late"] == true && o[0]["chainEntry"].find("lateIgnored") == o[0]["chainEntry"].end(),
          "a DLL the core loaded late: fix none, late true");
    // refused (ShroudForge) with the culprit being one of those
    const char* sf = R"j({"mode":"hosted","native":"game_thread=unavailable(game-thread:original-bytes-mismatch)"})j";
    map = {{"targets", json::array()}, {"foreign", json::array({{{"symbol", "a/B"}, {"rva", "0x1"}, {"module", "emf-x.dll"}, {"offset", "0x5"}, {"own", false}}})}};
    o = compute_overlaps(map, {ignored}, parse_sf_status(sf));
    check(o.size() == 1 && o[0]["fix"] == "rule" && o[0]["message"].get<std::string>().find("already loads late") == std::string::npos,
          "refused, culprit started by another loader: fix rule, never \"already loads late\"");

    // rules
    auto late_rule = [](const char* subject) {
        json doc = {{"format", 1}, {"rules", json::array({{{"id", "r1"}, {"action", "late"}, {"subject", {{"module", subject}}}, {"message", "m"}}})}};
        return parse_rules(doc.dump(), "test").rules.at(0);
    };
    RuleContext ctx;
    Facts facts;
    Outcome oc = outcome_of(late_rule("xhl-core.dll"), {plain}, facts, ctx);
    check(!oc.applied && oc.note == "rule r1 asks xhl-core.dll to load late; apply it in the launcher", "late rule, ordinary entry: apply it in the launcher");
    oc = outcome_of(late_rule("xhl-core.dll"), {late_row("xhl-core.dll")}, facts, ctx);
    check(oc.applied && oc.note == "xhl-core.dll already loads late", "late rule, a late entry: already loads late");
    oc = outcome_of(late_rule("emf-x.dll"), {ignored}, facts, ctx);
    check(!oc.applied && oc.note.find("another loader started it first") != std::string::npos && oc.note.find("already loads late") == std::string::npos,
          "late rule, a DLL another loader started first: not applied, " + oc.note);
    oc = outcome_of(late_rule("emf-dinput8.dll"), {loader}, facts, ctx);
    check(!oc.applied && oc.note.find("EMF's loader") != std::string::npos && oc.note.find("launcher") == std::string::npos,
          "late rule on EMF's loader: not applied, no launcher advice: " + oc.note);
    oc = outcome_of(late_rule("emf-y.dll"), {started}, facts, ctx);
    check(!oc.applied && oc.note.find("started by EMF's loader") != std::string::npos, "late rule on a DLL EMF's loader starts: " + oc.note);
    oc = outcome_of(late_rule("nothing.dll"), {plain}, facts, ctx);
    check(!oc.applied && oc.note.find("no chain entry named nothing.dll") != std::string::npos, "late rule, no such entry");

    // disable of a mod the profile lists: applied when the DLL was not running, not when the proxy had loaded it
    json off = {{"format", 1}, {"rules", json::array({{{"id", "off1"}, {"action", "disable"}, {"subject", {{"mod", "native:fps"}}}, {"message", "clash"}}})}};
    const Rule disable = parse_rules(off.dump(), "test").rules.at(0);
    ctx.early.emplace_back("off1", "switched off in the profile before the plugins read it");
    ctx.rule_off.insert("native:fps");
    facts.mods = {"native:fps"};
    oc = outcome_of(disable, {}, facts, ctx);
    check(oc.applied && oc.note == "switched off in the profile before the plugins read it", "disable of a listed mod: applied");
    ctx.proxy_loaded.insert("off1");
    oc = outcome_of(disable, {}, facts, ctx);
    check(!oc.applied && oc.note.find("loaded by the proxy before kiln-core started") != std::string::npos &&
          oc.note.find("switched off") == std::string::npos && oc.note.find("activate the profile again") != std::string::npos,
          "disable of a mod whose DLL the proxy had loaded: not applied, no \"switched off\": " + oc.note);
}

// dev.29 `before` (a DLL that must load before another): read like every rule, and reported on the rows' order. The case
// of a player's report: Auto Loot Plus (a dllmain native, [profile]) must patch the game's imports before Ultimate ASI
// Loader ([chain], `..\xinput9_1_0.dll`) redirects them; the launcher loads UAL late and keeps ALP early.
void before_cases() {
    const json alp = {{"mod", "native:auto-loot-plus"}}, ual = {{"engine", "ultimate-asi-loader"}};
    const json doc = {{"format", 1}, {"rules", json::array({{{"id", "alp-first"}, {"when", json::array({alp, ual})}, {"action", "before"},
                                                            {"subject", alp}, {"target", ual}, {"message", "UAL loads after Kiln."}}})}};
    const ParseResult pr = parse_rules(doc.dump(), "shipped");
    check(pr.ok && pr.rules.size() == 1 && pr.problems.empty() && pr.dropped == 0, "before: read without a problem");
    if (pr.rules.size() != 1) return;
    const Rule& r = pr.rules[0];
    check(r.action == "before" && r.has_subject && r.has_target && r.target.kind == Matcher::Kind::Engine &&
              r.target.value == "ultimate-asi-loader" && r.subject.kind == Matcher::Kind::Mod, "before: its subject and target");
    Facts facts;
    facts.mods = {"kiln:auto-loot-plus"};
    facts.engines = {"kiln", "ultimate-asi-loader"};
    check(evaluate(pr.rules, facts).size() == 1, "before: fires on its when");

    ChainRow ual_row = row("xinput9_1_0.dll", "loaded");
    ual_row.path = "Z:\\game\\xinput9_1_0.dll";
    ual_row.engine = "ultimate-asi-loader";
    ChainRow alp_row = row("dinput8.dll", "loaded", false, "profile");
    alp_row.mod = "kiln:auto-loot-plus";
    ChainRow ual_late = ual_row, alp_late = alp_row;
    ual_late.late = alp_late.late = true;
    RuleContext ctx;
    // the launcher's plan: UAL late, ALP early
    Outcome oc = outcome_of(r, {ual_late, alp_row}, facts, ctx);
    check(oc.applied && oc.note == "dinput8.dll loaded before xinput9_1_0.dll", "before: UAL late, ALP early: applied: " + oc.note);
    // dev.28's starts: both early ([chain] first), both late (the same order), ALP late
    for (const auto& [rows, what] : {std::make_pair(std::vector<ChainRow>{ual_row, alp_row}, "both early"),
                                     std::make_pair(std::vector<ChainRow>{ual_late, alp_late}, "both late"),
                                     std::make_pair(std::vector<ChainRow>{ual_row, alp_late}, "ALP late")}) {
        oc = outcome_of(r, rows, facts, ctx);
        check(!oc.applied && oc.note.find("asks dinput8.dll to load before xinput9_1_0.dll") != std::string::npos &&
                  oc.note.find("activate the profile again") != std::string::npos, std::string("before: ") + what + ": not applied: " + oc.note);
    }
    // a late UAL that another loader had started first loaded early after all
    ChainRow ual_ignored = ual_late;
    ual_ignored.late_ignored = true;
    oc = outcome_of(r, {ual_ignored, alp_row}, facts, ctx);
    check(!oc.applied, "before: a [late] that had no effect does not count as late");
    // UAL is not a chain entry (Windows loaded it next to the game): nothing to check, said so
    oc = outcome_of(r, {alp_row}, facts, ctx);
    check(!oc.applied && oc.note.find("no chain entry of this start is engine ultimate-asi-loader") != std::string::npos,
          "before: no target row: " + oc.note);
    // a failed row is not loaded at all
    ChainRow alp_failed = alp_row;
    alp_failed.status = "failed";
    oc = outcome_of(r, {ual_late, alp_failed}, facts, ctx);
    check(!oc.applied && oc.note.find("mod native:auto-loot-plus") != std::string::npos, "before: a failed subject is no row: " + oc.note);
    // module matchers, within one phase: list order
    const Rule by_name = one_rule({{"id", "b2"}, {"action", "before"}, {"subject", {{"module", "first.dll"}}}, {"target", {{"module", "second*.dll"}}}});
    check(by_name.has_target, "before with module matchers: read");
    oc = outcome_of(by_name, {row("first.dll", "loaded"), row("second-a.dll", "loaded")}, facts, ctx);
    check(oc.applied && oc.note == "first.dll loaded before second-a.dll", "before: list order in one phase: " + oc.note);
    oc = outcome_of(by_name, {row("second-a.dll", "loaded"), row("first.dll", "loaded")}, facts, ctx);
    check(!oc.applied, "before: the wrong list order in one phase");
    // A requested late load is not evidence it happened; every matched DLL must have loaded.
    oc = outcome_of(by_name, {row("first.dll", "loaded"), row("second-a.dll", "late", true)}, facts, ctx);
    check(!oc.applied, "before: pending target is not loaded");
    oc = outcome_of(by_name, {row("first.dll", "loaded"), row("second-a.dll", "loaded"),
                              row("second-b.dll", "failed")}, facts, ctx);
    check(!oc.applied, "before: a failed wildcard target must not disappear");
    ChainRow duplicate = row("second-a.dll", "duplicate");
    oc = outcome_of(by_name, {row("first.dll", "loaded"), duplicate}, facts, ctx);
    check(!oc.applied, "before: a duplicate row does not establish its original load order");
    // the same rule's report line in hooks.json would carry its target: an older core drops the whole rule instead
    const Rule late_with_target = one_rule({{"id", "x"}, {"action", "late"}, {"subject", {{"module", "a.dll"}}}, {"target", ual}});
    check(late_with_target.id.empty(), "a target belongs to before only");
}

// netjunk's report (2026-11-20): two Library DLLs named dinput8.dll (a ShroudLink player addon and Building Tools). The
// lines name each module's file too, and the chain entry is the one at that path, not the first of that name.
void module_place_cases() {
    const std::string game = "D:\\SteamLibrary\\steamapps\\common\\Enshrouded";
    const std::string lib = "C:\\Users\\p\\AppData\\Local\\Kiln\\library\\native\\building-tools\\0.0.0+26bc47c7\\dinput8.dll";
    check(module_path_text(game + "\\kiln\\chain\\xhl-core\\xhl-core.dll", game) == "kiln/chain/xhl-core/xhl-core.dll",
          "path text: a module under the game folder is relative, with '/'");
    check(module_path_text("\\\\?\\d:\\steamlibrary\\STEAMAPPS\\common\\enshrouded\\mods\\creative\\creative.dll", game + "\\") ==
              "mods/creative/creative.dll",
          "path text: case, a \\\\?\\ prefix and a trailing separator do not matter");
    check(module_path_text(game + "\\dinput8.dll", game) == "dinput8.dll", "path text: a module in the game folder itself");
    check(module_path_text(lib, game) == lib, "path text: a Library DLL keeps its full path (its folder names the mod)");
    check(module_path_text(game + "x\\a.dll", game) == game + "x\\a.dll", "path text: a sibling folder with a longer name is not inside");
    check(module_path_text("", game).empty() && module_path_text(lib, "") == lib, "path text: empty path, unknown game folder");
    check(module_place_suffix("kiln/chain/xhl-core/xhl-core.dll") == " [kiln/chain/xhl-core/xhl-core.dll]" &&
              module_place_suffix("dinput8.dll") == " [game folder]" && module_place_suffix(lib) == " [" + lib + "]" &&
              module_place_suffix("").empty(),
          "place suffix: relative path, game folder, full path, nothing");
    check(module_path_matches(game + "\\kiln\\chain\\xhl-core\\xhl-core.dll", "kiln/chain/xhl-core/xhl-core.dll") &&
              module_path_matches("\\\\?\\" + lib, lib) &&
              !module_path_matches(game + "\\kiln\\chain\\myxhl-core\\xhl-core.dll", "xhl-core/xhl-core.dll") && !module_path_matches(lib, ""),
          "path match: a full path, a game-relative tail on a separator, never a partial folder name");

    const std::string addon = "C:\\Users\\p\\AppData\\Local\\Kiln\\library\\native\\player-addon\\0.0.0+9b0759ea\\dinput8.dll";
    ChainRow a = row("dinput8.dll", "loaded", false, "profile");
    a.path = addon;
    ChainRow b = row("dinput8.dll", "late", true, "profile");
    b.path = lib;
    json before = dest("dinput8.dll", "0x54230");
    before["path"] = addon;
    json over = dest("dinput8.dll", "0x3e080");
    over["path"] = lib;
    json map = {{"targets", json::array({target("0x1000", "ui/ItemSlot", {"service:kiln-ui-values.dll"}, before),
                                         target("0x2000", "gameplay/PlaceBlueprint", {"service:kiln-gameplay.dll"}, nullptr, over)})},
                {"foreign", json::array()}};
    // Building Tools listed first: a file-name match alone would take its late row for the addon's hook
    json o = compute_overlaps(map, {b, a}, {});
    check(o.size() == 2, "two dinput8.dll overlaps (" + std::to_string(o.size()) + ")");
    if (o.size() == 2) {
        const std::string m0 = o[0]["message"].get<std::string>(), m1 = o[1]["message"].get<std::string>();
        check(m0.rfind("dinput8.dll+0x54230 [" + addon + "] hooked the function before Kiln did", 0) == 0, "foreign-before names the file: " + m0);
        check(m1.rfind("dinput8.dll+0x3e080 [" + lib + "] hooked over Kiln's gameplay/PlaceBlueprint", 0) == 0, "foreign-over names the file: " + m1);
        check(o[0]["foreign"]["path"] == addon && o[1]["foreign"]["path"] == lib, "the overlap's foreign side carries the path");
        check(o[0]["chainEntry"].is_object() && o[0]["chainEntry"]["path"] == addon && o[0]["fix"] == "late",
              "the chain entry is the one at that path: " + o[0]["chainEntry"].dump());
    }
    // without a path (an older hooks map) nothing changes: the file name alone, the first row of that name that is on
    o = compute_overlaps({{"targets", json::array({target("0x1000", "ui/ItemSlot", {"service:kiln-ui-values.dll"}, dest("dinput8.dll", "0x54230"))})}},
                         {b, a}, {});
    check(o.size() == 1 && o[0]["message"].get<std::string>().rfind("dinput8.dll+0x54230 hooked the function", 0) == 0 && !o[0]["foreign"].contains("path"),
          "no path: the line as before");
}

// dev.30.3 `known` (sidelia's server, 21 Nov 2026): Live Map, IPHLPAPI.DLL in the game folder, hooks
// players/RegisterSingleton before Kiln. The shipped rule names it; its overlaps say so and are info, others stay as they were.
void known_cases() {
    json doc = {{"format", 1}, {"rules", json::array({
        {{"id", "livemap-known-hooks"}, {"when", json::array({{{"engine", "livemap"}}})}, {"action", "known"},
         {"subject", {{"module", "iphlpapi.dll"}}}, {"message", "Live Map hooks a few game functions before Kiln."}},
        {{"id", "bad-known"}, {"action", "known"}, {"subject", {{"mod", "native:x"}}}}})}};
    const ParseResult pr = parse_rules(doc.dump(), "test");
    check(pr.ok && pr.rules.size() == 1 && pr.dropped == 1 && pr.problems.size() == 1 &&
              pr.problems[0].find("known needs a module subject") != std::string::npos,
          "known: a module subject parses, a mod subject is dropped");
    if (pr.rules.size() != 1) return;
    const Rule& rule = pr.rules[0];
    Facts facts;
    facts.modules = {"IPHLPAPI.DLL"};
    facts.engines = {"kiln"};
    check(evaluate(pr.rules, facts).empty(), "known: the DLL name alone is not Live Map");
    facts.engines.push_back("livemap");
    const auto fired = evaluate(pr.rules, facts);
    check(fired.size() == 1, "known: fires with the livemap engine");
    RuleContext ctx;
    const Outcome oc = outcome_of(rule, {}, facts, ctx);
    check(oc.applied && oc.note.find("known") != std::string::npos, "known: applied, " + oc.note);

    json lm = dest("IPHLPAPI.DLL", "0x171d0");
    lm["path"] = "IPHLPAPI.DLL";
    json map = {{"targets", json::array({target("0x1000", "players/RegisterSingleton", {"kiln"}, lm),
                                         target("0x2000", "time.daytime_update", {"service:kiln-time.dll"}, nullptr, dest("iphlpapi.dll", "0x9a0")),
                                         target("0x3000", "chat/ChatLogAdd", {"service:kiln-chat.dll"}, dest("xhl-core.dll", "0x2a0"))})}};
    json o = compute_overlaps(map, {row("xhl-core.dll", "loaded")}, {});
    check(o.size() == 3, "known: three overlaps");
    if (o.size() != 3) return;
    const std::string before0 = o[0]["message"].get<std::string>(), xhl = o[2]["message"].get<std::string>();
    check(before0.find("Add a rule to compat-rules.json") != std::string::npos, "known: unmarked, the advice is there: " + before0);
    check(mark_known(o, fired) == 2, "known: the two Live Map overlaps are marked");
    const std::string m0 = o[0]["message"].get<std::string>(), m1 = o[1]["message"].get<std::string>();
    check(o[0]["known"] == "livemap-known-hooks" && o[0]["fix"] == "none" &&
              m0.rfind("IPHLPAPI.DLL+0x171d0 [game folder] hooked the function before Kiln did; Kiln's ", 0) == 0 &&
              m0.find("chained over it. Known: Live Map hooks a few game functions before Kiln. (compat rule livemap-known-hooks)") != std::string::npos &&
              m0.find("Add a rule") == std::string::npos,
          "known: foreign-before says what happened and that it is known: " + m0);
    check(o[1]["known"] == "livemap-known-hooks" && m1.find("stays armed behind their jump. Known: Live Map") != std::string::npos,
          "known: foreign-over, the same: " + m1);
    check(!o[2].contains("known") && o[2]["fix"] == "late" && o[2]["message"] == xhl, "known: another module's overlap is untouched");
    json none = o;
    check(mark_known(none, {}) == 0, "known: no fired rule, nothing marked");

    check(contains_text("..\\LiveMap.INI..", "livemap.ini"), "contains_text: 8-bit, case ignored");
    const std::string wide("\\\0l\0i\0v\0e\0m\0a\0p\0.\0i\0n\0i\0", 24);
    check(contains_text(wide, "livemap.ini"), "contains_text: UTF-16LE");
    check(!contains_text("livemap.in", "livemap.ini") && !contains_text("", "livemap.ini"), "contains_text: no partial match");
}

}  // namespace

int main(int argc, char** argv) {
    const std::string dir = argc > 1 ? argv[1] : "schema/fixtures";
    rule_cases(dir);
    overlap_cases();
    null_tolerance_cases();
    chain_status_cases();
    late_limit_cases();
    before_cases();
    module_place_cases();
    known_cases();
    std::printf("compat_test: %d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
