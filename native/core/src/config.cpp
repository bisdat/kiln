// SPDX-License-Identifier: MIT
#include "config.h"

#include <fstream>

#include "log.h"
#include "nlohmann/json.hpp"

namespace kiln {
namespace {
Config g_config;
nlohmann::json g_doc;  // kiln.json as read, for apply_side

int parse_level(const std::string& s) {
    if (s == "error") return KILN_LOG_ERROR;
    if (s == "warn" || s == "warning") return KILN_LOG_WARN;
    if (s == "debug") return KILN_LOG_DEBUG;
    if (s == "trace") return KILN_LOG_TRACE;
    return KILN_LOG_INFO;
}
// "native": {"policy": ...}; an unknown value keeps the current one (logged).
void read_native(const nlohmann::json& sec, const char* where) {
    if (!sec.contains("native") || !sec["native"].is_object()) return;
    const auto& n = sec["native"];
    if (!n.contains("policy")) return;
    const std::string p = n["policy"].is_string() ? n["policy"].get<std::string>() : "";
    if (p == "always") g_config.native_policy = NativePolicy::Always;
    else if (p == "verified-only") g_config.native_policy = NativePolicy::VerifiedOnly;
    else if (p == "never") g_config.native_policy = NativePolicy::Never;
    else KLOG_W("config: %s native.policy '%s' is not always, verified-only or never; ignored", where, p.c_str());
}
// "preview": true | "all" | [names]. Present in a section, it replaces what an earlier section said.
void read_preview(const nlohmann::json& sec, const char* where) {
    if (!sec.contains("preview")) return;
    const auto& p = sec["preview"];
    g_config.preview_all = false;
    g_config.preview.clear();
    if (p.is_boolean()) {
        g_config.preview_all = p.get<bool>();
    } else if (p.is_string() && p.get<std::string>() == "all") {
        g_config.preview_all = true;
    } else if (p.is_array()) {
        for (const auto& n : p) {
            if (n.is_string()) g_config.preview.push_back(n.get<std::string>());
            else KLOG_W("config: %s \"preview\" holds an entry that is not a capability name; ignored", where);
        }
    } else {
        KLOG_W("config: %s \"preview\" is not true, false or a list of capability names; ignored", where);
    }
}
// "developer_debugging": true | false. Present in a section, it replaces what an earlier section said.
void read_debugging(const nlohmann::json& sec, const char* where) {
    if (!sec.contains("developer_debugging")) return;
    if (sec["developer_debugging"].is_boolean()) g_config.developer_debugging = sec["developer_debugging"].get<bool>();
    else KLOG_W("config: %s \"developer_debugging\" is not true or false; ignored", where);
}
// "boot": {"testRefuse" | "testFault": {"<mod>": "<collector>"}}: kiln.boot honours them only with policy any (labs).
void read_boot(const nlohmann::json& doc) {
    if (!doc.contains("boot") || !doc["boot"].is_object()) return;
    for (const auto& [key, into] : {std::pair{"testRefuse", &g_config.boot_test_refuse}, std::pair{"testFault", &g_config.boot_test_fault}}) {
        if (!doc["boot"].contains(key)) continue;
        const auto& t = doc["boot"][key];
        if (!t.is_object()) {
            KLOG_W("config: kiln.json boot.%s is not an object of mod id -> collector; ignored", key);
            continue;
        }
        for (const auto& [mod, collector] : t.items())
            if (collector.is_string()) (*into)[mod] = collector.get<std::string>();
            else KLOG_W("config: kiln.json boot.%s.%s is not a collector name; ignored", key, mod.c_str());
    }
}
// "tooltipProbe": true | false | "<key>" (the client session's tooltip probe; a side section replaces it).
void read_tooltip_probe(const nlohmann::json& sec, const char* where) {
    if (!sec.contains("tooltipProbe")) return;
    const auto& p = sec["tooltipProbe"];
    if (p.is_boolean()) g_config.tooltip_probe = p.get<bool>() ? "default" : "";
    else if (p.is_string()) g_config.tooltip_probe = p.get<std::string>();
    else KLOG_W("config: %s \"tooltipProbe\" is not true, false or a key name; ignored", where);
}
// "tasks": {...}: the pool's global knobs (config.h TasksConfig). Present in a section, each valid key replaces the
// value before it; a wrong type or a value out of its bound gives the default and one warning naming the key and bound.
void read_tasks(const nlohmann::json& sec, const char* where) {
    if (!sec.contains("tasks")) return;
    const auto& t = sec["tasks"];
    const TasksConfig defaults;
    if (!t.is_object()) {
        KLOG_W("config: %s \"tasks\" is not an object; the task pool keeps its defaults", where);
        g_config.tasks = defaults;
        return;
    }
    struct Knob {
        const char* key;
        uint32_t TasksConfig::*field;
        uint32_t lo, hi;
    };
    static constexpr Knob kKnobs[] = {{"workers", &TasksConfig::workers, 1, 8},
                                      {"long_running_global", &TasksConfig::long_running_global, 1, 16},
                                      {"watchdog_running_s", &TasksConfig::watchdog_running_s, 5, 600},
                                      {"watchdog_queued_s", &TasksConfig::watchdog_queued_s, 5, 600}};
    for (const Knob& k : kKnobs) {
        if (!t.contains(k.key)) continue;
        const auto& v = t[k.key];
        if (v.is_number_integer() && v.get<int64_t>() >= k.lo && v.get<int64_t>() <= k.hi) {
            g_config.tasks.*k.field = static_cast<uint32_t>(v.get<int64_t>());
        } else {
            g_config.tasks.*k.field = defaults.*k.field;
            KLOG_W("config: %s tasks.%s is not a whole number in %u..%u; the default is used", where, k.key, k.lo, k.hi);
        }
    }
}
}  // namespace

const Config& config() { return g_config; }

const char* native_policy_name(NativePolicy p) {
    return p == NativePolicy::Never ? "never" : p == NativePolicy::VerifiedOnly ? "verified-only" : "always";
}

void load_config() {
    const auto file = paths().kiln / L"kiln.json";
    std::ifstream in(file);
    if (!in) {
        KLOG_I("config: no kiln.json, defaults (policy verified-only)");
        return;
    }
    try {
        const auto doc = nlohmann::json::parse(in, nullptr, true, true);
        g_doc = doc;
        g_config.policy_any = doc.value("policy", "verified-only") == "any";
        g_config.clr = doc.value("clr", true);
        g_config.plugins = doc.value("plugins", true);
        g_config.log_level = parse_level(doc.value("log_level", "info"));
        log::set_level(g_config.log_level);
        read_native(doc, "kiln.json");
        read_preview(doc, "kiln.json");
        read_boot(doc);
        read_tasks(doc, "kiln.json");
        read_tooltip_probe(doc, "kiln.json");
        read_debugging(doc, "kiln.json");
        KLOG_I("config: policy=%s clr=%d plugins=%d native.policy=%s", g_config.policy_any ? "any" : "verified-only",
               g_config.clr, g_config.plugins, native_policy_name(g_config.native_policy));
    } catch (const std::exception& e) {
        KLOG_E("config: kiln.json unreadable (%s); using defaults", e.what());
    }
}

void apply_side(uint32_t side) {
    const char* name = side == KILN_SIDE_CLIENT ? "client" : "server";
    if (!g_doc.is_object() || !g_doc.contains(name) || !g_doc[name].is_object()) return;
    try {
        const auto& sec = g_doc[name];
        // Apply the gate override even if another side option has the wrong type. In particular, preview:false
        // must not inherit a top-level preview:true because e.g. clr could not be read.
        read_preview(sec, name);
        read_debugging(sec, name);
        read_tooltip_probe(sec, name);
        if (sec.contains("policy")) g_config.policy_any = sec.value("policy", "verified-only") == "any";
        g_config.clr = sec.value("clr", g_config.clr);
        g_config.plugins = sec.value("plugins", g_config.plugins);
        if (sec.contains("log_level")) {
            g_config.log_level = parse_level(sec.value("log_level", "info"));
            log::set_level(g_config.log_level);
        }
        read_native(sec, name);
        read_tasks(sec, name);
        KLOG_I("config: \"%s\" section: policy=%s clr=%d plugins=%d native.policy=%s", name,
               g_config.policy_any ? "any" : "verified-only", g_config.clr, g_config.plugins,
               native_policy_name(g_config.native_policy));
    } catch (const std::exception& e) {
        KLOG_E("config: kiln.json \"%s\" section partly unreadable (%s); readable values stay", name, e.what());
    }
}

bool build_verified(uint32_t side, const std::string& sha256, std::string* note) {
    const auto file = paths().sigdb / L"builds" / to_wide(side == KILN_SIDE_CLIENT ? "client" : "server") /
                      to_wide(sha256 + ".json");
    std::ifstream in(file);
    if (!in) {
        if (note) *note = "no build record";
        return false;
    }
    try {
        const auto doc = nlohmann::json::parse(in, nullptr, true, true);
        if (doc.value("sha256", "") != sha256) {
            if (note) *note = "build record sha256 mismatch";
            return false;
        }
        if (doc.contains("side") && doc.value("side", "") != (side == KILN_SIDE_CLIENT ? "client" : "server")) {
            if (note) *note = "build record is for the other side";
            return false;
        }
        const bool ok = doc.value("verified", false);
        if (note) *note = ok ? doc.value("note", "verified") : "build record not verified";
        return ok;
    } catch (const std::exception& e) {
        if (note) *note = std::string("build record unreadable: ") + e.what();
        return false;
    }
}

}  // namespace kiln
