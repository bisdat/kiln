// SPDX-License-Identifier: MIT
// kiln.launch: the pure validator, the freeze and the table (launch.h, abi/services/kiln_launch.h).
// Portable C++: no windows.h and no other core header but the ABI's, so test-launch.sh builds this file on the host.
#include "launch.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <memory>
#include <set>
#include <vector>

#include "nlohmann/json.hpp"
#include "pe_text.h"  // bounds-checked PE header reads
#include "services/boot_gamedata.h"  // the core's SHA-256 (header-only, portable)
#include "services/kiln_profile.h"

namespace kiln::launch {
namespace {

using nlohmann::json;

// At most `max` bytes of UTF-8 `s`, never inside a character.
std::string cut_utf8(std::string_view s, size_t max) {
    size_t n = std::min(s.size(), max);
    while (n > 0 && n < s.size() && (static_cast<unsigned char>(s[n]) & 0xC0) == 0x80) --n;
    return std::string(s.substr(0, n));
}

// A JSON value as reason text, short and without recursion: a string quoted and cut to 64 bytes, a number, true, false
// or null as it is, an array or an object by its kind only. A setting or a plan member can be megabytes long or nested
// a million levels deep (kiln.json is not depth-checked): dump() of such a value would put megabytes into the reason,
// kiln.log and launch.json, or recurse until the boot thread's stack overflows.
constexpr size_t kBriefMax = 64;
std::string brief(const json& v) {
    if (v.is_string()) {
        const auto& s = v.get_ref<const std::string&>();
        return s.size() <= kBriefMax ? json(s).dump() : json(cut_utf8(s, kBriefMax)).dump().insert(0, "a long string ");
    }
    if (v.is_array()) return "an array";
    if (v.is_object()) return "an object";
    if (v.is_discarded()) return "nothing";
    return v.dump();  // number, boolean or null: no recursion, a few bytes
}

// `s` with every byte that is not part of a well-formed UTF-8 character replaced by '?'. The reason can carry text the
// decision did not make itself (boot's safe-idle text quotes a JSON parser's error, which copies the offending bytes),
// and both kiln.launch's record and launch.json promise UTF-8 (nlohmann's dump() throws on anything else).
std::string utf8_clean(std::string_view s) {
    static constexpr uint32_t kMin[] = {0, 0, 0x80, 0x800, 0x10000};
    std::string out;
    out.reserve(s.size());
    size_t i = 0;
    while (i < s.size()) {
        const auto c = static_cast<unsigned char>(s[i]);
        const size_t n = c < 0x80 ? 1 : (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xE ? 3 : (c >> 3) == 0x1E ? 4 : 0;
        bool ok = n != 0 && i + n <= s.size();
        for (size_t k = 1; ok && k < n; ++k) ok = (static_cast<unsigned char>(s[i + k]) & 0xC0) == 0x80;
        if (ok && n > 1) {
            uint32_t cp = c & (0xFFu >> (n + 1));
            for (size_t k = 1; k < n; ++k) cp = (cp << 6) | (static_cast<unsigned char>(s[i + k]) & 0x3Fu);
            ok = cp >= kMin[n] && cp <= 0x10FFFF && (cp < 0xD800 || cp > 0xDFFF);
        }
        if (ok) {
            out.append(s.substr(i, n));
            i += n;
        } else {
            out += '?';
            ++i;
        }
    }
    return out;
}

Decision chosen(uint32_t backend, uint32_t code, std::string reason) {
    Decision d;
    d.backend = backend;
    d.reason_code = code;
    d.reason = std::move(reason);
    if (backend == KILN_LAUNCH_BACKEND_KILN)
        d.notice = "Your ShroudForge mods run on Kiln's own interpreter this start (kiln.json shroudforge.backend: "
                   "\"kiln\").";
    return d;
}

// The refused class: every ShroudForge backend stands aside this start, and the notice says so.
Decision refused(uint32_t code, std::string reason) {
    Decision d;
    d.reason_code = code;
    d.reason = std::move(reason);
    d.notice = "Kiln did not start your ShroudForge mods: " + d.reason +
               ". No other ShroudForge runs them in its place this start.";
    return d;
}

// ---- strict JSON: duplicate keys refused, nesting bounded (the launch plan's rules, kiln-sfi launch_plan.rs) ----
constexpr int kDepthMax = 64;

bool strict_parse(std::string_view text, json& out, std::string& why) {
    std::vector<std::set<std::string>> keys;
    bool duplicate = false, deep = false;
    const json::parser_callback_t watch = [&](int depth, json::parse_event_t event, json& parsed) {
        if (depth > kDepthMax) deep = true;
        switch (event) {
            case json::parse_event_t::object_start: keys.emplace_back(); break;
            case json::parse_event_t::key:
                if (!keys.empty() && !keys.back().insert(parsed.get<std::string>()).second) duplicate = true;
                break;
            case json::parse_event_t::object_end:
                if (!keys.empty()) keys.pop_back();
                break;
            default: break;
        }
        return true;
    };
    out = json::parse(text.begin(), text.end(), watch, false);
    if (out.is_discarded()) why = "is not valid JSON";
    else if (duplicate) why = "has a key twice in one object";
    else if (deep) why = "nests deeper than 64 levels";
    else return true;
    return false;
}

bool exact_keys(const json& o, std::initializer_list<const char*> names) {
    if (!o.is_object() || o.size() != names.size()) return false;
    return std::all_of(names.begin(), names.end(), [&](const char* k) { return o.contains(k); });
}
bool text(const json& v) {
    if (!v.is_string()) return false;
    const auto& s = v.get_ref<const std::string&>();
    return !s.empty() && s.find('\0') == std::string::npos;
}
bool lower_hex(std::string_view s, size_t n) {
    return s.size() == n && std::all_of(s.begin(), s.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}
bool hash(const json& v) { return text(v) && lower_hex(v.get_ref<const std::string&>(), 64); }
bool canonical_uuid(std::string_view s) {
    if (s.size() != 36) return false;
    for (size_t i = 0; i < s.size(); ++i) {
        const bool dash = i == 8 || i == 13 || i == 18 || i == 23;
        if (dash ? s[i] != '-' : !lower_hex(s.substr(i, 1), 1)) return false;
    }
    return true;
}
bool is_one(const json& v) { return v.is_number_unsigned() && v.get<uint64_t>() == 1; }
// A member's JSON text for a reason, whatever `o` is ("none" when it is not an object or has no such member).
std::string member_text(const json& o, const char* key) {
    return o.is_object() && o.contains(key) ? brief(o[key]) : std::string("none");
}
bool one_of(const json& v, std::initializer_list<const char*> values) {
    return v.is_string() && std::any_of(values.begin(), values.end(), [&](const char* x) { return v.get_ref<const std::string&>() == x; });
}

// ---- kiln.json "shroudforge" (game level) ----
struct Setting {
    enum Kind { Default, Hosted, Installed, Kiln, Invalid } kind = Default;
    uint32_t code = KILN_LAUNCH_REASON_HOSTED_DEFAULT;
    std::string text;
};

Setting read_setting(const std::string* kiln_json) {
    Setting s;
    s.text = "no kiln.json setting: hosted, the default";
    if (!kiln_json) return s;
    const json doc = json::parse(*kiln_json, nullptr, false);
    if (!doc.is_object()) {
        s.text = "kiln.json does not parse: hosted, the default";
        return s;
    }
    const auto sf = doc.find("shroudforge");
    if (sf == doc.end() || !sf->is_object()) return s;
    if (const auto b = sf->find("backend"); b != sf->end()) {  // the new key wins over useInstalled
        s.code = KILN_LAUNCH_REASON_SETTING;
        s.text = "kiln.json shroudforge.backend: " + brief(*b);
        if (one_of(*b, {"hosted"})) s.kind = Setting::Hosted;
        else if (one_of(*b, {"installed"})) s.kind = Setting::Installed;
        else if (one_of(*b, {"kiln"})) s.kind = Setting::Kiln;
        else {
            s.kind = Setting::Invalid;
            s.code = KILN_LAUNCH_REASON_SETTING_INVALID;
            s.text += " is not \"hosted\", \"installed\" or \"kiln\"";
        }
        return s;
    }
    // The older key, read as kiln-shroudforge reads it: true means installed; any other value leaves the default.
    if (const auto u = sf->find("useInstalled"); u != sf->end() && u->is_boolean() && u->get<bool>()) {
        s.kind = Setting::Installed;
        s.code = KILN_LAUNCH_REASON_USE_INSTALLED;
        s.text = "kiln.json shroudforge.useInstalled: true (the older setting): installed";
    }
    return s;
}

// ---- the profile's reference to its launch plan ----
struct Reference {
    enum State { Absent, Bad, Ok } state = Absent;
    std::string why;   // Bad
    std::string uuid;  // Ok: the plan id the file name carries
    std::string sha256;
};

Reference read_reference(std::string_view profile_text) {
    Reference r;
    if (profile_text.empty()) {
        r.why = "this start has no profile (a start without the launcher's profile.json names no launch plan)";
        return r;
    }
    const json doc = json::parse(profile_text, nullptr, false);
    if (!doc.is_object()) {
        r.state = Reference::Bad;
        r.why = "the profile does not parse";
        return r;
    }
    const auto ref = doc.find("sfLaunchPlan");
    if (ref == doc.end()) {
        r.why = "the profile names no launch plan (sfLaunchPlan)";
        return r;
    }
    r.state = Reference::Bad;
    if (!exact_keys(*ref, {"path", "sha256"}) || !text((*ref)["path"]) || !hash((*ref)["sha256"])) {
        r.why = "the profile's sfLaunchPlan is not {\"path\": \"launch-plans/<plan id>.json\", \"sha256\": \"<64 hex digits>\"}";
        return r;
    }
    // Strictly below <game>/kiln/launch-plans, one level, a canonical plan id as the name: no other path can be named.
    const std::string& path = (*ref)["path"].get_ref<const std::string&>();
    const std::string prefix = std::string(kPlanFolder) + "/", suffix = ".json";
    if (path.size() != prefix.size() + 36 + suffix.size() || path.compare(0, prefix.size(), prefix) != 0 ||
        path.compare(path.size() - suffix.size(), suffix.size(), suffix) != 0 ||
        !canonical_uuid(std::string_view(path).substr(prefix.size(), 36))) {
        r.why = "the profile's sfLaunchPlan path " + brief((*ref)["path"]) + " is not launch-plans/<plan id>.json";
        return r;
    }
    r.state = Reference::Ok;
    r.uuid = path.substr(prefix.size(), 36);
    r.sha256 = (*ref)["sha256"].get<std::string>();
    return r;
}

// ---- the launch plan (format 1; the checks the selection depends on; kiln-sfi checks the whole plan again) ----
Decision check_plan(const Inputs& in, const Reference& ref, const std::string& bytes, Decision d) {
    const std::string name = "the launch plan " + std::string(kPlanFolder) + "/" + ref.uuid + ".json";
    auto invalid = [&](const std::string& why) {
        Decision r = refused(KILN_LAUNCH_REASON_PLAN_INVALID, name + " " + why);
        r.plan_read = d.plan_read, r.plan_hash = d.plan_hash;
        return r;
    };
    auto with = [&](uint32_t code, const std::string& why) {
        Decision r = refused(code, name + " " + why);
        r.plan_read = d.plan_read, r.plan_hash = d.plan_hash;
        return r;
    };
    if (bytes.size() > kPlanMax) return invalid("is larger than 8 MiB");
    d.plan_read = true;
    d.plan_hash = boot_gd::sha256(bytes);
    if (boot_gd::hex_of(d.plan_hash) != ref.sha256)
        return invalid("changed since kiln-manage wrote it (its SHA-256 is not the one the profile names); activate the "
                       "profile in the launcher again");
    if (bytes.empty()) return invalid("is empty");
    json p;
    std::string why;
    if (!strict_parse(bytes, p, why)) return invalid(why);
    if (!exact_keys(p, {"format", "minReader", "plan_id", "launch_id", "startupDialect", "writer", "profile", "game",
                        "backend", "contract", "fingerprints", "packages", "loadOrder", "warnings"}))
        return invalid("is not a launch plan object of format 1 (a key is missing or unknown)");
    if (!is_one(p["format"]) || !is_one(p["minReader"]))
        return invalid("has format " + brief(p["format"]) + " / minReader " + brief(p["minReader"]) +
                       "; this Kiln reads format 1");
    if (!text(p["plan_id"]) || !canonical_uuid(p["plan_id"].get_ref<const std::string&>()) ||
        p["plan_id"].get_ref<const std::string&>() != ref.uuid)
        return invalid("has the plan id " + brief(p["plan_id"]) + ", not the one its file name carries");
    if (!p["launch_id"].is_null())
        return with(KILN_LAUNCH_REASON_PLAN_STAMPED,
                    "carries a launch id (" + brief(p["launch_id"]) +
                        "): kiln-manage writes none, and Kiln gives each start its own; activate the profile again");
    if (!one_of(p["startupDialect"], {"shroudforge", "eml", "none"})) return invalid("has an unknown startupDialect");
    const json& writer = p["writer"];
    if (!exact_keys(writer, {"component", "version"}) || !text(writer["version"]) || !one_of(writer["component"], {"kiln-manage"}))
        return invalid("was not written by kiln-manage");
    const json& game = p["game"];
    // archiveBaseOrigin: optional, and one of the inspector's three tokens when present (never "vanilla")
    const bool origin = game.is_object() && game.contains("archiveBaseOrigin");
    if (origin && !one_of(game["archiveBaseOrigin"], {"first-managed-unclassified", "rebaselined-unclassified", "unrecorded"}))
        return invalid("has an unknown game.archiveBaseOrigin " + brief(game["archiveBaseOrigin"]));
    if (!(origin ? exact_keys(game, {"side", "exe_sha256", "archiveBase_sha256", "archiveInput_sha256", "archiveBaseOrigin"})
                 : exact_keys(game, {"side", "exe_sha256", "archiveBase_sha256", "archiveInput_sha256"})) ||
        !one_of(game["side"], {"client", "server"}) || !hash(game["exe_sha256"]) || !hash(game["archiveBase_sha256"]) ||
        !hash(game["archiveInput_sha256"]))
        return invalid("has no valid game section");
    const std::string side = in.side == KILN_SIDE_SERVER ? "server" : "client";
    if (game["side"].get_ref<const std::string&>() != side)
        return with(KILN_LAUNCH_REASON_PLAN_OTHER_GAME, "is for the " + game["side"].get<std::string>() + ", not this " + side);
    if (game["exe_sha256"].get_ref<const std::string&>() != in.exe_sha256)
        return with(KILN_LAUNCH_REASON_PLAN_OTHER_GAME,
                    "is for another game executable (a game update since the launcher wrote it?); activate the profile again");
    const json& backend = p["backend"];
    if (!exact_keys(backend, {"requested", "selected", "fallbackOrder", "dataWriter", "selectionReason"}) ||
        !one_of(backend["requested"], {"kiln"}) || !one_of(backend["selected"], {"kiln"}))
        return invalid("does not select kiln (requested " + member_text(backend, "requested") + ", selected " +
                       member_text(backend, "selected") + ") while kiln.json asks for it; activate the profile again");
    if (!one_of(backend["dataWriter"], {"kiln-eml"})) return invalid("selects kiln without kiln-eml as its data writer");
    const json& contract = p["contract"];
    if (!exact_keys(contract, {"apiVersion", "sourceCommit", "upstream180Final", "inventory_sha256", "parity_sha256",
                               "serviceContracts_sha256"}) ||
        !contract["upstream180Final"].is_boolean())
        return invalid("has no valid contract section");
    const json& packages = p["packages"];
    if (!packages.is_array() || packages.size() > 4096) return invalid("has no valid package list");
    std::string needs_180;  // the first admitted package that needs ShroudForge 1.8
    for (const json& pkg : packages) {
        if (!pkg.is_object() || !text(pkg.value("id", json())) || !text(pkg.value("decision", json())) ||
            !pkg.value("requires180", json()).is_boolean() || !pkg.value("dlls", json()).is_array())
            return invalid("has a package entry without id, decision, requires180 or dlls");
        bool bridge_180 = false;
        for (const json& dll : pkg["dlls"]) {
            if (!dll.is_object() || !dll.value("bridgeAbi", json()).is_number_unsigned())
                return invalid("has a package DLL without bridgeAbi");
            bridge_180 |= dll["bridgeAbi"].get<uint64_t>() == 2;
        }
        if (pkg["decision"].get_ref<const std::string&>() == "candidate" && (pkg["requires180"].get<bool>() || bridge_180) &&
            needs_180.empty())
            needs_180 = pkg["id"].get<std::string>();
    }
    // The kiln-sfi.sf18 veto: nothing of 1.8 runs unless the shipped release policy allows it AND the plan says a final
    // 1.8 exists (kiln-sfi launch_plan.rs: requires180 && !(upstream180Final && upstream_180_final)).
    if (!needs_180.empty() && !(in.release_allows_180 && contract["upstream180Final"].get<bool>()))
        return with(KILN_LAUNCH_REASON_SF18_VETO,
                    "admits " + brief(json(needs_180)) +
                        ", which needs ShroudForge 1.8; Kiln runs nothing of 1.8 before a final 1.8 release (the "
                        "kiln-sfi.sf18 veto)");
    Decision ok = chosen(KILN_LAUNCH_BACKEND_KILN, KILN_LAUNCH_REASON_PLAN,
                         "kiln.json shroudforge.backend: \"kiln\", with " + name + " that the profile names");
    ok.plan_read = true;
    ok.plan_hash = d.plan_hash;
    ok.plan_id = ref.uuid;
    return ok;
}

// ---- the frozen record ----
struct Record {
    uint32_t backend = KILN_LAUNCH_BACKEND_NONE;
    uint32_t reason_code = 0;
    uint64_t launch_id = 0, freeze_stamp = 0;
    uint8_t plan_hash[32] = {};
    char reason[KILN_LAUNCH_REASON_BYTES] = {};
    uint32_t native_policy = KILN_LAUNCH_NATIVE_POLICY_NEVER;
    uint32_t build_verified = 0;
    const std::string* notice = nullptr;  // never freed: a reader may hold it at any time, the process's exit included
};
Record g_record;
// 0 open, 1 one caller is freezing, 2 frozen. The record is written before 2 is stored (release) and read only after
// 2 is seen (acquire): readers never lock, never wait and never see a half-written record.
std::atomic<int> g_state{0};

bool is_frozen() { return g_state.load(std::memory_order_acquire) == 2; }

// The record is size-versioned (kiln_launch.h): the first 192 bytes for any caller of at least that size, native_policy
// and build_verified only for a caller of the whole 200. A caller built before native_policy owns exactly 192 bytes, so
// nothing past them is ever written for it.
KilnResult KILN_CALL api_status(KilnLaunchStatus* out) {
    if (!out || out->size < KILN_LAUNCH_STATUS_V1_SIZE) return KILN_ERR_INVALID_ARGUMENT;
    if (!is_frozen()) {
        out->frozen = 0;
        return KILN_ERR_UNAVAILABLE;
    }
    const bool whole = out->size >= sizeof(KilnLaunchStatus);
    out->size = whole ? static_cast<uint32_t>(sizeof(KilnLaunchStatus)) : KILN_LAUNCH_STATUS_V1_SIZE;
    out->backend = g_record.backend;
    out->frozen = 1;
    out->reason_code = g_record.reason_code;
    out->launch_id = g_record.launch_id;
    std::memcpy(out->plan_hash, g_record.plan_hash, sizeof out->plan_hash);
    out->freeze_stamp = g_record.freeze_stamp;
    std::memcpy(out->reason, g_record.reason, sizeof out->reason);
    if (whole) {
        out->native_policy = g_record.native_policy;
        out->build_verified = g_record.build_verified;
    }
    return KILN_OK;
}

KilnResult KILN_CALL api_notice_copy(char* buffer, size_t capacity, size_t* out_len) {
    if (!is_frozen()) return KILN_ERR_UNAVAILABLE;
    const std::string& notice = *g_record.notice;
    if (out_len) *out_len = notice.size();
    if (notice.empty()) return KILN_OK;
    if (!buffer || capacity < notice.size()) return KILN_ERR_BUFFER_TOO_SMALL;
    std::memcpy(buffer, notice.data(), notice.size());
    return KILN_OK;
}

const KilnLaunchApi g_table = {{sizeof(KilnLaunchApi), KILN_LAUNCH_VERSION}, &api_status, &api_notice_copy};

// ---- the ShroudForge-dialect owner (launch_internal.h) ----
// 0 until designate() sets it, once (compare-exchange from 0); never written again.
std::atomic<KilnOwner> g_dialect_owner{0};

KilnOwner KILN_CALL api_dialect_owner() { return g_dialect_owner.load(std::memory_order_acquire); }

const KilnLaunchInternalApi g_internal = {{sizeof(KilnLaunchInternalApi), KILN_LAUNCH_INTERNAL_VERSION}, &api_dialect_owner};

const char* reason_class(uint32_t code) {
    return KILN_LAUNCH_REASON_REFUSED(code) ? "refused" : KILN_LAUNCH_REASON_IDLE(code) ? "idle" : "selected";
}

}  // namespace

void set_idle(Inputs& in, std::string_view safe_idle, uint32_t profile_mode, bool plugins_on) {
    in.idle_code = 0;
    in.idle_text.clear();
    if (!safe_idle.empty()) {
        in.idle_code = KILN_LAUNCH_REASON_SAFE_IDLE;
        in.idle_text = "safe idle (" + std::string(safe_idle) + "): Kiln runs no plugin this start";
    } else if (profile_mode == KILN_PROFILE_INVALID) {
        in.idle_code = KILN_LAUNCH_REASON_PROFILE_INVALID;
        in.idle_text = "the profile is invalid: no mods load this start";
    } else if (profile_mode == KILN_PROFILE_NO_MODS) {
        in.idle_code = KILN_LAUNCH_REASON_NO_MODS;
        in.idle_text = "play-once no-mods: no mods load this start";
    } else if (!plugins_on) {
        in.idle_code = KILN_LAUNCH_REASON_PLUGINS_OFF;
        in.idle_text = "kiln.json \"plugins\": false: Kiln loads no plugin this start";
    }
}

Hold interpreter_hold(bool gate_held, std::string_view gate_text) {
    // The client gate alone: its row in native/core/gates/sfi.json while the interpreter is a preview, which kiln.json
    // "preview" lifts like any gate, and the release policy's row on top when the list holds it on one side only
    // (sfi_release_policy.h, gates.cpp). A list with no such row holds nothing: the interpreter is a normal choice.
    if (gate_held) return {true, std::string(gate_text)};
    return {false, {}};
}

bool eml_r_proxy(std::string bytes) {
    const auto* data = reinterpret_cast<const uint8_t*>(bytes.data());
    const std::string_view view(bytes);
    uint32_t pe_offset = 0;
    if (!view.starts_with("MZ") || !pe::detail::read(data, view.size(), 0x3C, &pe_offset) ||
        pe_offset < 0x40 || pe_offset > view.size() || view.size() - pe_offset < 26 ||
        view.substr(pe_offset, 4) != std::string_view("PE\0\0", 4))
        return false;
    const size_t coff = static_cast<size_t>(pe_offset) + 4;
    uint16_t machine = 0, characteristics = 0, magic = 0, optional_size = 0;
    pe::detail::read(data, bytes.size(), coff, &machine);
    pe::detail::read(data, bytes.size(), coff + 16, &optional_size);
    pe::detail::read(data, bytes.size(), coff + 18, &characteristics);
    pe::detail::read(data, bytes.size(), coff + 20, &magic);
    if (machine != 0x8664 || magic != 0x020B || !(characteristics & 0x2000) ||
        optional_size < 112 || optional_size > view.size() - pe_offset - 24 ||
        view.substr(0x40).starts_with("Wine builtin DLL"))
        return false;

    for (char& c : bytes)
        if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
    for (const std::string_view marker : {"eml-r.json", "eml_r.dll"}) {
        if (bytes.find(marker) != std::string::npos) return true;
        std::string wide;
        wide.reserve(marker.size() * 2);
        for (char c : marker) {
            wide.push_back(c);
            wide.push_back('\0');
        }
        if (bytes.find(wide) != std::string::npos) return true;
    }
    return false;
}

namespace {
Decision decide_inputs(const Inputs& in) {
    if (in.idle_code) {
        Decision d;
        d.reason_code = in.idle_code;
        d.reason = in.idle_text;
        return d;
    }
    const Setting s = read_setting(in.kiln_json);
    if (s.kind == Setting::Invalid) return refused(s.code, s.text);
    if (s.kind != Setting::Kiln) {
        // KILN_SHROUDFORGE_RUNTIME chooses between hosted and installed, over kiln.json, as kiln-shroudforge reads it
        // (plugin.cpp prefer_installed): "installed" is installed, any other value the bundled runtime.
        if (in.env_runtime) {
            const bool installed = *in.env_runtime == "installed";
            return chosen(installed ? KILN_LAUNCH_BACKEND_INSTALLED : KILN_LAUNCH_BACKEND_HOSTED,
                          KILN_LAUNCH_REASON_ENV_RUNTIME,
                          "KILN_SHROUDFORGE_RUNTIME=" + *in.env_runtime + ": " + (installed ? "installed" : "hosted") +
                              ", over " + s.text);
        }
        return chosen(s.kind == Setting::Installed ? KILN_LAUNCH_BACKEND_INSTALLED : KILN_LAUNCH_BACKEND_HOSTED, s.code,
                      s.text);
    }
    if (in.interpreter_held)
        return refused(KILN_LAUNCH_REASON_INTERPRETER_HELD,
                       "kiln.json asks for Kiln's interpreter (shroudforge.backend: \"kiln\"), but the capability " +
                           std::string(kInterpreterGate) + " is held (" + in.held_text + ")");
    // An earlier activation may still have a valid plan. Stand aside before reading it or exposing it to kiln-eml.
    if (in.eml_r_present && in.eml_r_present()) {
        Decision d;
        d.reason_code = KILN_LAUNCH_REASON_EML_R_LOADER;
        d.reason = "EML-R's own loader is in the game folder. It may run these mods itself, so Kiln's interpreter stands aside.";
        d.notice = d.reason;
        return d;
    }
    const Reference ref = read_reference(in.profile_text);
    if (ref.state == Reference::Absent)
        return refused(KILN_LAUNCH_REASON_PLAN_MISSING, "kiln.json asks for Kiln's interpreter, but " + ref.why +
                                                            "; activate the profile in the launcher");
    if (ref.state == Reference::Bad) return refused(KILN_LAUNCH_REASON_PLAN_INVALID, ref.why);
    const std::string file = ref.uuid + ".json";
    const PlanFile plan = in.read_plan ? in.read_plan(file) : PlanFile{PlanFile::Missing, {}, "no reader"};
    const std::string named = "the profile names " + std::string(kPlanFolder) + "/" + file;
    if (plan.state == PlanFile::Missing)
        return refused(KILN_LAUNCH_REASON_PLAN_MISSING,
                       named + ", which is not there (" + plan.error + "); activate the profile in the launcher again");
    if (plan.state == PlanFile::Unreadable)
        return refused(KILN_LAUNCH_REASON_PLAN_INVALID, named + ", which cannot be read (" + plan.error + ")");
    // Every member is type-checked before use; a JSON exception that slips through anyway refuses the plan instead of
    // leaving decide() (boot.cpp's caller would end Kiln's whole boot).
    try {
        return check_plan(in, ref, plan.bytes, Decision{});
    } catch (const std::exception& e) {
        return refused(KILN_LAUNCH_REASON_PLAN_INVALID, named + ", which could not be checked (" + e.what() + ")");
    }
}
}  // namespace

Decision decide(const Inputs& in) {
    Decision d = decide_inputs(in);
    d.reason = utf8_clean(d.reason);
    d.notice = utf8_clean(d.notice);
    return d;
}

Decision undecided(std::string_view what) {
    Decision d;
    d.reason_code = KILN_LAUNCH_REASON_UNDECIDED;
    d.reason = utf8_clean("Kiln could not decide the ShroudForge backend (" + cut_utf8(what, 256) +
                          "): each loader keeps its rules from before kiln.launch");
    return d;
}

const char* backend_name(uint32_t backend) {
    switch (backend) {
        case KILN_LAUNCH_BACKEND_HOSTED: return "hosted";
        case KILN_LAUNCH_BACKEND_INSTALLED: return "installed";
        case KILN_LAUNCH_BACKEND_KILN: return "kiln";
        default: return "none";
    }
}

const char* native_policy_name(uint32_t policy) {
    switch (policy) {
        case KILN_LAUNCH_NATIVE_POLICY_ALWAYS: return "always";
        case KILN_LAUNCH_NATIVE_POLICY_VERIFIED_ONLY: return "verified-only";
        case KILN_LAUNCH_NATIVE_POLICY_NEVER: return "never";
        default: return "unknown";
    }
}

bool freeze(const Decision& d, uint64_t launch_id, uint64_t freeze_stamp, const std::function<void()>& published) {
    if (launch_id == 0) return false;
    // Allocated before the record is claimed: nothing below can throw, so a claimed record is always served.
    auto notice = std::make_unique<std::string>(d.notice);
    int open = 0;
    if (!g_state.compare_exchange_strong(open, 1, std::memory_order_acq_rel)) return false;
    g_record.backend = d.backend;
    g_record.reason_code = d.reason_code;
    g_record.launch_id = launch_id;
    g_record.freeze_stamp = freeze_stamp;
    g_record.native_policy = d.native_policy;
    // The build's answer rides along for a backend that runs; backend none says 0 whatever the boot answered
    g_record.build_verified = d.backend != KILN_LAUNCH_BACKEND_NONE && d.build_verified ? 1u : 0u;
    if (d.plan_read) std::memcpy(g_record.plan_hash, d.plan_hash.data(), sizeof g_record.plan_hash);
    // The reason, cut to the field (NUL included) before a character that does not fit whole.
    size_t n = std::min(d.reason.size(), sizeof g_record.reason - 1);
    while (n > 0 && n < d.reason.size() && (static_cast<unsigned char>(d.reason[n]) & 0xC0) == 0x80) --n;
    std::memcpy(g_record.reason, d.reason.data(), n);
    g_record.notice = notice.release();
    g_state.store(2, std::memory_order_release);
    // After the record is served: nothing in the process waits for the disk, and only this freeze ever gets here. The
    // record is frozen whatever `published` does: an exception from it (launch.json's text or write) never leaves the
    // freeze, so it can never end Kiln's boot (kiln_boot's catch would).
    if (published) {
        try {
            published();
        } catch (...) {
        }
    }
    return true;
}

bool frozen() { return is_frozen(); }

uint32_t backend() { return is_frozen() ? g_record.backend : KILN_LAUNCH_BACKEND_NONE; }

const KilnLaunchApi* table() { return &g_table; }

std::string state_json(const Decision& d, uint64_t launch_id, uint64_t freeze_stamp, uint32_t pid, std::string_view kiln) {
    char id[17];
    std::snprintf(id, sizeof id, "%016llx", static_cast<unsigned long long>(launch_id));
    nlohmann::ordered_json doc;
    doc["format"] = 1;
    doc["kiln"] = std::string(kiln);
    doc["pid"] = pid;
    doc["launchId"] = id;  // hex: a 64-bit id does not fit a JSON reader's double
    doc["frozen"] = true;
    doc["backend"] = backend_name(d.backend);
    doc["reasonCode"] = d.reason_code;
    doc["reasonClass"] = reason_class(d.reason_code);
    doc["reason"] = d.reason;  // the whole text; the in-process record cuts it to 127 bytes
    doc["notice"] = d.notice;
    doc["planSha256"] = d.plan_read ? json(boot_gd::hex_of(d.plan_hash)) : json(nullptr);
    doc["planId"] = d.plan_id.empty() ? json(nullptr) : json(d.plan_id);
    doc["freezeStamp"] = freeze_stamp;
    doc["nativePolicy"] = native_policy_name(d.native_policy);  // kiln.json native.policy, as the record says it
    // `replace`: a reason that is not UTF-8 after all is written with U+FFFD, never thrown out of the freeze
    return doc.dump(2, ' ', false, nlohmann::ordered_json::error_handler_t::replace) + "\n";
}

bool write_retrying(const std::function<bool()>& write, const std::function<void(uint32_t)>& sleep, int attempts,
                    uint32_t wait_ms) {
    for (int i = 0; i < attempts; ++i) {
        if (write()) return true;
        if (i + 1 < attempts && sleep) sleep(wait_ms);
    }
    return false;
}

bool is_dialect_dll(const std::filesystem::path& file, const std::filesystem::path& plugins_dir) {
    const auto fold = [](std::wstring s) {
        for (wchar_t& c : s)
            if (c >= L'A' && c <= L'Z') c = static_cast<wchar_t>(c - L'A' + L'a');
        return s;
    };
    const std::filesystem::path f = file.lexically_normal(), dir = plugins_dir.lexically_normal();
    return fold(f.filename().wstring()) == L"kiln-sfi.dll" && fold(f.parent_path().wstring()) == fold(dir.wstring());
}

bool wants_dialect_owner() {
    return backend() == KILN_LAUNCH_BACKEND_KILN && g_dialect_owner.load(std::memory_order_acquire) == 0;
}

bool designate(KilnOwner owner, bool stamp_trusted, std::string& why) {
    if (!is_frozen()) {
        why = "the ShroudForge backend is not frozen yet";
        return false;
    }
    if (g_record.backend != KILN_LAUNCH_BACKEND_KILN) {
        why = std::string("the frozen backend is ") + backend_name(g_record.backend) + ", not kiln";
        return false;
    }
    if (owner == 0) {
        why = "owner 0 is the core";
        return false;
    }
    if (!stamp_trusted) {
        why = "its build stamp is not this core's (a stale or foreign kiln-sfi.dll)";
        return false;
    }
    KilnOwner none = 0;
    if (!g_dialect_owner.compare_exchange_strong(none, owner, std::memory_order_acq_rel)) {
        why = "owner " + std::to_string(none) + " is designated already (never twice, never moved)";
        return false;
    }
    return true;
}

KilnOwner dialect_owner() { return api_dialect_owner(); }

const KilnLaunchInternalApi* internal_table() { return &g_internal; }

void reset_for_tests() {
    delete g_record.notice;
    g_record = Record{};
    g_dialect_owner.store(0, std::memory_order_release);
    g_state.store(0, std::memory_order_release);
}

}  // namespace kiln::launch
