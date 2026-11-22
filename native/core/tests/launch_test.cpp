// SPDX-License-Identifier: MIT
// kiln.launch (src/launch.cpp, as it is): the pure validator's decision table, the idle class, the interim hold of
// kiln-sfi.interpreter, and the frozen record's members (status, notice_copy) including readers racing the freeze.
// Built by test-launch.sh with ASan+UBSan, and again with TSan (KILN_TEST_TSAN), on the host: no Windows, no game.
#include <atomic>
#include <cstddef>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "launch.h"
#include "nlohmann/json.hpp"
#include "services/boot_gamedata.h"  // the core's SHA-256
#include "services/kiln_profile.h"

using nlohmann::json;
namespace launch = kiln::launch;

namespace {
int checks = 0, failures = 0;
void check(bool ok, const std::string& what) {
    ++checks;
    if (!ok) {
        ++failures;
        std::fprintf(stderr, "FAIL %s\n", what.c_str());
    }
}

const std::string kExe = std::string(64, 'e');
const std::string kHash = std::string(64, 'a');
const std::string kPlanId = "0f0e0d0c-0b0a-4908-8706-050403020100";

std::string hex(std::string_view bytes) { return kiln::boot_gd::hex_of(kiln::boot_gd::sha256(bytes)); }

json package(const std::string& id, const std::string& decision, bool requires180, int bridge) {
    return {{"id", id},
            {"version", "1.0.0"},
            {"kind", "shroudforge"},
            {"root", "mods/" + id},
            {"content_sha256", kHash},
            {"config_sha256", kHash},
            {"enabled", decision != "disabled"},
            {"targets", {"client"}},
            {"capabilities", json::array()},
            {"dependencies", json::array()},
            {"requiredApis", json::array()},
            {"services", json::array()},
            {"requires180", requires180},
            {"saveSensitive", false},
            {"decision", decision},
            {"reasons", decision == "skip" ? json::array({{{"code", "x"}, {"message", "y"}, {"causedBy", json::array()},
                                                           {"severity", "warning"}}})
                                           : json::array()},
            {"data", {{"required", false}, {"admission", "not-needed"}}},
            {"lua", {{"required", false}, {"admission", "not-needed"}}},
            {"dlls", json::array({{{"path", id + ".dll"}, {"sha256", kHash}, {"mode", "library"}, {"source", "ini"},
                                   {"required", true}, {"gate", decision == "candidate" ? "pass" : "unknown"},
                                   {"gateProof_sha256", kHash}, {"bridgeAbi", bridge}}})}};
}

// A plan kiln-manage could have written for this executable, client side, selecting kiln.
json plan() {
    json fingerprints = json::object();
    for (const char* k : {"packages_sha256", "settings_sha256", "chain_sha256", "loaderInventory_sha256", "compat_sha256",
                          "releasePolicy_sha256", "placementReceipts_sha256"})
        fingerprints[k] = kHash;
    return {{"format", 1},
            {"minReader", 1},
            {"plan_id", kPlanId},
            {"launch_id", nullptr},
            {"startupDialect", "none"},
            {"writer", {{"component", "kiln-manage"}, {"version", "0.2.0-dev.30"}}},
            {"profile", {{"id", "p1"}, {"generation", 3}, {"mode", "active"}, {"semantic_sha256", kHash}}},
            {"game", {{"side", "client"}, {"exe_sha256", kExe}, {"archiveBase_sha256", kHash}, {"archiveInput_sha256", kHash}}},
            {"backend", {{"requested", "kiln"}, {"selected", "kiln"}, {"fallbackOrder", json::array()},
                         {"dataWriter", "kiln-eml"}, {"selectionReason", "the player chose kiln"}}},
            {"contract", {{"apiVersion", "1.7.9"}, {"sourceCommit", std::string(40, 'c')}, {"upstream180Final", false},
                          {"inventory_sha256", kHash}, {"parity_sha256", kHash}, {"serviceContracts_sha256", kHash}}},
            {"fingerprints", fingerprints},
            {"packages", json::array({package("alpha", "candidate", false, 0), package("beta", "skip", true, 2)})},
            {"loadOrder", json::array({"alpha"})},
            {"warnings", json::array()}};
}

json profile_for(std::string_view bytes, const std::string& name = kPlanId + ".json") {
    return {{"format", 1}, {"id", "p1"}, {"generation", 3}, {"mods", json::object()},
            {"sfLaunchPlan", {{"path", "launch-plans/" + name}, {"sha256", hex(bytes)}}}};
}

// One run of decide(): kiln.json, the plan's bytes as the reader returns them, the profile that names them.
struct Run {
    std::string kiln_json = R"({"shroudforge": {"backend": "kiln"}})";
    bool has_kiln_json = true;
    std::string env;
    bool has_env = false;
    std::string plan_bytes = plan().dump();
    std::string dbghelp;
    launch::PlanFile::State plan_state = launch::PlanFile::Read;
    std::string profile;  // empty: profile_for(plan_bytes)
    bool no_profile = false;
    bool held = false;
    bool allows_180 = false;
    uint32_t side = KILN_SIDE_CLIENT;
    std::string exe = kExe;
    std::string asked;  // what the reader was asked for
    int reads = 0;
    launch::Decision go() {
        launch::Inputs in;
        if (has_kiln_json) in.kiln_json = &kiln_json;
        if (has_env) in.env_runtime = &env;
        if (profile.empty() && !no_profile) profile = profile_for(plan_bytes).dump();
        in.profile_text = profile;
        in.side = side;
        in.exe_sha256 = exe;
        in.interpreter_held = held;
        in.held_text = held ? "game test pending: sfi-ui-page" : "";
        in.release_allows_180 = allows_180;
        in.eml_r_present = [this] { return launch::eml_r_proxy(dbghelp); };
        in.read_plan = [this](std::string_view name) {
            ++reads;
            asked = std::string(name);
            launch::PlanFile f;
            f.state = plan_state;
            if (plan_state == launch::PlanFile::Read) f.bytes = plan_bytes;
            else f.error = plan_state == launch::PlanFile::Missing ? "the file is not there" : "access denied";
            return f;
        };
        return launch::decide(in);
    }
};

bool has(const std::string& text, std::string_view part) { return text.find(part) != std::string::npos; }

constexpr uint32_t kV1Size = KILN_LAUNCH_STATUS_V1_SIZE;  // an object to compare bytes with

void expect(const launch::Decision& d, uint32_t backend, uint32_t code, const std::string& what) {
    check(d.backend == backend && d.reason_code == code,
          what + " (got " + launch::backend_name(d.backend) + ", 0x" + [&] {
              char b[16];
              std::snprintf(b, sizeof b, "%x", d.reason_code);
              return std::string(b);
          }() + ": " + d.reason + ")");
    check(!d.reason.empty(), what + ": a reason text");
}

void settings() {
    {
        Run r;
        r.has_kiln_json = false;
        auto d = r.go();
        expect(d, KILN_LAUNCH_BACKEND_HOSTED, KILN_LAUNCH_REASON_HOSTED_DEFAULT, "no kiln.json: hosted, the default");
        check(r.reads == 0 && d.notice.empty() && !d.plan_read, "hosted reads no plan and has no notice");
    }
    for (const char* text : {"{}", R"({"shroudforge": {}})", R"({"shroudforge": 3})", "not json"}) {
        Run r;
        r.kiln_json = text;
        expect(r.go(), KILN_LAUNCH_BACKEND_HOSTED, KILN_LAUNCH_REASON_HOSTED_DEFAULT,
               std::string("kiln.json without a usable shroudforge object (") + text + "): hosted");
    }
    {
        Run r;
        r.kiln_json = R"({"shroudforge": {"backend": "hosted"}})";
        expect(r.go(), KILN_LAUNCH_BACKEND_HOSTED, KILN_LAUNCH_REASON_SETTING, "backend hosted");
        check(r.reads == 0, "hosted: no plan read");
    }
    {
        Run r;
        r.kiln_json = R"({"shroudforge": {"backend": "installed"}})";
        auto d = r.go();
        expect(d, KILN_LAUNCH_BACKEND_INSTALLED, KILN_LAUNCH_REASON_SETTING, "backend installed");
        check(r.reads == 0 && d.notice.empty(), "installed: no plan read, no notice");
    }
    {
        Run r;
        r.kiln_json = R"({"shroudforge": {"useInstalled": true}})";
        expect(r.go(), KILN_LAUNCH_BACKEND_INSTALLED, KILN_LAUNCH_REASON_USE_INSTALLED, "legacy useInstalled: installed");
    }
    {
        Run r;
        r.kiln_json = R"({"shroudforge": {"useInstalled": "yes"}})";
        expect(r.go(), KILN_LAUNCH_BACKEND_HOSTED, KILN_LAUNCH_REASON_HOSTED_DEFAULT,
               "a useInstalled that is not true/false counts as unset, as kiln-shroudforge reads it");
    }
    {
        Run r;
        r.kiln_json = R"({"shroudforge": {"backend": "hosted", "useInstalled": true}})";
        expect(r.go(), KILN_LAUNCH_BACKEND_HOSTED, KILN_LAUNCH_REASON_SETTING, "both keys: the new key wins (hosted)");
    }
    {
        Run r;
        r.kiln_json = R"({"shroudforge": {"backend": "kiln", "useInstalled": true}})";
        expect(r.go(), KILN_LAUNCH_BACKEND_KILN, KILN_LAUNCH_REASON_PLAN, "both keys: the new key wins (kiln)");
    }
    for (const char* text : {R"({"shroudforge": {"backend": "Kiln"}})", R"({"shroudforge": {"backend": "bundled"}})",
                             R"({"shroudforge": {"backend": 3}})", R"({"shroudforge": {"backend": null}})",
                             R"({"shroudforge": {"backend": ""}})"}) {
        Run r;
        r.kiln_json = text;
        auto d = r.go();
        expect(d, KILN_LAUNCH_BACKEND_NONE, KILN_LAUNCH_REASON_SETTING_INVALID, std::string("refused: ") + text);
        check(!d.notice.empty() && has(d.notice, d.reason), "a refusal's notice carries its reason");
        check(r.reads == 0, "a refused setting reads no plan");
    }
    // KILN_SHROUDFORGE_RUNTIME, as kiln-shroudforge reads it (plugin.cpp prefer_installed): "installed" -> installed,
    // anything else -> hosted, over kiln.json; never for kiln.
    {
        Run r;
        r.has_kiln_json = false;
        r.env = "installed", r.has_env = true;
        expect(r.go(), KILN_LAUNCH_BACKEND_INSTALLED, KILN_LAUNCH_REASON_ENV_RUNTIME, "env installed over the default");
    }
    {
        Run r;
        r.kiln_json = R"({"shroudforge": {"useInstalled": true}})";
        r.env = "bundled", r.has_env = true;
        expect(r.go(), KILN_LAUNCH_BACKEND_HOSTED, KILN_LAUNCH_REASON_ENV_RUNTIME, "env bundled over useInstalled");
    }
    {
        Run r;
        r.kiln_json = R"({"shroudforge": {"backend": "installed"}})";
        r.env = "whatever", r.has_env = true;
        expect(r.go(), KILN_LAUNCH_BACKEND_HOSTED, KILN_LAUNCH_REASON_ENV_RUNTIME,
               "env with any other value means bundled (hosted), as in kiln-shroudforge");
    }
    {
        Run r;
        r.env = "installed", r.has_env = true;
        expect(r.go(), KILN_LAUNCH_BACKEND_KILN, KILN_LAUNCH_REASON_PLAN, "env never overrides kiln");
    }
    {
        Run r;
        r.kiln_json = R"({"shroudforge": {"backend": "nope"}})";
        r.env = "installed", r.has_env = true;
        expect(r.go(), KILN_LAUNCH_BACKEND_NONE, KILN_LAUNCH_REASON_SETTING_INVALID, "env never rescues a refused setting");
    }
}

void kiln_plans() {
    {
        Run r;
        auto d = r.go();
        expect(d, KILN_LAUNCH_BACKEND_KILN, KILN_LAUNCH_REASON_PLAN, "a valid plan: kiln");
        check(r.asked == kPlanId + ".json" && r.reads == 1, "the reader is asked once, for <plan id>.json");
        check(d.plan_read && kiln::boot_gd::hex_of(d.plan_hash) == hex(r.plan_bytes), "the record carries the plan's SHA-256");
        check(d.plan_id == kPlanId, "the record carries the plan id");
        check(!d.notice.empty(), "kiln has a notice");
    }
    {
        Run r;
        // Minimal PE32+ DLL, followed by the same broad marker ShroudForge's loader carries.
        constexpr size_t pe = 0x80;
        r.dbghelp.assign(pe + 24 + 112, '\0');
        r.dbghelp.replace(0, 2, "MZ");
        r.dbghelp[0x3C] = static_cast<char>(pe);
        r.dbghelp.replace(pe, 2, "PE");
        r.dbghelp[pe + 4] = '\x64';
        r.dbghelp[pe + 5] = '\x86';
        r.dbghelp[pe + 20] = 112;
        r.dbghelp[pe + 23] = '\x20';
        r.dbghelp[pe + 24] = '\x0B';
        r.dbghelp[pe + 25] = '\x02';
        r.dbghelp += "shroudforge";
        check(!launch::eml_r_proxy(r.dbghelp), "ShroudForge text alone is not an EML-R marker");
        check(!launch::eml_r_proxy("mods/eml-r.json"), "an EML-R string outside a PE is not a proxy");
        const std::string stock = r.dbghelp;
        r.dbghelp += "MODS/EML-R.JSON";
        check(launch::eml_r_proxy(r.dbghelp), "ASCII EML-R config marker");
        r.dbghelp = stock;
        for (char c : std::string_view("EML_R.DLL")) {
            r.dbghelp.push_back(c);
            r.dbghelp.push_back('\0');
        }
        Run hosted = r;
        hosted.kiln_json = R"({"shroudforge": {"backend": "hosted"}})";
        expect(hosted.go(), KILN_LAUNCH_BACKEND_HOSTED, KILN_LAUNCH_REASON_SETTING, "EML-R does not block hosted");
        const auto d = r.go();
        expect(d, KILN_LAUNCH_BACKEND_NONE, KILN_LAUNCH_REASON_EML_R_LOADER, "EML-R blocks a pinned interpreter plan");
        check(r.reads == 0 && !d.plan_read, "EML-R stands the interpreter aside before its launch plan is read");
        check(has(d.notice, "It may run these mods itself"), "EML-R explains why the interpreter stands aside");
    }
    {
        Run r;
        r.held = true;
        auto d = r.go();
        expect(d, KILN_LAUNCH_BACKEND_NONE, KILN_LAUNCH_REASON_INTERPRETER_HELD, "held row on: kiln refused");
        check(has(d.reason, "sfi-ui-page") && has(d.reason, "kiln-sfi.interpreter"), "the held text names the row and its checks");
        check(r.reads == 0, "held: no plan read");
    }
    {
        Run r;
        r.no_profile = true;
        expect(r.go(), KILN_LAUNCH_BACKEND_NONE, KILN_LAUNCH_REASON_PLAN_MISSING, "no profile (legacy start): plan missing");
    }
    {
        Run r;
        r.profile = R"({"format": 1, "mods": {}})";
        expect(r.go(), KILN_LAUNCH_BACKEND_NONE, KILN_LAUNCH_REASON_PLAN_MISSING, "profile without sfLaunchPlan: plan missing");
    }
    {
        Run r;
        r.plan_state = launch::PlanFile::Missing;
        auto d = r.go();
        expect(d, KILN_LAUNCH_BACKEND_NONE, KILN_LAUNCH_REASON_PLAN_MISSING, "named plan not there: plan missing");
        check(!d.plan_read, "nothing hashed when nothing was read");
    }
    {
        Run r;
        r.plan_state = launch::PlanFile::Unreadable;
        expect(r.go(), KILN_LAUNCH_BACKEND_NONE, KILN_LAUNCH_REASON_PLAN_INVALID, "named plan unreadable: invalid");
    }
    // the reference itself
    const std::string bytes = plan().dump();
    for (const json& ref : {json{{"path", "launch-plans/../" + kPlanId + ".json"}, {"sha256", hex(bytes)}},
                            json{{"path", "launch-plans/" + kPlanId + ".JSON"}, {"sha256", hex(bytes)}},
                            json{{"path", "launch-plans\\" + kPlanId + ".json"}, {"sha256", hex(bytes)}},
                            json{{"path", "/launch-plans/" + kPlanId + ".json"}, {"sha256", hex(bytes)}},
                            json{{"path", "launch-plans/plan.json"}, {"sha256", hex(bytes)}},
                            json{{"path", "launch-plans/" + kPlanId + ".json"}, {"sha256", std::string(64, 'A')}},
                            json{{"path", "launch-plans/" + kPlanId + ".json"}},
                            json{{"path", "launch-plans/" + kPlanId + ".json"}, {"sha256", hex(bytes)}, {"x", 1}},
                            json("launch-plans/" + kPlanId + ".json")}) {
        Run r;
        r.profile = json{{"format", 1}, {"sfLaunchPlan", ref}}.dump();
        expect(r.go(), KILN_LAUNCH_BACKEND_NONE, KILN_LAUNCH_REASON_PLAN_INVALID, "bad reference refused: " + ref.dump());
        check(r.reads == 0, "a bad reference reads nothing: " + ref.dump());
    }
    {
        Run r;
        r.profile = profile_for(r.plan_bytes).dump();
        r.plan_bytes += " ";  // changed after kiln-manage hashed it
        auto d = r.go();
        expect(d, KILN_LAUNCH_BACKEND_NONE, KILN_LAUNCH_REASON_PLAN_INVALID, "hash mismatch: invalid");
        check(d.plan_read && kiln::boot_gd::hex_of(d.plan_hash) == hex(r.plan_bytes), "a refused plan's hash is still recorded");
    }
    // malformed
    for (const std::string& text : {std::string("{"), std::string(""), std::string("[]"), std::string("null"),
                                    std::string(R"({"format": 1, "format": 1})"), plan().dump() + "x",
                                    std::string(70, '[') + std::string(70, ']')}) {
        Run r;
        r.plan_bytes = text;
        expect(r.go(), KILN_LAUNCH_BACKEND_NONE, KILN_LAUNCH_REASON_PLAN_INVALID, "malformed plan: " + text.substr(0, 40));
    }
    {
        Run r;  // a duplicate key deep inside, where a plain parser keeps the last one
        std::string text = plan().dump();
        text.replace(text.find("\"selected\":\"kiln\""), 17, "\"selected\":\"kiln\",\"selected\":\"hosted\"");
        r.plan_bytes = text;
        expect(r.go(), KILN_LAUNCH_BACKEND_NONE, KILN_LAUNCH_REASON_PLAN_INVALID, "duplicate nested key: invalid");
    }
    {
        Run r;
        r.plan_bytes = std::string(launch::kPlanMax + 1, ' ');
        expect(r.go(), KILN_LAUNCH_BACKEND_NONE, KILN_LAUNCH_REASON_PLAN_INVALID, "a plan over 8 MiB: invalid");
    }
    // structure
    auto mutated = [](auto edit) {
        json p = plan();
        edit(p);
        return p.dump();
    };
    struct Edit {
        const char* what;
        std::function<void(json&)> edit;
        uint32_t code;
    };
    const std::vector<Edit> edits = {
        {"format 2", [](json& p) { p["format"] = 2; }, KILN_LAUNCH_REASON_PLAN_INVALID},
        {"format 1.0", [](json& p) { p["format"] = 1.0; }, KILN_LAUNCH_REASON_PLAN_INVALID},
        {"minReader 2", [](json& p) { p["minReader"] = 2; }, KILN_LAUNCH_REASON_PLAN_INVALID},
        {"a missing key", [](json& p) { p.erase("warnings"); }, KILN_LAUNCH_REASON_PLAN_INVALID},
        {"an unknown key", [](json& p) { p["extra"] = 1; }, KILN_LAUNCH_REASON_PLAN_INVALID},
        {"plan id not the file's name", [](json& p) { p["plan_id"] = "1f0e0d0c-0b0a-4908-8706-050403020100"; },
         KILN_LAUNCH_REASON_PLAN_INVALID},
        {"plan id upper case", [](json& p) { p["plan_id"] = "0F0E0D0C-0B0A-4908-8706-050403020100"; },
         KILN_LAUNCH_REASON_PLAN_INVALID},
        {"a stamped launch id", [](json& p) { p["launch_id"] = "6b2f1c9e-0000-4000-8000-000000000001"; },
         KILN_LAUNCH_REASON_PLAN_STAMPED},
        {"launch id 0", [](json& p) { p["launch_id"] = 0; }, KILN_LAUNCH_REASON_PLAN_STAMPED},
        {"another writer", [](json& p) { p["writer"]["component"] = "my-tool"; }, KILN_LAUNCH_REASON_PLAN_INVALID},
        {"writer with an extra key", [](json& p) { p["writer"]["x"] = 1; }, KILN_LAUNCH_REASON_PLAN_INVALID},
        {"an unknown dialect", [](json& p) { p["startupDialect"] = "lua"; }, KILN_LAUNCH_REASON_PLAN_INVALID},
        {"the server's plan", [](json& p) { p["game"]["side"] = "server"; }, KILN_LAUNCH_REASON_PLAN_OTHER_GAME},
        {"another executable", [](json& p) { p["game"]["exe_sha256"] = std::string(64, 'f'); },
         KILN_LAUNCH_REASON_PLAN_OTHER_GAME},
        {"an exe hash that is not a hash", [](json& p) { p["game"]["exe_sha256"] = "E"; }, KILN_LAUNCH_REASON_PLAN_INVALID},
        // game.archiveBaseOrigin is one of the inspector's three tokens when present, never "vanilla"
        {"a vanilla archive base", [](json& p) { p["game"]["archiveBaseOrigin"] = "vanilla"; }, KILN_LAUNCH_REASON_PLAN_INVALID},
        {"an unknown archive base origin", [](json& p) { p["game"]["archiveBaseOrigin"] = "first-managed"; },
         KILN_LAUNCH_REASON_PLAN_INVALID},
        {"an archive base origin that is not text", [](json& p) { p["game"]["archiveBaseOrigin"] = 1; },
         KILN_LAUNCH_REASON_PLAN_INVALID},
        {"another unknown game key", [](json& p) { p["game"]["archiveBaseLabel"] = "unrecorded"; },
         KILN_LAUNCH_REASON_PLAN_INVALID},
        {"a plan selecting hosted",
         [](json& p) {
             p["backend"]["selected"] = "hosted";
             p["backend"]["requested"] = "hosted";
             p["backend"]["dataWriter"] = "shroudforge";
         },
         KILN_LAUNCH_REASON_PLAN_INVALID},
        {"kiln requested, none selected",
         [](json& p) {
             p["backend"]["selected"] = "none";
             p["backend"]["dataWriter"] = "none";
         },
         KILN_LAUNCH_REASON_PLAN_INVALID},
        {"kiln with another data writer", [](json& p) { p["backend"]["dataWriter"] = "shroudforge"; },
         KILN_LAUNCH_REASON_PLAN_INVALID},
        {"contract without a boolean upstream180Final", [](json& p) { p["contract"]["upstream180Final"] = "no"; },
         KILN_LAUNCH_REASON_PLAN_INVALID},
        {"packages not an array", [](json& p) { p["packages"] = json::object(); }, KILN_LAUNCH_REASON_PLAN_INVALID},
        // sections of the wrong JSON type: refused, never an exception out of decide() (it would end Kiln's boot)
        {"backend a string", [](json& p) { p["backend"] = "kiln"; }, KILN_LAUNCH_REASON_PLAN_INVALID},
        {"backend an array", [](json& p) { p["backend"] = json::array({"kiln"}); }, KILN_LAUNCH_REASON_PLAN_INVALID},
        {"game null", [](json& p) { p["game"] = nullptr; }, KILN_LAUNCH_REASON_PLAN_INVALID},
        {"writer an array", [](json& p) { p["writer"] = json::array(); }, KILN_LAUNCH_REASON_PLAN_INVALID},
        {"contract a number", [](json& p) { p["contract"] = 1; }, KILN_LAUNCH_REASON_PLAN_INVALID},
        {"a package a string", [](json& p) { p["packages"][0] = "alpha"; }, KILN_LAUNCH_REASON_PLAN_INVALID},
        {"a dll list of numbers", [](json& p) { p["packages"][0]["dlls"] = json::array({1}); }, KILN_LAUNCH_REASON_PLAN_INVALID},
        {"plan_id a number", [](json& p) { p["plan_id"] = 7; }, KILN_LAUNCH_REASON_PLAN_INVALID},
        {"a package without requires180", [](json& p) { p["packages"][0].erase("requires180"); },
         KILN_LAUNCH_REASON_PLAN_INVALID},
        {"a dll without bridgeAbi", [](json& p) { p["packages"][0]["dlls"][0].erase("bridgeAbi"); },
         KILN_LAUNCH_REASON_PLAN_INVALID},
        // the kiln-sfi.sf18 veto
        {"a candidate that requires 1.8", [](json& p) { p["packages"][0]["requires180"] = true; },
         KILN_LAUNCH_REASON_SF18_VETO},
        {"a candidate DLL on the 1.8 bridge", [](json& p) { p["packages"][0]["dlls"][0]["bridgeAbi"] = 2; },
         KILN_LAUNCH_REASON_SF18_VETO},
        {"a candidate that requires 1.8 in a plan that calls 1.8 final",
         [](json& p) {
             p["packages"][0]["requires180"] = true;
             p["contract"]["upstream180Final"] = true;
         },
         KILN_LAUNCH_REASON_SF18_VETO},
    };
    for (const auto& e : edits) {
        Run r;
        r.plan_bytes = mutated(e.edit);
        r.profile = profile_for(r.plan_bytes).dump();
        auto d = r.go();
        expect(d, KILN_LAUNCH_BACKEND_NONE, e.code, std::string("refused: ") + e.what);
        check(!d.notice.empty(), std::string("a refusal has a notice: ") + e.what);
    }
    {
        Run r;  // the veto names the package
        r.plan_bytes = mutated([](json& p) { p["packages"][0]["requires180"] = true; });
        r.profile = profile_for(r.plan_bytes).dump();
        check(has(r.go().reason, "alpha"), "the veto names the package that needs 1.8");
    }
    {
        Run r;  // a skipped package may need 1.8: it does not run
        r.plan_bytes = mutated([](json& p) { p["packages"][1]["requires180"] = true; });
        r.profile = profile_for(r.plan_bytes).dump();
        expect(r.go(), KILN_LAUNCH_BACKEND_KILN, KILN_LAUNCH_REASON_PLAN, "a skipped 1.8 package is no veto");
    }
    for (const char* origin : {"first-managed-unclassified", "rebaselined-unclassified", "unrecorded"}) {
        Run r;  // kiln-manage's plans name the archive base's origin
        r.plan_bytes = mutated([&](json& p) { p["game"]["archiveBaseOrigin"] = origin; });
        r.profile = profile_for(r.plan_bytes).dump();
        expect(r.go(), KILN_LAUNCH_BACKEND_KILN, KILN_LAUNCH_REASON_PLAN, std::string("archiveBaseOrigin ") + origin);
    }
    {
        // what kiln-manage's plan producer writes (the shared accept vector accept/produced.json, its own output for a
        // synthetic game; exe "a" x 64, plan id ...0001) is a plan the core selects kiln with
        const auto vector = std::filesystem::path(__FILE__).parent_path() / "../../../schema/fixtures/sfi/launch-plan/accept/produced.json";
        std::ifstream f(vector, std::ios::binary);
        check(f.good(), "the produced vector is there: " + vector.string());
        Run r;
        r.plan_bytes.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
        r.exe = std::string(64, 'a');
        r.profile = profile_for(r.plan_bytes, "00000000-0000-4000-8000-000000000001.json").dump();
        expect(r.go(), KILN_LAUNCH_BACKEND_KILN, KILN_LAUNCH_REASON_PLAN, "kiln-manage's produced plan");
    }
    {
        Run r;  // the veto open (a developer build) and a plan that calls 1.8 final: admitted
        r.allows_180 = true;
        r.plan_bytes = mutated([](json& p) {
            p["packages"][0]["requires180"] = true;
            p["contract"]["upstream180Final"] = true;
        });
        r.profile = profile_for(r.plan_bytes).dump();
        expect(r.go(), KILN_LAUNCH_BACKEND_KILN, KILN_LAUNCH_REASON_PLAN, "veto open + upstream final: kiln");
    }
    {
        Run r;  // the veto open but the plan does not call 1.8 final: still refused
        r.allows_180 = true;
        r.plan_bytes = mutated([](json& p) { p["packages"][0]["requires180"] = true; });
        r.profile = profile_for(r.plan_bytes).dump();
        expect(r.go(), KILN_LAUNCH_BACKEND_NONE, KILN_LAUNCH_REASON_SF18_VETO, "veto open, no upstream final: refused");
    }
    {
        Run r;
        r.side = KILN_SIDE_SERVER;
        expect(r.go(), KILN_LAUNCH_BACKEND_NONE, KILN_LAUNCH_REASON_PLAN_OTHER_GAME, "a client plan on a server: refused");
    }
    {
        Run r;
        r.side = KILN_SIDE_SERVER;
        r.plan_bytes = mutated([](json& p) { p["game"]["side"] = "server"; });
        r.profile = profile_for(r.plan_bytes).dump();
        expect(r.go(), KILN_LAUNCH_BACKEND_KILN, KILN_LAUNCH_REASON_PLAN, "a server plan on a server: kiln");
    }
}

void idle_class() {
    struct Row {
        std::string safe_idle;
        uint32_t mode;
        bool plugins;
        uint32_t code;
    };
    const Row rows[] = {
        {"the game image cannot be parsed", KILN_PROFILE_ACTIVE, true, KILN_LAUNCH_REASON_SAFE_IDLE},
        {"", KILN_PROFILE_INVALID, true, KILN_LAUNCH_REASON_PROFILE_INVALID},
        {"", KILN_PROFILE_NO_MODS, true, KILN_LAUNCH_REASON_NO_MODS},
        {"", KILN_PROFILE_ACTIVE, false, KILN_LAUNCH_REASON_PLUGINS_OFF},
        {"build x is not verified", KILN_PROFILE_NO_MODS, false, KILN_LAUNCH_REASON_SAFE_IDLE},
        {"", KILN_PROFILE_INVALID, false, KILN_LAUNCH_REASON_PROFILE_INVALID},
        {"", KILN_PROFILE_ACTIVE, true, 0},
        {"", KILN_PROFILE_LEGACY, true, 0},
        {"", KILN_PROFILE_ONCE, true, 0},
    };
    for (const auto& row : rows) {
        launch::Inputs in;
        launch::set_idle(in, row.safe_idle, row.mode, row.plugins);
        check(in.idle_code == row.code && (row.code == 0) == in.idle_text.empty(),
              "idle class for mode " + std::to_string(row.mode) + (row.plugins ? "" : ", plugins off") +
                  (row.safe_idle.empty() ? "" : ", safe idle"));
        if (!row.safe_idle.empty()) check(has(in.idle_text, row.safe_idle), "the safe-idle text is kept");
    }
    // the idle class beats every setting, kiln included, and reads no plan
    Run r;
    launch::Inputs in;
    in.kiln_json = &r.kiln_json;
    in.read_plan = [&](std::string_view) {
        ++r.reads;
        return launch::PlanFile{};
    };
    launch::set_idle(in, "the game image cannot be parsed", KILN_PROFILE_ACTIVE, true);
    auto d = launch::decide(in);
    expect(d, KILN_LAUNCH_BACKEND_NONE, KILN_LAUNCH_REASON_SAFE_IDLE, "safe idle gives backend none");
    check(has(d.reason, "the game image cannot be parsed") && r.reads == 0 && d.notice.empty(),
          "with the safe-idle reason, no plan read, no notice");
}

// The held row is the client gate's answer alone (native/core/gates/sfi.json after kiln.json "preview", with the
// release policy's rows: test-sfi-release-policy.sh).
void held_row() {
    auto h = launch::interpreter_hold(false, "");
    check(!h.held && h.text.empty(), "not held by the gate (preview lifted the row): open");
    h = launch::interpreter_hold(true, "game test pending: sfi-ui-page");
    check(h.held && h.text == "game test pending: sfi-ui-page", "held by the gate: held, with the gate's text");
    h = launch::interpreter_hold(true, "game test pending: the release policy (x)");
    check(h.held && has(h.text, "the release policy"), "held by the release policy's row: held, with its text");
}

void record() {
    launch::reset_for_tests();
    const KilnLaunchApi* t = launch::table();
    check(t && t->header.size == sizeof(KilnLaunchApi) && t->header.version == KILN_LAUNCH_VERSION && t->status &&
              t->notice_copy,
          "kiln.launch v1, 24 bytes, both members");
    KilnLaunchStatus st{};
    st.size = sizeof st;
    st.frozen = 7;
    check(t->status(&st) == KILN_ERR_UNAVAILABLE && st.frozen == 0, "before the freeze: unavailable, frozen 0");
    size_t len = 99;
    check(t->notice_copy(nullptr, 0, &len) == KILN_ERR_UNAVAILABLE, "notice before the freeze: unavailable");
    check(!launch::frozen() && launch::backend() == KILN_LAUNCH_BACKEND_NONE, "nothing frozen yet");

    launch::Decision d;
    d.backend = KILN_LAUNCH_BACKEND_NONE;
    d.reason_code = KILN_LAUNCH_REASON_PLAN_INVALID;
    // 120 ASCII bytes, then a 4-byte character across byte 127, then more
    d.reason = std::string(125, 'r') + "\xF0\x9F\x94\xA5" + std::string(40, 's');
    d.notice = "Kiln did not start ShroudForge mods this time.";
    d.plan_read = true;
    for (size_t i = 0; i < 32; ++i) d.plan_hash[i] = static_cast<uint8_t>(i + 1);
    check(!launch::freeze(d, 0, 1), "launch id 0 is refused");
    check(!launch::frozen(), "...and freezes nothing");
    check(launch::freeze(d, 0x1122334455667788ull, 1763500000123ull), "the freeze");
    check(launch::frozen() && launch::backend() == KILN_LAUNCH_BACKEND_NONE, "frozen");
    launch::Decision other = d;
    other.backend = KILN_LAUNCH_BACKEND_KILN;
    check(!launch::freeze(other, 5, 6), "a second freeze is refused");
    check(launch::backend() == KILN_LAUNCH_BACKEND_NONE, "...and changes nothing");

    check(t->status(nullptr) == KILN_ERR_INVALID_ARGUMENT, "status(NULL) is refused");
    KilnLaunchStatus small{};
    small.size = KILN_LAUNCH_STATUS_V1_SIZE - 1;
    check(t->status(&small) == KILN_ERR_INVALID_ARGUMENT && small.frozen == 0,
          "a record smaller than the first one (192 bytes) is refused");
    struct Bigger {
        KilnLaunchStatus v1;
        uint64_t later;
    } big{};
    big.v1.size = sizeof big;
    big.later = 42;
    check(t->status(&big.v1) == KILN_OK && big.v1.size == sizeof(KilnLaunchStatus) && big.later == 42,
          "a larger record gets v1 and its size says so; nothing past v1 is written");
    std::memset(&st, 0xCC, sizeof st);
    st.size = sizeof st;
    check(t->status(&st) == KILN_OK, "status after the freeze");
    check(st.frozen == 1 && st.backend == KILN_LAUNCH_BACKEND_NONE && st.reason_code == KILN_LAUNCH_REASON_PLAN_INVALID &&
              st.launch_id == 0x1122334455667788ull && st.freeze_stamp == 1763500000123ull,
          "the frozen fields");
    bool hash_ok = true;
    for (size_t i = 0; i < 32; ++i) hash_ok &= st.plan_hash[i] == i + 1;
    check(hash_ok, "the plan hash");
    const size_t n = strnlen(st.reason, sizeof st.reason);
    check(n == 125 && std::string(st.reason, n) == std::string(125, 'r'),
          "the reason is cut before a character that does not fit, not inside it");
    bool zeroed = true;
    for (size_t i = n; i < sizeof st.reason; ++i) zeroed &= st.reason[i] == 0;
    check(zeroed, "the rest of the reason field is zero");

    len = 0;
    check(t->notice_copy(nullptr, 0, &len) == KILN_ERR_BUFFER_TOO_SMALL && len == d.notice.size(),
          "a NULL buffer: too small, and the length");
    char buf[128];
    std::memset(buf, '#', sizeof buf);
    check(t->notice_copy(buf, d.notice.size() - 1, &len) == KILN_ERR_BUFFER_TOO_SMALL && buf[0] == '#',
          "one byte short: too small, nothing copied");
    check(t->notice_copy(buf, d.notice.size(), nullptr) == KILN_OK && std::string(buf, d.notice.size()) == d.notice &&
              buf[d.notice.size()] == '#',
          "exactly the length: copied, no terminator written");

    // an empty notice
    launch::reset_for_tests();
    launch::Decision hosted;
    hosted.backend = KILN_LAUNCH_BACKEND_HOSTED;
    hosted.reason_code = KILN_LAUNCH_REASON_HOSTED_DEFAULT;
    hosted.reason = "hosted";
    check(launch::freeze(hosted, 9, 10), "freeze hosted");
    len = 99;
    check(t->notice_copy(nullptr, 0, &len) == KILN_OK && len == 0, "an empty notice: OK with length 0, even without a buffer");
    std::memset(&st, 0, sizeof st);
    st.size = sizeof st;
    bool zero_hash = true;
    check(t->status(&st) == KILN_OK && st.backend == KILN_LAUNCH_BACKEND_HOSTED, "hosted frozen");
    for (uint8_t b : st.plan_hash) zero_hash &= b == 0;
    check(zero_hash, "no plan read: the hash is all zero");
}

// LP1a: kiln.json native.policy, appended to the record after its first 192 bytes, and (LP1b) build_verified beside it.
// A reader built before them (a 192-byte record) still gets every field of the first record, its size says 192, and
// nothing past those 192 bytes is written.
void native_policy_record() {
    const KilnLaunchApi* t = launch::table();
    check(sizeof(KilnLaunchStatus) == 200 && offsetof(KilnLaunchStatus, native_policy) == KILN_LAUNCH_STATUS_V1_SIZE &&
              offsetof(KilnLaunchStatus, build_verified) == 196 && KILN_LAUNCH_STATUS_V1_SIZE == 192,
          "the record: the first 192 bytes, then native_policy and build_verified (200)");
    check(launch::Decision{}.native_policy == KILN_LAUNCH_NATIVE_POLICY_NEVER,
          "a decision nobody gave kiln.json's policy says never (it refuses package DLLs, never allows them)");
    check(launch::Decision{}.build_verified == 0, "a decision nobody gave the build's answer says not verified");
    check(std::string(launch::native_policy_name(KILN_LAUNCH_NATIVE_POLICY_ALWAYS)) == "always" &&
              std::string(launch::native_policy_name(KILN_LAUNCH_NATIVE_POLICY_VERIFIED_ONLY)) == "verified-only" &&
              std::string(launch::native_policy_name(KILN_LAUNCH_NATIVE_POLICY_NEVER)) == "never" &&
              std::string(launch::native_policy_name(3)) == "unknown",
          "native_policy_name: kiln.json's words");

    struct Case {
        uint32_t backend, code;
    };
    std::vector<unsigned char> first_hosted;  // the first 192 bytes of a hosted record, under each policy
    for (const Case c : {Case{KILN_LAUNCH_BACKEND_KILN, KILN_LAUNCH_REASON_PLAN},
                         Case{KILN_LAUNCH_BACKEND_HOSTED, KILN_LAUNCH_REASON_HOSTED_DEFAULT},
                         Case{KILN_LAUNCH_BACKEND_INSTALLED, KILN_LAUNCH_REASON_SETTING},
                         Case{KILN_LAUNCH_BACKEND_NONE, KILN_LAUNCH_REASON_PLAN_INVALID},
                         Case{KILN_LAUNCH_BACKEND_NONE, KILN_LAUNCH_REASON_SAFE_IDLE}})
        for (const uint32_t policy : {KILN_LAUNCH_NATIVE_POLICY_ALWAYS, KILN_LAUNCH_NATIVE_POLICY_VERIFIED_ONLY,
                                      KILN_LAUNCH_NATIVE_POLICY_NEVER})
        for (const uint32_t verified : {0u, 1u}) {
            const std::string what = std::string(launch::backend_name(c.backend)) + "/0x" + [&] {
                char b[16];
                std::snprintf(b, sizeof b, "%x", c.code);
                return std::string(b);
            }() + ", native.policy " + launch::native_policy_name(policy) + (verified ? ", verified" : ", not verified");
            launch::reset_for_tests();
            launch::Decision d;
            d.backend = c.backend;
            d.reason_code = c.code;
            d.reason = "why";
            d.native_policy = policy;
            d.build_verified = verified;
            check(launch::freeze(d, 0x55, 0x66), what + ": frozen");
            KilnLaunchStatus st;
            std::memset(&st, 0xCC, sizeof st);
            st.size = sizeof st;
            // LP1b: the build's answer for every backend that runs, 0 for backend none whatever the boot answered
            const uint32_t expected = c.backend != KILN_LAUNCH_BACKEND_NONE ? verified : 0u;
            check(t->status(&st) == KILN_OK && st.size == 200 && st.backend == c.backend && st.reason_code == c.code &&
                      st.native_policy == policy && st.build_verified == expected,
                  what + ": a 200-byte reader gets native_policy, build_verified " + std::to_string(expected) +
                      " and size 200 (it got " + std::to_string(st.build_verified) + ")");

            // a reader built before native_policy: exactly 192 bytes of its own (ASan: a write past them is an overflow).
            // Its bytes are only read and written here as bytes: the test has no 200-byte object there.
            std::unique_ptr<uint64_t[]> old(new uint64_t[KILN_LAUNCH_STATUS_V1_SIZE / 8]);
            std::memset(old.get(), 0xCC, KILN_LAUNCH_STATUS_V1_SIZE);
            std::memcpy(old.get(), &kV1Size, 4);
            check(t->status(reinterpret_cast<KilnLaunchStatus*>(old.get())) == KILN_OK &&
                      std::memcmp(old.get(), &kV1Size, 4) == 0 &&
                      std::memcmp(reinterpret_cast<const unsigned char*>(old.get()) + 4,
                                  reinterpret_cast<const unsigned char*>(&st) + 4, KILN_LAUNCH_STATUS_V1_SIZE - 4) == 0,
                  what + ": a 192-byte reader gets every field of the first record, the same bytes, and size 192");

            // 192..199 bytes: the first record only, and nothing after byte 192 written
            unsigned char raw[208];
            std::memset(raw, 0xEE, sizeof raw);
            uint32_t size = KILN_LAUNCH_STATUS_V1_SIZE + 7;
            std::memcpy(raw, &size, sizeof size);
            check(t->status(reinterpret_cast<KilnLaunchStatus*>(raw)) == KILN_OK &&
                      std::memcmp(raw, &kV1Size, 4) == 0,
                  what + ": a 199-byte reader gets the first record and size 192");
            bool untouched = true;
            for (size_t i = KILN_LAUNCH_STATUS_V1_SIZE; i < sizeof raw; ++i) untouched &= raw[i] == 0xEE;
            check(untouched, what + ": ...nothing written past byte 192");

            if (c.backend == KILN_LAUNCH_BACKEND_HOSTED) {
                // hosted (and installed) do not read native_policy: its first record is the same bytes under every policy
                std::vector<unsigned char> bytes(reinterpret_cast<const unsigned char*>(&st),
                                                 reinterpret_cast<const unsigned char*>(&st) + KILN_LAUNCH_STATUS_V1_SIZE);
                if (first_hosted.empty()) first_hosted = bytes;
                check(bytes == first_hosted, what + ": the first record is byte for byte the same as under \"always\"");
            }
        }
    launch::reset_for_tests();
}

// Readers on other threads while the boot thread freezes: each read is either "not frozen" or the whole record.
void readers_race_the_freeze() {
    for (int round = 0; round < 20; ++round) {
        launch::reset_for_tests();
        launch::Decision d;
        d.backend = KILN_LAUNCH_BACKEND_KILN;
        d.reason_code = KILN_LAUNCH_REASON_PLAN;
        d.reason = "kiln, with the plan the profile names";
        d.notice = "ShroudForge mods run on Kiln's interpreter this start.";
        d.plan_read = true;
        d.plan_hash.fill(0xAB);
        d.native_policy = KILN_LAUNCH_NATIVE_POLICY_VERIFIED_ONLY;
        d.build_verified = 1;
        std::atomic<bool> go{false};
        std::atomic<int> torn{0}, seen{0};
        std::vector<std::thread> readers;
        for (int i = 0; i < 4; ++i)
            readers.emplace_back([&] {
                while (!go.load(std::memory_order_acquire)) std::this_thread::yield();
                for (int k = 0; k < 2000; ++k) {
                    KilnLaunchStatus st{};
                    st.size = sizeof st;
                    const KilnResult rc = launch::table()->status(&st);
                    if (rc == KILN_ERR_UNAVAILABLE) continue;
                    ++seen;
                    char buf[128];
                    size_t len = 0;
                    const bool whole = rc == KILN_OK && st.frozen == 1 && st.backend == KILN_LAUNCH_BACKEND_KILN &&
                                       st.launch_id == 77 && st.plan_hash[31] == 0xAB && st.size == sizeof st &&
                                       st.native_policy == KILN_LAUNCH_NATIVE_POLICY_VERIFIED_ONLY && st.build_verified == 1 &&
                                       std::string(st.reason, strnlen(st.reason, sizeof st.reason)) == "kiln, with the plan the profile names" &&
                                       launch::table()->notice_copy(buf, sizeof buf, &len) == KILN_OK && len == d.notice.size();
                    if (!whole) ++torn;
                }
            });
        go.store(true, std::memory_order_release);
        launch::freeze(d, 77, 1);
        for (auto& r : readers) r.join();
        if (torn) {
            check(false, "a reader saw a half-frozen record");
            return;
        }
    }
    check(true, "20 rounds: readers saw either nothing or the whole record");
}


// The ShroudForge-dialect owner (launch_internal.h): kiln/plugins/kiln-sfi.dll of this build, backend kiln, once.
void dialect_owner() {
    namespace fs = std::filesystem;
    const fs::path plugins = fs::path("/game") / "kiln" / "plugins";
    check(launch::is_dialect_dll(plugins / "kiln-sfi.dll", plugins), "kiln/plugins/kiln-sfi.dll is the dialect DLL");
    check(launch::is_dialect_dll(plugins / "KILN-SFI.DLL", plugins), "...whatever its case (Windows file names)");
    check(launch::is_dialect_dll(plugins / ".." / "plugins" / "kiln-sfi.dll", plugins), "...after normalising");
    check(!launch::is_dialect_dll(plugins / "kiln-sfi2.dll", plugins) &&
              !launch::is_dialect_dll(plugins / "kiln-shroudforge.dll", plugins) &&
              !launch::is_dialect_dll(plugins / "kiln-sfi.dll.bak", plugins),
          "no other name");
    check(!launch::is_dialect_dll(fs::path("/game") / "mods" / "x" / "kiln-sfi.dll", plugins) &&
              !launch::is_dialect_dll(plugins / "sub" / "kiln-sfi.dll", plugins) &&
              !launch::is_dialect_dll(fs::path("/game") / "kiln" / "kiln-sfi.dll", plugins),
          "no copy in another folder (a profile entry pointing elsewhere)");

    std::string why;
    launch::reset_for_tests();
    const KilnLaunchInternalApi* t = launch::internal_table();
    check(t && t->header.size == sizeof(KilnLaunchInternalApi) && t->header.version == KILN_LAUNCH_INTERNAL_VERSION &&
              t->dialect_owner && t->dialect_owner() == 0,
          "kiln.internal.launch v1, 16 bytes; nobody designated yet");
    check(!launch::wants_dialect_owner() && !launch::designate(5, true, why) && launch::dialect_owner() == 0,
          "before the freeze: nobody is designated");

    launch::Decision hosted;
    hosted.backend = KILN_LAUNCH_BACKEND_HOSTED;
    hosted.reason_code = KILN_LAUNCH_REASON_SETTING;
    hosted.reason = "hosted";
    launch::freeze(hosted, 1, 1);
    check(!launch::wants_dialect_owner() && !launch::designate(5, true, why) && launch::dialect_owner() == 0 &&
              has(why, "hosted"),
          "backend hosted: nobody is designated, and why says the backend");

    launch::reset_for_tests();
    launch::Decision none;
    none.reason_code = KILN_LAUNCH_REASON_INTERPRETER_HELD;
    none.reason = "held";
    launch::freeze(none, 1, 1);
    check(!launch::wants_dialect_owner() && !launch::designate(5, true, why) && launch::dialect_owner() == 0,
          "backend none (kiln refused): nobody is designated");

    launch::reset_for_tests();
    launch::Decision kiln;
    kiln.backend = KILN_LAUNCH_BACKEND_KILN;
    kiln.reason_code = KILN_LAUNCH_REASON_PLAN;
    kiln.reason = "kiln";
    launch::freeze(kiln, 1, 1);
    check(launch::wants_dialect_owner(), "backend kiln: the loader asks");
    check(!launch::designate(0, true, why) && launch::dialect_owner() == 0, "owner 0 (the core) is never designated");
    check(!launch::designate(5, false, why) && has(why, "build stamp") && launch::dialect_owner() == 0 &&
              launch::wants_dialect_owner(),
          "a DLL of another build: not designated (why names the stamp)");
    check(launch::designate(5, true, why) && launch::dialect_owner() == 5 && t->dialect_owner() == 5,
          "the stamped kiln-sfi.dll's owner is designated, and kiln.internal.launch says so");
    check(!launch::wants_dialect_owner(), "...and nobody else is asked about");
    check(!launch::designate(6, true, why) && launch::dialect_owner() == 5, "never moved to another owner");
    check(!launch::designate(5, true, why) && launch::dialect_owner() == 5, "never twice, not even for the same owner");
    launch::reset_for_tests();
    check(t->dialect_owner() == 0, "reset (tests only) forgets it");
}

// Two loaders racing to be designated, and readers of kiln.internal.launch: exactly one wins, and a reader never sees
// the owner change once it is set.
void designation_race() {
    for (int round = 0; round < 50; ++round) {
        launch::reset_for_tests();
        launch::Decision kiln;
        kiln.backend = KILN_LAUNCH_BACKEND_KILN;
        kiln.reason_code = KILN_LAUNCH_REASON_PLAN;
        kiln.reason = "kiln";
        launch::freeze(kiln, 1, 1);
        std::atomic<bool> go{false};
        std::atomic<int> wins{0}, moved{0};
        std::vector<std::thread> threads;
        for (KilnOwner owner : {11, 12})
            threads.emplace_back([&, owner] {
                while (!go.load(std::memory_order_acquire)) std::this_thread::yield();
                std::string why;
                if (launch::designate(owner, true, why)) ++wins;
            });
        threads.emplace_back([&] {
            while (!go.load(std::memory_order_acquire)) std::this_thread::yield();
            KilnOwner first = 0;
            for (int k = 0; k < 2000; ++k) {
                const KilnOwner now = launch::internal_table()->dialect_owner();
                if (now != 0 && now != 11 && now != 12) ++moved;
                if (first == 0) first = now;
                else if (now != first) ++moved;
            }
        });
        go.store(true, std::memory_order_release);
        for (auto& th : threads) th.join();
        if (wins != 1 || moved) {
            check(false, "round " + std::to_string(round) + ": " + std::to_string(wins.load()) + " designations, " +
                             std::to_string(moved.load()) + " changes seen");
            return;
        }
    }
    check(true, "50 rounds: one designation each, never seen to change");
}

// kiln/state/launch.json: the same record, the whole reason, for the launcher; written once per boot.
void state_file() {
    launch::Decision d;
    d.backend = KILN_LAUNCH_BACKEND_KILN;
    d.reason_code = KILN_LAUNCH_REASON_PLAN;
    d.reason = std::string(300, 'k');
    d.notice = "Your ShroudForge mods run on Kiln's own interpreter this start.";
    d.plan_read = true;
    d.plan_hash.fill(0xAB);
    d.plan_id = kPlanId;
    d.native_policy = KILN_LAUNCH_NATIVE_POLICY_VERIFIED_ONLY;
    json j = json::parse(launch::state_json(d, 0x0123456789abcdefull, 1763500000123ull, 4242, "0.2.0-dev+20261119.abc"),
                               nullptr, false);
    check(j.is_object() && j["format"] == 1 && j["kiln"] == "0.2.0-dev+20261119.abc" && j["pid"] == 4242 &&
              j["launchId"] == "0123456789abcdef" && j["frozen"] == true && j["backend"] == "kiln" &&
              j["reasonCode"] == KILN_LAUNCH_REASON_PLAN && j["reasonClass"] == "selected" && j["reason"] == d.reason &&
              j["notice"] == d.notice && j["planId"] == kPlanId && j["freezeStamp"] == 1763500000123ull &&
              j["nativePolicy"] == "verified-only",
          "launch.json: every field of a kiln record, the whole reason (not cut to 127 bytes), kiln.json native.policy");
    check(j.value("planSha256", "") == [] {
        std::string s;
        for (int i = 0; i < 32; ++i) s += "ab";
        return s;
    }(),
          "launch.json: the plan's SHA-256 in hex");
    launch::Decision refused;
    refused.reason_code = KILN_LAUNCH_REASON_PLAN_STAMPED;
    refused.reason = "stamped";
    refused.notice = "off";
    refused.plan_read = true;
    json r = json::parse(launch::state_json(refused, 1, 2, 3, "k"), nullptr, false);
    check(r["backend"] == "none" && r["reasonClass"] == "refused" && r["planId"].is_null() && r["planSha256"].is_string(),
          "launch.json: a refused plan keeps its hash, has no plan id");
    launch::Decision idle;
    idle.reason_code = KILN_LAUNCH_REASON_SAFE_IDLE;
    idle.reason = "safe idle";
    json i = json::parse(launch::state_json(idle, 1, 2, 3, "k"), nullptr, false);
    check(i["reasonClass"] == "idle" && i["planSha256"].is_null() && i["notice"] == "", "launch.json: an idle record");
    for (const auto& [policy, word] : {std::pair{KILN_LAUNCH_NATIVE_POLICY_ALWAYS, "always"},
                                       std::pair{KILN_LAUNCH_NATIVE_POLICY_NEVER, "never"}}) {
        idle.native_policy = policy;
        check(json::parse(launch::state_json(idle, 1, 2, 3, "k"), nullptr, false)["nativePolicy"] == word,
              std::string("launch.json: nativePolicy \"") + word + "\" for an idle record too");
    }

    launch::reset_for_tests();
    int writes = 0;
    const auto write = [&] { ++writes; };
    check(!launch::freeze(d, 0, 2, write) && writes == 0, "a refused freeze (launch id 0) writes nothing");
    check(launch::freeze(d, 1, 2, write) && writes == 1, "the freeze writes launch.json once");
    check(launch::frozen(), "...after the record is frozen");
    check(!launch::freeze(d, 3, 4, write) && writes == 1, "a second freeze writes nothing: once per boot");

    // the write retried while a reader holds the old file (the rename is refused)
    int tries = 0, sleeps = 0;
    uint32_t slept = 0;
    const auto sleep = [&](uint32_t ms) { ++sleeps, slept += ms; };
    check(launch::write_retrying([&] { return ++tries > 3; }, sleep) && tries == 4 && sleeps == 3 &&
              slept == 3 * launch::kStateWriteWaitMs,
          "three refused renames, then the write: retried, 25 ms apart");
    tries = sleeps = 0;
    slept = 0;
    check(!launch::write_retrying([&] { return ++tries, false; }, sleep) && tries == launch::kStateWriteAttempts &&
              sleeps == launch::kStateWriteAttempts - 1 && slept <= 500,
          "never written: given up after 20 tries, within half a second");
    tries = sleeps = 0;
    check(launch::write_retrying([&] { return ++tries, true; }, sleep) && tries == 1 && sleeps == 0,
          "written at once: no wait");
}
}  // namespace

