// SPDX-License-Identifier: MIT
// kiln-dllmod: starting Enshrouded ModLoader packages at a world's start (src/session.h).
#include "session.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <map>
#include <set>
#include <utility>

#include "../../compat/compat.h"

namespace kdllmod {
namespace {

namespace fs = std::filesystem;
using kcompat::path_utf8;

// ---- each mod's log function and context ----
// write_log takes nothing but the text, so each started mod gets a function of its own that knows whose line it is
// (kiln.log "[dllmod:<id>] ..."). 64 of them; a 65th mod and later share the last one ("[dllmod:mods] ...").
constexpr size_t kThunks = 64;
std::string g_tags[kThunks + 1];
LogSink g_sink;
ModContext g_contexts[kThunks + 1];
char g_world[33] = {};

// Copies at most 4096 bytes of a mod's C string; false when reading it faults (a bad pointer from the mod).
bool copy_text(const char* text, char* out, size_t cap, size_t* len) {
    __try {
        size_t n = 0;
        while (n + 1 < cap && text[n]) {
            out[n] = text[n];
            ++n;
        }
        out[n] = 0;
        *len = n;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void emit(size_t slot, const char* text) {
    if (!text || !g_sink) return;
    char buf[4097];
    size_t n = 0;
    if (!copy_text(text, buf, sizeof buf, &n)) return;
    std::string line(buf, n);
    while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();
    g_sink(KILN_LOG_INFO, g_tags[slot], line);
}

template <size_t N>
void __cdecl thunk(const char* text) {
    emit(N, text);
}

template <size_t... I>
constexpr std::array<void(__cdecl*)(const char*), sizeof...(I)> make_thunks(std::index_sequence<I...>) {
    return {&thunk<I>...};
}
const auto kThunkTable = make_thunks(std::make_index_sequence<kThunks + 1>{});

// ---- guarded calls into a mod ----
HMODULE load(const fs::path& dll, DWORD* error, DWORD* fault) {
    *error = 0;
    *fault = 0;
    __try {
        // the package's own folder and System32: the DLLs it depends on come from there, never from the game folder
        HMODULE h = LoadLibraryExW(dll.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!h) *error = GetLastError();
        return h;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *fault = GetExceptionCode();
        return nullptr;
    }
}

int call_configure(ConfigureFn fn, const char* world, const char* text, DWORD* fault) {
    __try {
        return fn(world, text);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *fault = GetExceptionCode();
        return -1;
    }
}

int call_initialize(InitializeFn fn, const ModContext* ctx, DWORD* fault) {
    __try {
        return fn(ctx);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *fault = GetExceptionCode();
        return -1;
    }
}

std::string hex32(DWORD v) {
    char b[16];
    std::snprintf(b, sizeof b, "0x%08lX", static_cast<unsigned long>(v));
    return b;
}

std::string lower(std::string s) { return kcompat::lower(s); }

bool has_export(const std::vector<std::string>& exports, const char* name) {
    return std::find(exports.begin(), exports.end(), name) != exports.end();
}

}  // namespace

bool has_package_header(const fs::path& dll) {
    fs::path manifest = dll;
    manifest += L".mod";
    std::ifstream f(manifest, std::ios::binary);
    if (!f) return false;
    char head[64] = {};
    f.read(head, sizeof head - 1);
    std::string_view s(head, static_cast<size_t>(f.gcount()));
    if (s.substr(0, 3) == "\xEF\xBB\xBF") s.remove_prefix(3);
    return s.substr(0, kPackageHeader.size()) == kPackageHeader &&
           (s.size() == kPackageHeader.size() || s[kPackageHeader.size()] == '\r' || s[kPackageHeader.size()] == '\n');
}

bool read_manifest(const fs::path& dll, Manifest* out, std::string* error) {
    fs::path manifest = dll;
    manifest += L".mod";
    std::error_code ec;
    const auto size = fs::file_size(manifest, ec);
    if (ec) {
        *error = "no " + path_utf8(manifest.filename()) + " beside it";
        return false;
    }
    if (size > kManifestMax) {
        *error = path_utf8(manifest.filename()) + " is larger than 16 KiB";
        return false;
    }
    std::string text;
    if (!kcompat::read_file(manifest, &text)) {
        *error = "cannot read " + path_utf8(manifest);
        return false;
    }
    if (!parse_manifest(text, out, error)) {
        *error = path_utf8(manifest.filename()) + ": " + *error;
        return false;
    }
    return true;
}

std::vector<fs::path> package_dlls(const fs::path& folder) {
    std::vector<fs::path> out;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(folder, ec)) {
        if (!e.is_regular_file(ec) || lower(path_utf8(e.path().extension())) != ".dll") continue;
        fs::path m = e.path();
        m += L".mod";
        if (fs::is_regular_file(m, ec)) out.push_back(e.path());
    }
    std::sort(out.begin(), out.end());
    return out;
}

bool Session::resident() const {
    std::lock_guard lock(mu_);
    return resident_;
}

std::string Session::world() const {
    std::lock_guard lock(mu_);
    return world_;
}

size_t Session::started() const {
    std::lock_guard lock(mu_);
    return static_cast<size_t>(std::count_if(mods.begin(), mods.end(), [](const Mod& m) { return m.status == "started"; }));
}

size_t Session::failed() const {
    std::lock_guard lock(mu_);
    return static_cast<size_t>(
        std::count_if(mods.begin(), mods.end(), [](const Mod& m) { return m.status == "failed" || m.status == "rejected"; }));
}

bool Session::may_join(std::string* why) const {
    std::lock_guard lock(mu_);
    if (!resident_) return true;
    *why = "Mods for Enshrouded ModLoader are loaded in this game session (started for a world of your own), and they "
           "are made for your own worlds only. Restart Enshrouded to join another player's world.";
    return false;
}

bool Session::start_world(const std::string& world_id, std::string* why) {
    std::lock_guard lock(mu_);
    auto say = [&](int level, const std::string& mod, const std::string& text) {
        if (log) log(level, mod, text);
    };
    if (tainted_) {
        *why = "A mod for Enshrouded ModLoader failed to start earlier in this game session and may be loaded half started. "
               "Restart Enshrouded before you start a world (kiln/logs/kiln.log names the mod).";
        return false;
    }
    std::vector<Mod*> wanted;
    for (auto& m : mods) {
        if (!m.enabled || !m.manifest.is_mod) continue;
        if (m.status == "rejected") {
            // fail closed: a world started without a mod it has items of can lose them when it saves
            *why = (m.name.empty() ? m.id : m.name) + " (a mod for Enshrouded ModLoader) cannot run: " + m.reason +
                   ". Switch it off in the Kiln launcher (or fix it), then start the world again.";
            return false;
        }
        if (m.status == "waiting" || m.status == "started") wanted.push_back(&m);
    }
    if (committed_) {
        if (world_.empty() || world_ == world_id) return true;
        *why = "The mods for Enshrouded ModLoader were started for another world in this game session. They start once per "
               "game session, for one world: restart Enshrouded to play this world with them.";
        return false;
    }
    if (wanted.empty()) return true;
    if (!valid_world_id(world_id)) {
        *why = "Kiln could not read this world's id, which the mods for Enshrouded ModLoader need, so it started none of "
               "them. Restart Enshrouded and try again; kiln/logs/kiln.log has the details.";
        return false;
    }

    // the load order: required packages first, each once
    std::map<std::string, Mod*> by_package;
    std::vector<fs::path> roots;
    for (auto& m : mods) by_package[m.package] = &m;
    for (Mod* m : wanted)
        if (std::find(roots.begin(), roots.end(), m->root) == roots.end()) roots.push_back(m->root);
    std::map<std::string, fs::path> files;  // package id -> DLL
    auto lookup = [&](const std::string& id, PlanItem* item) {
        item->id = id;
        if (auto it = by_package.find(id); it != by_package.end()) {
            item->manifest = it->second->manifest;
            files[id] = it->second->dll;
            return true;
        }
        for (const auto& root : roots) {
            const fs::path dll = root / kcompat::to_wide(id);
            std::error_code ec;
            if (!fs::is_regular_file(dll, ec)) continue;
            files[id] = dll;
            read_manifest(dll, &item->manifest, &item->error);
            return true;
        }
        item->error = "the package is not there (Kiln looked in " + (roots.empty() ? std::string("no folder") : path_utf8(roots[0])) + ")";
        return true;
    };
    std::vector<std::string> wanted_ids;
    for (Mod* m : wanted) wanted_ids.push_back(m->package);
    std::vector<PlanItem> plan;
    std::string error;
    if (!make_plan(wanted_ids, lookup, &plan, &error)) {
        *why = "The mods for Enshrouded ModLoader cannot start: " + error + ".";
        say(KILN_LOG_ERROR, "", "world start refused: " + error);
        return false;
    }

    // before anything runs: each DLL once, none already in the process, the exports the package format asks for, and
    // each mod's settings
    std::set<std::string> names;
    std::map<std::string, std::vector<std::string>> exports;
    for (const auto& item : plan) {
        const fs::path& dll = files[item.id];
        const std::string base = lower(path_utf8(dll.filename()));
        if (!names.insert(base).second) {
            *why = "Two packages have a DLL named " + base + ": Windows would load only one of them.";
            return false;
        }
        if (GetModuleHandleW(dll.filename().c_str())) {
            *why = item.id + " is already loaded by something else in the game (another loader, or a copy of it added as a "
                   "plain DLL mod), so Kiln does not start it a second time.";
            return false;
        }
        exports[item.id] = kcompat::pe_exports(dll);
        const auto& ex = exports[item.id];
        if (item.manifest.is_mod && !has_export(ex, kInitializeExport)) {
            *why = item.id + " does not export EnshroudedModInitialize, which a mod for Enshrouded ModLoader must.";
            return false;
        }
        if (item.manifest.is_mod && !item.manifest.settings.empty() && !has_export(ex, kConfigureExport)) {
            *why = item.id + " has settings but does not export EnshroudedModConfigure.";
            return false;
        }
        if (!item.manifest.is_mod && has_export(ex, kInitializeExport)) {
            *why = item.id + " says it is a library but exports EnshroudedModInitialize.";
            return false;
        }
    }
    for (Mod* m : wanted) {
        m->settings.clear();
        m->settings_from.clear();
        if (m->manifest.settings.empty()) continue;
        Values v;
        std::string from, err;
        if (!settings || !settings(*m, world_id, &v, &from, &err)) {
            m->status = "rejected";
            m->reason = err;
            *why = (m->name.empty() ? m->id : m->name) + ": " + err + ".";
            return false;
        }
        m->settings = std::move(v);
        m->settings_from = std::move(from);
    }

    // run them
    std::memcpy(g_world, world_id.data(), 32);
    g_world[32] = 0;
    g_sink = log;
    world_ = world_id;
    tainted_ = true;
    size_t slot = 0;
    for (const auto& item : plan) {
        Mod* m = by_package.count(item.id) ? by_package[item.id] : nullptr;
        const std::string tag = m ? m->id : item.id;
        const fs::path& dll = files[item.id];
        DWORD err = 0, fault = 0;
        HMODULE h = load(dll, &err, &fault);
        resident_ = resident_ || h || fault;
        auto fail = [&](const std::string& reason) {
            if (m) {
                m->status = "failed";
                m->reason = reason;
            }
            say(KILN_LOG_ERROR, tag, reason);
            *why = (m && !m->name.empty() ? m->name : item.id) + " (a mod for Enshrouded ModLoader) failed to start: " + reason +
                   ". Restart Enshrouded; switch the mod off in the Kiln launcher if it keeps failing.";
            return false;
        };
        if (!h)
            return fail(fault ? "loading " + path_utf8(dll) + " faulted (" + hex32(fault) + ")"
                              : "Windows could not load " + path_utf8(dll) + " (error " + std::to_string(err) + ")");
        HMODULE pinned = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN, reinterpret_cast<LPCWSTR>(h), &pinned);
        if (!item.manifest.is_mod) {
            say(KILN_LOG_INFO, tag, "library " + item.id + " " + item.manifest.version + " loaded");
            continue;
        }
        const size_t s = slot < kThunks ? slot++ : kThunks;
        g_tags[s] = s < kThunks ? tag : std::string("mods");
        g_contexts[s] = ModContext{sizeof(ModContext), 1, g_world, kThunkTable[s]};
        if (!item.manifest.settings.empty()) {
            auto configure = reinterpret_cast<ConfigureFn>(reinterpret_cast<void*>(GetProcAddress(h, kConfigureExport)));
            const std::string text = settings_text(m ? m->settings : defaults(item.manifest));
            fault = 0;
            const int r = configure ? call_configure(configure, g_world, text.c_str(), &fault) : 0;
            if (r != 1)
                return fail(fault ? "EnshroudedModConfigure faulted (" + hex32(fault) + ")"
                                  : "EnshroudedModConfigure refused its settings (returned " + std::to_string(r) + ")");
        }
        auto initialize = reinterpret_cast<InitializeFn>(reinterpret_cast<void*>(GetProcAddress(h, kInitializeExport)));
        fault = 0;
        const int r = initialize ? call_initialize(initialize, &g_contexts[s], &fault) : 0;
        if (r != 1)
            return fail(fault ? "EnshroudedModInitialize faulted (" + hex32(fault) + ")"
                              : "EnshroudedModInitialize returned " + std::to_string(r) + " (not 1: the mod did not start)");
        if (m) {
            m->status = "started";
            m->reason.clear();
        }
        say(KILN_LOG_INFO, tag,
            "started " + item.id + " " + item.manifest.version + " for world " + world_id +
                (m && !m->settings.empty() ? " (settings: " + m->settings_from + ")" : ""));
    }
    committed_ = true;
    tainted_ = false;
    return true;
}

}  // namespace kdllmod
