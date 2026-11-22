// SPDX-License-Identifier: MIT
// Overlap report and compatibility rules inside kiln-core (compat.h).
#include "compat.h"

#include <algorithm>
#include <cctype>
#include <mutex>
#include <set>

#include "api.h"
#include "chain_status.h"
#include "common.h"
#include "log.h"
#include "native_mods.h"
#include "profile.h"
#include "sigdb.h"
#include "services/hooks_internal.h"

#include <psapi.h>  // after windows.h (common.h)

namespace kiln::compat {
namespace {

namespace fs = std::filesystem;

struct FileInfo {
    std::string path, source, error;
    size_t rules = 0;
};

std::mutex g_mu;
std::vector<Rule> g_rules;                         // shipped, then the player's
std::vector<FileInfo> g_files;                     // the rule files that exist
std::vector<std::pair<std::string, std::string>> g_early;  // rule id -> what apply_early() did
std::set<std::string> g_rule_off;                  // "<engine>:<id>" switched off by a rule: still part of this start for the rules
std::set<std::string> g_logged;                    // lines logged once
std::set<std::string> g_prev_late;                 // `late` rules the last session's hooks.json reports as applied
bool g_inited = false;

std::string file_name_of(const std::string& path) {
    const size_t at = path.find_last_of("\\/");
    return at == std::string::npos ? path : path.substr(at + 1);
}

// EMF's own loader, loaded by the proxy: it exports one of the names the proxy recognises it by (dinput8.cpp classify).
bool is_emf_loader(const KilnChainEntry& e) {
    if (e.source != KILN_CHAIN_SOURCE_CHAIN || !e.module || (e.status != KILN_CHAIN_LOADED && e.status != KILN_CHAIN_DUPLICATE))
        return false;
    const HMODULE m = static_cast<HMODULE>(e.module);
    return GetProcAddress(m, "emf_loader_identity") || GetProcAddress(m, "emf_loader_play_once");
}

// The engine word of a loaded chain DLL that its exports name, for `before` rules (dev.29): Ultimate ASI Loader, by the
// export the launcher knows it by too (crates/kiln-manage/src/engines.rs), whatever the file is called. Empty otherwise.
std::string engine_of(const KilnChainEntry& e, const std::string& path) {
    HMODULE m = static_cast<HMODULE>(e.module);
    if (!m) m = GetModuleHandleW(to_wide(path).c_str());  // a DLL the late phase loaded
    if (m && GetProcAddress(m, "IsUltimateASILoader")) return "ultimate-asi-loader";
    return {};
}

// The proxy's chain entries, a late one with the status the late phase gave it (still "late" while it has not run).
std::vector<ChainRow> chain_rows() {
    std::vector<ChainRow> rows;
    const KilnBootInfo& bi = boot_info();
    const auto late = native_mods::late_rows();
    bool emf_chained = false;
    for (uint32_t i = 0; i < bi.chain_count; ++i) {
        const KilnChainEntry& e = bi.chain[i];
        ChainRow r;
        r.path = std::string(sv(e.path));
        r.file = file_name_of(r.path);
        r.source = e.source == KILN_CHAIN_SOURCE_EMF ? "emf" : e.source == KILN_CHAIN_SOURCE_PROFILE ? "profile" : "chain";
        const std::string status = chain_final_status(e, i, late, "late");
        r.status = chain_reported_status(status);
        r.late = e.status == KILN_CHAIN_LATE;
        r.late_ignored = status == kLateAlreadyLoaded;
        r.loader = is_emf_loader(e);
        emf_chained = emf_chained || r.loader;
        if (r.on()) r.engine = engine_of(e, r.path);
        if (e.source == KILN_CHAIN_SOURCE_PROFILE) {
            const std::string id = native_mods::dllmain_mod_of(i);
            if (!id.empty()) r.mod = "kiln:" + id;
        }
        rows.push_back(std::move(r));
    }
    // EMF's loader (chained) reads dinput8-chain.ini and starts those DLLs itself
    if (emf_chained)
        for (ChainRow& r : rows)
            if (r.source == "emf") r.started_by_loader = true;
    return rows;
}

std::vector<std::string> loaded_module_names() {
    std::vector<std::string> out;
    HMODULE mods[2048];
    DWORD needed = 0;
    if (!K32EnumProcessModules(GetCurrentProcess(), mods, sizeof mods, &needed)) return out;
    const size_t n = std::min<size_t>(needed / sizeof(HMODULE), 2048);
    for (size_t i = 0; i < n; ++i) {
        wchar_t path[2048];
        const DWORD len = GetModuleFileNameW(mods[i], path, 2048);
        if (len == 0 || len >= 2048) continue;
        out.push_back(file_name_of(to_utf8(std::wstring(path, len))));
    }
    return out;
}

void add_unique(std::vector<std::string>& v, const std::string& s) {
    if (!s.empty() && std::find(v.begin(), v.end(), s) == v.end()) v.push_back(s);
}

// Live Map (github.com/anthony-spruyt/enshrouded-livemap, a stand-alone server map): an iphlpapi.dll proxy in the game
// folder. IPHLPAPI is a Windows DLL name, so the name proves nothing: Live Map is the one whose loaded image holds the name
// of its config file, livemap.ini (its source reads L"\\livemap.ini"; the launcher's engine table knows it by the same
// text). The running image is read, not the file: a Wine builtin under that name reports the game-folder path without
// running the file there. Read once per module base, readable committed pages only, with a reference held so that the
// module cannot unload (another thread's FreeLibrary) while its pages are read.
bool livemap_loaded() {
    static std::mutex mu;
    static std::vector<std::pair<HMODULE, bool>> seen;
    HMODULE h = nullptr;
    if (!GetModuleHandleExW(0, L"iphlpapi.dll", &h) || !h) return false;
    struct Release {
        HMODULE m;
        ~Release() { FreeLibrary(m); }
    } release{h};
    std::lock_guard lock(mu);
    for (const auto& s : seen)
        if (s.first == h) return s.second;
    bool found = false;
    wchar_t path[2048];
    const DWORD len = GetModuleFileNameW(h, path, 2048);
    std::error_code ec;
    if (len && len < 2048 && fs::equivalent(fs::path(std::wstring(path, len)).parent_path(), paths().game, ec)) {
        MODULEINFO mi{};
        if (K32GetModuleInformation(GetCurrentProcess(), h, &mi, sizeof mi) && mi.SizeOfImage <= 256u * 1024 * 1024) {
            const auto* base = static_cast<const char*>(mi.lpBaseOfDll);
            const char* end = base + mi.SizeOfImage;
            for (const char* p = base; p < end && !found;) {
                MEMORY_BASIC_INFORMATION m{};
                if (!VirtualQuery(p, &m, sizeof m) || m.RegionSize == 0) break;
                const char* next = static_cast<const char*>(m.BaseAddress) + m.RegionSize;
                const DWORD prot = m.Protect & 0xff;
                const bool readable = m.State == MEM_COMMIT && !(m.Protect & PAGE_GUARD) && prot != PAGE_NOACCESS && prot != PAGE_EXECUTE;
                if (readable) {
                    const char* from = std::max(p, static_cast<const char*>(m.BaseAddress));
                    found = contains_text(std::string_view(from, static_cast<size_t>(std::min(next, end) - from)), "livemap.ini");
                }
                p = next;
            }
        }
    }
    seen.emplace_back(h, found);
    return found;
}

bool ieq(const std::string& a, const char* b) { return glob_match(b, a); }  // no wildcard in b: a case-insensitive compare

// What is present now (compat_rules.h Facts).
Facts gather_facts(const std::vector<ChainRow>& rows, const std::vector<std::string>& symbols, bool sf_hosted, bool include_disabled = true) {
    Facts f;
    f.modules = loaded_module_names();
    for (const ChainRow& r : rows)
        if (r.on()) add_unique(f.modules, r.file);
    for (const ChainRow& r : rows)  // an engine a chain DLL's exports name (the launcher classifies its chain DLLs too)
        if (r.on() && !r.engine.empty()) add_unique(f.engines, r.engine);
    f.symbols = symbols;
    f.engines.push_back("kiln");
    if (sf_hosted) add_unique(f.engines, "shroudforge");
    for (const std::string& m : f.modules) {  // a few loaders every player knows by name
        if (ieq(m, "shroudforge-runtime.dll")) add_unique(f.engines, "shroudforge");
        if (ieq(m, "emf_core.dll")) add_unique(f.engines, "emf");
        if (ieq(m, "enshwick.dll")) add_unique(f.engines, "enshwick");
        if (ieq(m, "iphlpapi.dll") && livemap_loaded()) add_unique(f.engines, "livemap");
    }
    const auto& prof = profile::state();
    if (prof.mode == KILN_PROFILE_ACTIVE || prof.mode == KILN_PROFILE_ONCE) {
        for (uint32_t k = 0; k < KILN_PROFILE_KINDS; ++k) {
            for (const auto& e : prof.mods[k]) {
                if (!e.enabled || e.id.empty()) continue;
                const std::string engine = k == KILN_PROFILE_KIND_EMF ? "emf" : "kiln";
                add_unique(f.mods, engine + ":" + e.id);
                if (k == KILN_PROFILE_KIND_EMF) add_unique(f.engines, "emf");
            }
        }
        for (uint32_t i = 0; i < profile::kEngineKindCount; ++i) {
            for (const auto& e : prof.engine_mods[i]) {
                if (!e.enabled) continue;
                add_unique(f.mods, std::string(profile::kEngineKinds[i]) + ":" + e.id);
                add_unique(f.engines, profile::kEngineKinds[i]);
            }
        }
    }
    if (include_disabled) for (const std::string& m : g_rule_off) add_unique(f.mods, m);
    return f;
}

std::string read_state_file(const wchar_t* name) {
    std::string text;
    read_file(paths().state / name, &text);
    return text;
}

bool log_once(const std::string& key) { return g_logged.insert(key).second; }

ojson matcher_json(const Matcher& m) {
    ojson o;
    o[matcher_key(m.kind)] = m.value;
    return o;
}

// A `late` rule that was applied last session stays in force while its subject still loads late, whether or not its
// `when` still holds: a rule on {"symbol": ...} that moved the DLL late may have removed the overlap it fired on, and
// would otherwise switch itself off and put the DLL back to the proxy at the next start (the overlap would return).
bool sticky_late(const Rule& r, const std::vector<ChainRow>& rows) {
    if (r.action != "late" || !g_prev_late.count(r.id) || r.subject.kind != Matcher::Kind::Module) return false;
    bool any = false;
    for (const ChainRow& row : rows) {
        if (!glob_match(r.subject.value, row.file) || !row.on()) continue;
        if (!row.late || row.late_ignored) return false;
        any = true;
    }
    return any;
}

// The "rules" section: the rules that fire in this start, with what became of them. Logs each once.
ojson rules_report(const std::vector<ChainRow>& rows, const Facts& facts) {
    ojson out = ojson::array();
    std::set<const Rule*> fired;
    for (const Rule* r : evaluate(g_rules, facts)) fired.insert(r);
    RuleContext ctx;
    ctx.early = g_early;
    ctx.rule_off = g_rule_off;
    for (const Rule& r : g_rules) {  // a mod a rule switched off whose DLL the proxy started before the rule could be read
        const bool early = std::any_of(g_early.begin(), g_early.end(), [&](const auto& e) { return e.first == r.id; });
        if (!early || r.subject.kind != Matcher::Kind::Mod) continue;
        const std::string& ref = r.subject.value;
        std::string engine = ref.substr(0, ref.find(':'));
        for (char& c : engine) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if ((engine == "kiln" || engine == "native" || engine == "plugin") && native_mods::loaded_by_proxy(ref.substr(ref.find(':') + 1)))
            ctx.proxy_loaded.insert(r.id);
    }
    for (const Rule& r : g_rules) {
        if (r.action == "original-code") continue; // per-mod import results belong to the host log
        const bool early = std::any_of(g_early.begin(), g_early.end(), [&](const auto& e) { return e.first == r.id; });
        if (!fired.count(&r) && !early && !sticky_late(r, rows)) continue;
        const Outcome oc = outcome_of(r, rows, facts, ctx);
        ojson o;
        o["id"] = r.id;
        o["action"] = r.action;
        o["applied"] = oc.applied;
        o["message"] = r.message;
        o["subject"] = r.has_subject ? matcher_json(r.subject) : ojson(nullptr);
        if (r.has_target) o["target"] = matcher_json(r.target);  // dev.29, `before` only
        o["source"] = r.source;
        if (!oc.note.empty()) o["note"] = oc.note;
        out.push_back(std::move(o));
        if (r.action == "warn") {
            if (log_once("rule|" + r.id)) KLOG_W("compat: rule %s: %s", r.id.c_str(), r.message.c_str());
        } else if (!oc.applied) {
            if (log_once("rule|" + r.id + "|" + oc.note)) KLOG_W("compat: %s (%s)", oc.note.c_str(), r.message.c_str());
        } else if (early && log_once("rule|" + r.id + "|off")) {  // switched off in the profile (apply_early), its DLL is not running
            KLOG_W("compat: rule %s switched %s off: %s", r.id.c_str(), r.subject.value.c_str(), r.message.c_str());
        }
    }
    return out;
}

ojson files_json() {
    ojson out = ojson::array();
    for (const FileInfo& f : g_files) {
        ojson o;
        o["path"] = f.path;
        o["source"] = f.source;
        o["rules"] = f.rules;
        o["error"] = f.error.empty() ? ojson(nullptr) : ojson(f.error);
        out.push_back(std::move(o));
    }
    return out;
}

// Adds "pid", "overlaps", "rules" and "ruleFiles" to the hook map (called by the map writer, registry lock not held).
void fill(ojson& doc) {
    std::lock_guard lock(g_mu);
    doc["pid"] = GetCurrentProcessId();
    const auto rows = chain_rows();
    const std::string sf = read_state_file(L"shroudforge.json");
    const ojson overlaps = compute_overlaps(doc, rows, parse_sf_status(sf));
    doc["overlaps"] = overlaps;  // the sections go in before anything is logged: one odd row must not drop them
    std::vector<std::string> symbols;
    for (const auto& o : overlaps)
        if (o.contains("symbol") && o["symbol"].is_string()) add_unique(symbols, o["symbol"].get<std::string>());
    const Facts facts = gather_facts(rows, symbols, sf_running(sf));
    // a `known` rule's overlaps are info (Live Map's hooks, sidelia's server 2026-11-21); the rest stay warnings
    ojson marked = overlaps;
    if (mark_known(marked, evaluate(g_rules, facts))) doc["overlaps"] = marked;
    for (const auto& o : marked) {
        if (!log_once("overlap|" + overlap_key(o))) continue;
        if (o.contains("known")) KLOG_I("compat: overlap: %s", json_text(o, "message").c_str());
        else KLOG_W("compat: overlap: %s", json_text(o, "message").c_str());
    }
    doc["rules"] = rules_report(rows, facts);
    doc["ruleFiles"] = files_json();
}

void map_extension(void* json, const char*) {
    if (!json) return;
    // Runs on the map writer's timer thread too: shroudforge.json (another runtime's file) with an unexpected type must
    // not throw out of it (std::terminate). The map is still written, without the compat sections.
    try {
        fill(*static_cast<ojson*>(json));
    } catch (const std::exception& e) {
        std::lock_guard lock(g_mu);
        if (log_once(std::string("fill|") + e.what())) KLOG_W("compat: overlap/rule report skipped: %s", e.what());
    }
}

void load_file(const fs::path& file, const char* source, std::vector<Rule>* rules) {
    std::error_code ec;
    if (!fs::exists(file, ec)) return;
    FileInfo info;
    info.path = u8(file);
    info.source = source;
    std::string text;
    if (!read_file(file, &text)) {
        info.error = "cannot be read";
        KLOG_W("compat: %s cannot be read; its rules are ignored", info.path.c_str());
    } else {
        ParseResult r = parse_rules(text, source);
        if (!r.ok) {
            info.error = r.error;
            KLOG_W("compat: %s: %s; its rules are ignored", info.path.c_str(), r.error.c_str());
        } else {
            for (const std::string& p : r.problems) {
                KLOG_W("compat: %s: %s", info.path.c_str(), p.c_str());
                info.error += (info.error.empty() ? "" : "; ") + p;
            }
            info.rules = r.rules.size();
            *rules = std::move(r.rules);
        }
    }
    g_files.push_back(std::move(info));
}

void CALLBACK settle_timer(PVOID, BOOLEAN) { hooks::mark_map_dirty(); }

}  // namespace

void init() {
    std::lock_guard lock(g_mu);
    if (g_inited) return;
    g_inited = true;
    // The last session's report, before this one overwrites it (the first write is at boot end).
    {
        std::string text;
        if (read_file(paths().state / L"hooks.json", &text)) try {
            const ojson prev = ojson::parse(text, nullptr, false);
            if (prev.is_object() && prev.contains("rules") && prev["rules"].is_array())
                for (const auto& r : prev["rules"])
                    if (r.is_object() && json_text(r, "action") == "late" && json_flag(r, "applied") && r.contains("id") && r["id"].is_string())
                        g_prev_late.insert(r["id"].get<std::string>());
        } catch (const std::exception&) {
            g_prev_late.clear();  // an edited or damaged file (a key of another type): no sticky rules this start
        }
    }
    std::vector<Rule> shipped, user;
    load_file(paths().kiln / L"compat-rules.json", "shipped", &shipped);
    load_file(paths().config / L"compat-rules.json", "user", &user);
    std::vector<std::string> replaced;
    g_rules = merge_rules(std::move(shipped), user, &replaced);
    for (const std::string& id : replaced) KLOG_I("compat: the player's rule '%s' replaces the shipped one", id.c_str());
    if (!g_rules.empty()) KLOG_I("compat: %zu compatibility rule(s)", g_rules.size());
    hooks::add_map_extension(&map_extension);  // before hooks::boot_complete() writes the first hooks.json
}

void apply_early() {
    std::lock_guard lock(g_mu);
    if (g_rules.empty()) return;
    const auto rows = chain_rows();
    const Facts facts = gather_facts(rows, {}, false);
    for (const Rule* r : evaluate(g_rules, facts)) {
        if (r->action != "disable" || r->subject.kind != Matcher::Kind::Mod) continue;
        const std::string& ref = r->subject.value;
        const size_t colon = ref.find(':');
        std::string engine = ref.substr(0, colon), id = ref.substr(colon + 1);
        for (char& c : engine) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        const std::string why = "switched off by compat rule '" + r->id + "': " + r->message;
        bool done = false;
        if (engine == "kiln" || engine == "native" || engine == "plugin") {
            for (const char* kind : {"kiln", "native", "plugin"}) done |= profile::disable_entry(kind, id, why);
        } else {
            done = profile::disable_entry(engine, id, why);
        }
        if (!done) continue;
        g_rule_off.insert(ref);
        // The report (rules_report) says whether that held: a dllmain native mod's DLL may be running already.
        g_early.emplace_back(r->id, "switched off in the profile before the plugins read it");
    }
}

std::vector<Rule> native_wait_rules(Facts* facts) {
    std::lock_guard lock(g_mu);
    *facts = gather_facts(chain_rows(), {}, false, false);
    std::vector<Rule> out;
    for (const Rule* r : evaluate(g_rules, *facts))
        if (r->action == "defer" && present(r->target, *facts)) out.push_back(*r);
    return out;
}

namespace {
KilnStr KILN_CALL original_rule(KilnStr mod) {
    std::lock_guard lock(g_mu);
    const Facts facts = gather_facts(chain_rows(), {}, false, false);
    const Rule* r = original_code_rule(g_rules, facts, sv(mod));
    return r ? kstr(r->id) : KilnStr{};
}
void KILN_CALL original_used(uint32_t rva) {
    std::string who;
    for (const auto& t : hooks::hooked_targets()) {
        if (t.rva != rva) continue;
        if (t.has_over) who = t.over.module;
        else if (!t.links.empty()) who = t.links.front().owner;
        else if (t.has_before) who = t.before.module;
        break;
    }
    if (who.empty()) {
        const auto address = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr)) + rva;
        for (const auto& d : sigdb::live_differs())
            if (d.address == address) { who = d.by; break; }
    }
    KLOG_I("shroudtopia: original-code check used the game's original bytes at rva 0x%X%s%s", rva,
           who.empty() ? "" : "; hook ", who.c_str());
}
bool KILN_CALL original_read(uintptr_t address, uint8_t* out, size_t size) {
    if (!out || !size || size > 4096 + 34) return false;
    // A mapped file can fault after a disk error. Never propagate that fault into a foreign memcmp.
    __try { return sigdb::file_bytes(address, out, size); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
const OriginalCodeApi kOriginalCodeTable{{sizeof(OriginalCodeApi), kOriginalCodeVersion}, &original_rule, &original_used, &original_read};
} // namespace
const OriginalCodeApi* original_code_table() { return &kOriginalCodeTable; }

void boot_done() {
    const bool hooked = hooks::chain::ready();
    if (!hooked) {  // safe idle: no hook map, so no hooks.json; the warnings still go to kiln.log
        ojson doc;
        doc["targets"] = ojson::array();
        doc["foreign"] = ojson::array();
        map_extension(&doc, "boot");
        return;
    }
    // ShroudForge reports its hooks a little after boot: write the map again once it had time to
    for (const auto& p : plugin_records()) {
        if (p.file.find("shroudforge") == std::string::npos || std::string_view(p.status) != "loaded") continue;
        HANDLE t = nullptr;
        for (DWORD ms : {12000u, 45000u})
            CreateTimerQueueTimer(&t, nullptr, &settle_timer, nullptr, ms, 0, WT_EXECUTEONLYONCE);
        break;
    }
}

}  // namespace kiln::compat