// Whether `s` is UTF-8 (nlohmann's strict dump() throws otherwise).
bool is_utf8(const std::string& s) {
    try {
        (void)json(s).dump();
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

// Nothing in the decision, the freeze or launch.json's text can throw, crash or bloat on hostile input: an exception
// out of freeze_at_boot ends Kiln's whole boot (kiln_boot's catch), before any native mod, plugin or chain DLL.
void robustness() {
    {
        // boot.cpp's verified-only safe-idle text quotes build_verified's note, which quotes nlohmann's parse error of
        // the build record, which copies the offending bytes: a record saved as Latin-1 puts 0xE9 into the reason
        std::string note;
        try {
            (void)json::parse("{\"note\": \"caf\xE9\"}");
        } catch (const std::exception& e) {
            note = std::string("build record unreadable: ") + e.what();
        }
        check(!is_utf8(note), "the parse error really carries the byte that is not UTF-8");
        launch::Inputs in;
        launch::set_idle(in, "build eee is not verified (" + note + ") and policy is verified-only", KILN_PROFILE_ACTIVE, true);
        const auto d = launch::decide(in);
        expect(d, KILN_LAUNCH_BACKEND_NONE, KILN_LAUNCH_REASON_SAFE_IDLE, "safe idle quoting bytes that are not UTF-8");
        check(is_utf8(d.reason) && has(d.reason, "caf?"), "decide() hands out a UTF-8 reason (the byte is a '?')");
        bool threw = false;
        std::string text;
        try {
            text = launch::state_json(d, 1, 2, 3, "k");
        } catch (const std::exception&) {
            threw = true;
        }
        check(!threw && json::parse(text, nullptr, false).is_object(), "launch.json's text of that record: no exception");
        launch::Decision raw;  // a reason that bypassed decide(): still never thrown
        raw.reason = "x\xC3(\xED\xA0\x80\xF8\x88\x80\x80\x80";
        threw = false;
        try {
            text = launch::state_json(raw, 1, 2, 3, "k");
        } catch (const std::exception&) {
            threw = true;
        }
        check(!threw && json::parse(text, nullptr, false).is_object(), "launch.json's text of a raw non-UTF-8 reason: no exception");
    }
    {
        // kiln.json is not depth-checked: a backend nested 200000 levels deep must not be dumped recursively
        Run r;
        r.kiln_json = "{\"shroudforge\": {\"backend\": " + std::string(200000, '[') + std::string(200000, ']') + "}}";
        const auto d = r.go();
        expect(d, KILN_LAUNCH_BACKEND_NONE, KILN_LAUNCH_REASON_SETTING_INVALID, "a backend nested 200000 deep: refused");
        check(d.reason.size() < 256 && has(d.reason, "an array"), "...with a short reason: " + d.reason.substr(0, 200));
    }
    std::string wide = "[";
    for (int i = 0; i < 1000000; ++i) wide += "1,";
    wide += "1]";
    for (const std::string& value : {wide, "\"" + std::string(3000000, 'k') + "\"",
                                     "{\"a\": \"" + std::string(100000, 'x') + "\"}"}) {
        Run r;
        r.kiln_json = "{\"shroudforge\": {\"backend\": " + value + "}}";
        const auto d = r.go();
        expect(d, KILN_LAUNCH_BACKEND_NONE, KILN_LAUNCH_REASON_SETTING_INVALID, "a huge backend value: refused");
        check(d.reason.size() < 256, "...with a short reason (" + std::to_string(d.reason.size()) + " bytes)");
    }
    {
        Run r;  // a multi-byte character across the 64-byte cut of a long string value
        r.kiln_json = "{\"shroudforge\": {\"backend\": \"" + std::string(63, 'k') + "\xE2\x82\xAC" + std::string(100, 'k') + "\"}}";
        const auto d = r.go();
        check(is_utf8(d.reason) && d.reason.size() < 256 && has(d.reason, "a long string"), "a long string is cut whole");
    }
    {
        Run r;  // a profile reference with a path of a mebibyte
        r.profile = json{{"sfLaunchPlan", {{"path", std::string(1u << 20, 'p')}, {"sha256", kHash}}}}.dump();
        const auto d = r.go();
        expect(d, KILN_LAUNCH_BACKEND_NONE, KILN_LAUNCH_REASON_PLAN_INVALID, "a mebibyte path: refused");
        check(d.reason.size() < 512, "...with a short reason (" + std::to_string(d.reason.size()) + " bytes)");
    }
    for (const char* key : {"format", "plan_id", "launch_id"}) {
        Run r;  // a plan member that is a huge array (the plan's own size is the only bound)
        json p = plan();
        p[key] = json::array();
        for (int i = 0; i < 200000; ++i) p[key].push_back(i);
        r.plan_bytes = p.dump();
        const auto d = r.go();
        check(d.backend == KILN_LAUNCH_BACKEND_NONE && KILN_LAUNCH_REASON_REFUSED(d.reason_code) && d.reason.size() < 512,
              std::string("a huge plan member (") + key + "): refused with a short reason (" + std::to_string(d.reason.size()) +
                  " bytes)");
    }
    {
        Run r;  // the veto names a package whose id is a mebibyte
        r.plan_bytes = [] {
            json p = plan();
            p["packages"][0]["id"] = std::string(1u << 20, 'i');
            p["packages"][0]["requires180"] = true;
            return p.dump();
        }();
        const auto d = r.go();
        expect(d, KILN_LAUNCH_BACKEND_NONE, KILN_LAUNCH_REASON_SF18_VETO, "the veto with a mebibyte package id");
        check(d.reason.size() < 512, "...with a short reason (" + std::to_string(d.reason.size()) + " bytes)");
    }
    {
        // a publish callback that throws (launch.json's text or write): the record stays frozen, nothing escapes
        launch::reset_for_tests();
        launch::Decision hosted;
        hosted.backend = KILN_LAUNCH_BACKEND_HOSTED;
        hosted.reason_code = KILN_LAUNCH_REASON_HOSTED_DEFAULT;
        hosted.reason = "hosted";
        bool ok = false, threw = false;
        try {
            ok = launch::freeze(hosted, 3, 4, [] { throw std::runtime_error("disk on fire"); });
        } catch (...) {
            threw = true;
        }
        check(ok && !threw && launch::frozen() && launch::backend() == KILN_LAUNCH_BACKEND_HOSTED,
              "a throwing publish: frozen, freeze() true, no exception");
        launch::reset_for_tests();
    }
    {
        // freeze_at_boot's fallback when reading the inputs or deciding threw anyway
        const auto d = launch::undecided("bad allocation \xFF");
        expect(d, KILN_LAUNCH_BACKEND_NONE, KILN_LAUNCH_REASON_UNDECIDED, "undecided: backend none");
        check(KILN_LAUNCH_REASON_IDLE(d.reason_code) && !KILN_LAUNCH_REASON_REFUSED(d.reason_code) && d.notice.empty(),
              "undecided is idle class (every loader keeps its old rules), no notice");
        check(has(d.reason, "bad allocation") && is_utf8(d.reason), "undecided names what failed, in UTF-8");
        check(launch::undecided(std::string(10000, 'w')).reason.size() < 512, "undecided's reason is bounded");
    }
}

int main() {
    robustness();
    settings();
    kiln_plans();
    idle_class();
    held_row();
    record();
    native_policy_record();
    readers_race_the_freeze();
    dialect_owner();
    designation_race();
    state_file();
    std::printf("launch: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
