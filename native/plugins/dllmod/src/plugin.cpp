// SPDX-License-Identifier: MIT
// kiln-dllmod.dll: a Kiln plugin (<game>/kiln/plugins/) that runs mods made for AndyPimms' Enshrouded ModLoader
// (github.com/aebischer/Enshrouded-ModLoader; packages of a DLL and a <file>.dll.mod manifest, ABI 1: CarryOn, Belt
// Lantern) without that loader's dinput8.dll. Kiln's own code, written from the package format and the observable
// contract between that loader and its mods (docs/kiln/engines/dllmod.md); no code of the loader is used.
//   1. client build 26430625 (enshrouded.exe 5d9ef2f1) only, as the loader and its mods: any other build, or a
//      dedicated server, starts nothing and says so;
//   2. step aside when Enshrouded ModLoader itself is in the process or listed in a chain list (it refuses to start when
//      a function it hooks was patched first, so Kiln then hooks nothing at all);
//   3. find the mods: the active profile's "dllmod" entries (Kiln's copies in <game>/Mods/EnshroudedModLoader/<id>/),
//      then the packages in that folder when drop-ins are on (compat.h's rule);
//   4. with a mod to run, hook the game's local world start and session start through kiln.hooks. When a local world
//      starts, before the game's own start: load each package (required ones first), EnshroudedModConfigure with its
//      settings, EnshroudedModInitialize with the world's id (session.cpp). A mod that cannot run or fails keeps the
//      world from starting (a world saved without a mod can lose that mod's items); joining another player's world
//      while mods are loaded is refused, as the loader does;
//   5. write <state>/dllmod.json, log under [dllmod] and [dllmod:<id>].
#include <windows.h>

#include <psapi.h>

#include <algorithm>
#include <atomic>
#include <memory>
#include <set>

#include "../../compat/compat.h"
#include "kiln_hook.hpp"
#include "package.h"
#include "session.h"

namespace kdllmod {
namespace {

namespace fs = std::filesystem;
using kcompat::path_utf8;
using kcompat::to_utf8;
using kcompat::to_wide;

constexpr const char* kPluginId = "kiln-dllmod";
constexpr const char* kPluginVersion = "1.0.0";
constexpr const char* kKind = "dllmod";
constexpr const char* kEngineName = "Enshrouded ModLoader";

kcompat::Host H{nullptr, 0, kKind};
Session g_session;
const GameBinding* g_bind = nullptr;
std::mutex g_mu;  // status fields below
std::string g_status = "idle", g_status_reason, g_aside, g_build;
fs::path g_game, g_kiln, g_state, g_root;  // g_root: <game>\Mods\EnshroudedModLoader, the folder the packages live in
kiln::RawHook g_start_local, g_start_session;
std::atomic<bool> g_box_open{false};

// ---- status ----
void write_status() {
    using json = nlohmann::json;
    std::lock_guard lock(g_mu);
    json mods = json::array();
    size_t started = 0, failed = 0;
    for (const auto& m : g_session.mods) {
        json j = {{"id", m.id},         {"package", m.package}, {"name", m.name},         {"version", m.version},
                  {"origin", m.origin}, {"status", m.status},   {"reason", m.reason},     {"enabled", m.enabled},
                  {"folder", path_utf8(m.dll.parent_path())},   {"dll", path_utf8(m.dll)}};
        if (!m.settings.empty()) {
            j["settings"] = m.settings;
            j["settingsFrom"] = m.settings_from;
        }
        mods.push_back(std::move(j));
        started += m.status == "started";
        failed += m.status == "failed" || m.status == "rejected";
    }
    std::string status = g_status;
    if (status == "waiting" && started) status = "running";
    if (status == "waiting" && failed && !started) status = "failed";
    json doc = {{"format", 1},
                {"plugin", kPluginId},
                {"version", kPluginVersion},
                {"engine", kEngineName},
                {"abi", 1},
                {"side", H.api && H.api->side() == KILN_SIDE_SERVER ? "server" : "client"},
                {"status", status},
                {"reason", g_status_reason},
                {"build", g_build},
                {"pid", static_cast<uint64_t>(GetCurrentProcessId())},
                {"world", g_session.world()},
                {"started", started},
                {"failed", failed},
                {"steppedAside", !g_aside.empty()},
                {"steppedAsideReason", g_aside},
                {"packages", path_utf8(g_root)},
                {"updatedUnixMs", kcompat::unix_ms()},
                {"mods", mods}};
    const auto file = g_state / L"dllmod.json";
    if (!kcompat::write_status_file(file, doc.dump(2) + "\n")) H.logf(KILN_LOG_WARN, "cannot write %s", path_utf8(file).c_str());
}

// ---- telling the player ----
// The world start is refused from the game's own start function, which has no way to show a message: a Windows message
// box on a thread of its own (the game keeps running), one at a time.
DWORD WINAPI box_thread(void* p) {
    std::unique_ptr<std::wstring> text(static_cast<std::wstring*>(p));
    MessageBoxW(nullptr, text->c_str(), L"Kiln: mods for Enshrouded ModLoader", MB_OK | MB_ICONWARNING | MB_SETFOREGROUND | MB_TOPMOST);
    g_box_open = false;
    return 0;
}

void notify(const std::string& text) {
    if (g_box_open.exchange(true)) return;
    auto* w = new std::wstring(to_wide(text));
    HANDLE t = CreateThread(nullptr, 0, &box_thread, w, 0, nullptr);
    if (t) {
        CloseHandle(t);
    } else {
        delete w;
        g_box_open = false;
    }
}

// ---- settings ----
// What Configure gets, the first of: Kiln's settings file for the mod (<config>/dllmod/<id>.ini, the profile's config
// folder; the same format as the loader's), Enshrouded ModLoader's own file for this world
// (<game>/ModLoader/worlds/<world id>.settings-v2/<SHA-256 of the package id>.ini, what its MODS page saved), the
// manifest's defaults. A file that is there but invalid keeps the world from starting, as the loader does.
bool read_settings_file(const fs::path& f, const Manifest& m, Values* v, std::string* err) {
    std::string text;
    if (!kcompat::read_file(f, &text)) {
        *err = "cannot read " + path_utf8(f);
        return false;
    }
    std::string why;
    if (!parse_settings(text, m, v, &why)) {
        *err = "its settings file " + path_utf8(f) + " is not valid (" + why + ")";
        return false;
    }
    return true;
}

bool settings_for(const Mod& m, const std::string& world, Values* v, std::string* from, std::string* err) {
    if (kcompat::safe_component(m.id)) {
        const fs::path kiln = H.config_dir() / L"dllmod" / (to_wide(m.id) + L".ini");
        if (kcompat::exists(kiln)) {
            *from = path_utf8(kiln);
            return read_settings_file(kiln, m.manifest, v, err);
        }
    }
    // 0.6.15 keeps them in Mods/EnshroudedModLoader/worlds; earlier versions in ModLoader/worlds
    fs::path own = g_root / L"worlds" / (to_wide(world) + L".settings-v2") / (to_wide(sha256_hex(m.package)) + L".ini");
    if (!kcompat::exists(own))
        own = g_game / L"ModLoader" / L"worlds" / (to_wide(world) + L".settings-v2") / (to_wide(sha256_hex(m.package)) + L".ini");
    if (kcompat::exists(own)) {
        *from = "Enshrouded ModLoader's settings for this world (" + path_utf8(own) + ")";
        return read_settings_file(own, m.manifest, v, err);
    }
    *v = defaults(m.manifest);
    *from = "the mod's defaults";
    return true;
}

// ---- Enshrouded ModLoader itself ----
// Its dinput8.dll: exports DirectInput8Create, names its world profile header and its log file. Kiln's own files are
// never it (compat's kiln_own_module), nor is a mod (exports EnshroudedModInitialize). The marker texts are put
// together at run time so this plugin's file does not carry them.
bool is_loader_file(const fs::path& f) {
    std::string d;
    if (!kcompat::read_file(f, &d)) return false;
    const auto ex = kcompat::pe_exports(f);
    if (std::find(ex.begin(), ex.end(), "DirectInput8Create") == ex.end() ||
        std::find(ex.begin(), ex.end(), kInitializeExport) != ex.end())
        return false;
    std::string profile = "ENSHROUDED_";
    profile += "MODS_V1";
    std::wstring log = L"mod-loader";
    log += L".log";
    const std::string log16(reinterpret_cast<const char*>(log.data()), log.size() * sizeof(wchar_t));
    return d.find(profile) != std::string::npos || d.find(log16) != std::string::npos;
}

// Kiln's own modules, which name every engine. Not compat's kiln_own_module: that one counts everything under
// <game>\kiln\ (but kiln\chain\) as Kiln's, and Enshrouded ModLoader 0.6.15 installs itself next to Kiln as a Kiln native
// module, kiln\native\enshrouded-modloader\modloader.dll. Kiln's own: the proxy, kiln-core.dll, plugins and services.
bool kiln_module(HMODULE h, const fs::path& f) {
    if (h && (GetProcAddress(h, "KilnProxyInfo") || GetProcAddress(h, "kiln_boot") || GetProcAddress(h, "kiln_plugin_main"))) return true;
    const std::string p = kcompat::lower(path_utf8(f)), k = kcompat::lower(path_utf8(g_kiln)) + "\\";
    if (kcompat::lower(path_utf8(f.filename())) == "kiln-core.dll") return true;
    for (const char* own : {"plugins\\", "services\\", "host\\"})
        if (p.rfind(k + own, 0) == 0) return true;
    return false;
}

std::string loader_in_process() {
    HMODULE mods[1024];
    DWORD needed = 0;
    if (!K32EnumProcessModules(GetCurrentProcess(), mods, sizeof mods, &needed)) return {};
    const DWORD n = std::min<DWORD>(needed / sizeof(HMODULE), 1024);
    wchar_t sys[MAX_PATH] = L"";
    const UINT sn = GetSystemDirectoryW(sys, MAX_PATH);
    const std::string system = kcompat::lower(to_utf8(std::wstring(sys, sn < MAX_PATH ? sn : 0)));
    for (DWORD i = 0; i < n; ++i) {
        if (mods[i] == GetModuleHandleW(nullptr)) continue;
        wchar_t p[MAX_PATH] = L"";
        if (!GetModuleFileNameW(mods[i], p, MAX_PATH)) continue;
        const fs::path f(p);
        if (!system.empty() && kcompat::lower(path_utf8(f)).rfind(system, 0) == 0) continue;
        if (kiln_module(mods[i], f) || !kcompat::module_is_file(mods[i], f)) continue;
        if (GetProcAddress(mods[i], "DirectInput8Create") && is_loader_file(f)) return path_utf8(f);
    }
    return {};
}

// An active line of <game>/dinput8-chain.ini or kiln/chain.ini (any section: [chain], [profile], [late]) naming it:
// it loads in this process even when it has not loaded yet.
std::string loader_listed() {
    for (const auto& [list, base] : {std::pair{g_game / L"dinput8-chain.ini", g_game}, std::pair{g_kiln / L"chain.ini", g_kiln}}) {
        const bool shared = base == g_game;
        std::string t;
        if (!kcompat::read_file(list, &t)) continue;
        size_t i = 0;
        while (i < t.size()) {
            size_t j = t.find('\n', i);
            if (j == std::string::npos) j = t.size();
            std::string line = t.substr(i, j - i);
            i = j + 1;
            if (shared)
                for (size_t k = 0; k + 1 < line.size(); ++k)
                    if ((line[k] == ' ' || line[k] == '\t') && (line[k + 1] == ';' || line[k + 1] == '#')) {
                        line.resize(k);
                        break;
                    }
            while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) line.pop_back();
            const size_t eq = line.find('=');
            if (eq != std::string::npos) line = line.substr(eq + 1);
            while (!line.empty() && (line.front() == ' ' || line.front() == '\t')) line.erase(0, 1);
            if (shared && line.size() >= 2 && line.front() == '"' && line.back() == '"') line = line.substr(1, line.size() - 2);
            if (line.empty() || line[0] == ';' || line[0] == '#' || line[0] == '[' || line.rfind("//", 0) == 0) continue;
            const std::string low = kcompat::lower(line);
            if (low.size() < 4 || low.substr(low.size() - 4) != ".dll") continue;
            std::replace(line.begin(), line.end(), '/', '\\');
            const fs::path p = (line.size() > 2 && line[1] == ':') ? fs::path(to_wide(line)) : base / to_wide(line);
            if (kcompat::exists(p) && is_loader_file(p)) return path_utf8(list) + " lists it (" + line + ")";
        }
    }
    return {};
}

// Enshrouded ModLoader 0.6.15+ as a Kiln native module (its installer puts it in kiln\native\enshrouded-modloader\ with
// a native.json, phase boot). kiln-core loads phase boot native mods after every plugin's on_ready, so it is not loaded
// yet when kiln-dllmod decides: what counts is whether kiln-core will load it, the profile's native entries (switched
// on) and the kiln\native drop-in folder when the profile reads it (the core's rule, native_mods.cpp discover).
std::string loader_native_module() {
    std::vector<fs::path> folders;
    bool dropins = true;
    if (const auto* prof = H.service<KilnProfileApi>(KILN_PROFILE_SERVICE, 1)) {
        const uint32_t mode = prof->mode();
        if (mode == KILN_PROFILE_NO_MODS || mode == KILN_PROFILE_INVALID) return {};
        dropins = (prof->dropins() & (1u << KILN_PROFILE_KIND_NATIVE)) != 0;
        if (mode != KILN_PROFILE_LEGACY) {
            try {
                const auto doc = nlohmann::json::parse(std::string(kcompat::sv(prof->json())), nullptr, true, true);
                if (doc.is_object() && doc.contains("mods") && doc["mods"].is_object() && doc["mods"].contains("native") &&
                    doc["mods"]["native"].is_array())
                    for (const auto& m : doc["mods"]["native"])
                        if (m.is_object() && m.contains("path") && m["path"].is_string() &&
                            !(m.contains("enabled") && m["enabled"].is_boolean() && !m["enabled"].get<bool>()))
                            folders.push_back(kcompat::resolve_profile_path(m["path"].get<std::string>(), g_kiln));
            } catch (...) {
            }
        }
    }
    if (dropins) {
        std::error_code ec;
        for (const auto& d : fs::directory_iterator(g_kiln / L"native", ec))
            if (d.is_directory(ec) && kcompat::exists(d.path() / L"native.json")) folders.push_back(d.path());
    }
    for (const auto& folder : folders) {
        std::error_code ec;
        for (const auto& e : fs::directory_iterator(folder, ec))
            if (e.is_regular_file(ec) && kcompat::lower(path_utf8(e.path().extension())) == ".dll" && is_loader_file(e.path()))
                return "kiln-core loads it as a native mod (" + path_utf8(e.path()) + ")";
    }
    return {};
}

// ---- finding the mods ----
// The packages in one folder (a profile entry: Kiln's copy of a Library entry). Every kind=mod package in it is a mod;
// kind=library packages load when a mod requires them.
void add_folder(const fs::path& folder, const std::string& id, bool enabled, const std::string& origin, std::set<std::string>* seen) {
    const auto dlls = package_dlls(folder);
    size_t mods = 0;
    for (const auto& dll : dlls) {
        Mod m;
        m.dll = dll;
        m.root = folder.parent_path();
        m.package = package_id(path_utf8(dll.lexically_relative(m.root)));
        m.name = to_utf8(dll.stem().wstring());
        m.origin = origin;
        m.enabled = enabled;
        std::string error;
        if (!read_manifest(dll, &m.manifest, &error)) {
            m.id = id.empty() ? m.package : id;
            m.status = enabled ? "rejected" : "disabled";
            m.reason = enabled ? error : "switched off in the profile";
            if (seen->insert(m.package).second) g_session.mods.push_back(std::move(m));
            ++mods;
            continue;
        }
        if (!m.manifest.is_mod) continue;  // a library
        m.version = m.manifest.version;
        m.id = id.empty() ? m.package : mods == 0 ? id : id + ":" + m.name;
        ++mods;
        if (!seen->insert(m.package).second) {
            H.logf(KILN_LOG_INFO, "%s: also listed before, the first entry is used (%s skipped)", m.package.c_str(), path_utf8(dll).c_str());
            continue;
        }
        kcompat::CheckedDll checked;
        if (m.package.empty()) {
            m.status = "rejected";
            m.reason = "its file name is not one a package can have (" + path_utf8(dll.filename()) + ")";
        } else if (!enabled) {
            m.status = "disabled";
            m.reason = "switched off in the profile";
        } else if (!checked.open(folder, dll, kInitializeExport)) {
            m.status = "rejected";
            m.reason = checked.error.find(kInitializeExport) != std::string::npos
                           ? "it does not export EnshroudedModInitialize"
                           : "its DLL must stay inside its folder, without links: " + checked.error;
        } else if (!m.manifest.settings.empty() && !kcompat::pe_exports_name(dll, kConfigureExport)) {
            m.status = "rejected";
            m.reason = "it has settings but does not export EnshroudedModConfigure";
        }
        g_session.mods.push_back(std::move(m));
    }
    if (!mods) {
        Mod m;
        m.id = id.empty() ? to_utf8(folder.filename().wstring()) : id;
        m.name = m.id;
        m.origin = origin;
        m.enabled = enabled;
        m.dll = folder / L"?";
        m.status = enabled ? "rejected" : "disabled";
        m.reason = !enabled       ? "switched off in the profile"
                   : dlls.empty() ? "no DLL with a .dll.mod manifest beside it in " + path_utf8(folder)
                                  : "only library packages in " + path_utf8(folder) + " (they load when a mod requires them)";
        g_session.mods.push_back(std::move(m));
    }
}

// The packages below <game>/Mods/EnshroudedModLoader (drop-ins): every DLL with a package manifest beside it, at most 16
// folders deep and 8192 entries, links not followed; Kiln's copies of profile entries are the profile's.
void add_dropins(const kcompat::EnginePlan& plan, std::set<std::string>* seen) {
    if (!kcompat::exists(g_root)) return;
    std::set<fs::path> folders;
    std::error_code ec;
    fs::recursive_directory_iterator it(g_root, fs::directory_options::skip_permission_denied, ec), end;
    size_t visited = 0;
    const auto ids = kcompat::listed_ids(plan);
    for (; !ec && it != end; it.increment(ec)) {
        if (++visited > 8192) {
            H.logf(KILN_LOG_WARN, "%s holds more than 8192 entries: the rest is not read", path_utf8(g_root).c_str());
            break;
        }
        if (it.depth() >= 16) it.disable_recursion_pending();
        const auto& e = *it;
        if (e.is_symlink(ec)) {
            it.disable_recursion_pending();
            continue;
        }
        if (e.is_directory(ec) && it.depth() == 0 && kcompat::lower(path_utf8(e.path().filename())) == "worlds") {
            it.disable_recursion_pending();  // Enshrouded ModLoader's per-world profiles and settings (0.6.15)
            continue;
        }
        if (e.is_directory(ec) && it.depth() == 0) {
            if (const auto id = kcompat::library_copy_of(e.path(), kKind, ids); !id.empty()) {
                it.disable_recursion_pending();
                continue;
            }
        }
        if (e.is_regular_file(ec) && kcompat::lower(path_utf8(e.path().extension())) == ".dll" && has_package_header(e.path()))
            folders.insert(e.path().parent_path());
    }
    for (const auto& folder : folders) {
        const size_t before = g_session.mods.size();
        add_folder(folder, {}, true, "folder", seen);
        // a drop-in's root is Mods/EnshroudedModLoader, so its id is its path below that folder
        for (size_t i = before; i < g_session.mods.size(); ++i) {
            auto& m = g_session.mods[i];
            m.root = g_root;
            const std::string pkg = package_id(path_utf8(m.dll.lexically_relative(g_root)));
            if (!pkg.empty()) m.id = m.package = pkg;
        }
    }
}

// ---- the game's world and session starts ----
using StartLocalFn = void (*)(uint8_t* world_slot);
using StartSessionFn = void (*)(void* request);

bool read_world(const uint8_t* slot, uint8_t guid[16]) {
    __try {
        if (!slot || !slot[g_bind->world_valid]) return false;
        std::memcpy(guid, slot + g_bind->world_guid, 16);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool read_u64(const void* p, uint64_t* out) {
    __try {
        std::memcpy(out, p, 8);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool bytes_match(uintptr_t at, const uint8_t* want, size_t n) {
    __try {
        return std::memcmp(reinterpret_cast<const void*>(at), want, n) == 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void on_start_local(uint8_t* slot) {
    uint8_t guid[16] = {};
    const std::string world = read_world(slot, guid) ? world_id_hex(guid) : std::string();
    std::string why;
    bool ok = false;
    try {
        ok = g_session.start_world(world, &why);
    } catch (const std::exception& e) {
        why = std::string("Kiln failed while starting the mods for Enshrouded ModLoader (") + e.what() + "). Restart Enshrouded.";
    } catch (...) {
        why = "Kiln failed while starting the mods for Enshrouded ModLoader. Restart Enshrouded.";
    }
    write_status();
    if (!ok) {
        H.logf(KILN_LOG_WARN, "a world start was refused%s: %s", world.empty() ? "" : (" (world " + world + ")").c_str(), why.c_str());
        notify("Kiln did not start this world.\n\n" + why);
        return;
    }
    g_start_local.original<StartLocalFn>()(slot);
}

void on_start_session(void* request) {
    uint64_t first = 1;
    std::string why;
    // a session whose request has 0 in its first field joins another machine's world (a local world's start goes
    // through on_start_local and carries its world there)
    if (request && read_u64(request, &first) && first == 0 && !g_session.may_join(&why)) {
        H.logf(KILN_LOG_WARN, "joining another world was refused: %s", why.c_str());
        notify("Kiln did not join this world.\n\n" + why);
        return;
    }
    g_start_session.original<StartSessionFn>()(request);
}

bool install_hooks(std::string* why) {
    const auto base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    const uintptr_t local = base + g_bind->start_local_rva, session = base + g_bind->start_session_rva;
    if (!bytes_match(local, g_bind->start_local_bytes.data(), g_bind->start_local_bytes.size()) ||
        !bytes_match(session, g_bind->start_session_bytes.data(), g_bind->start_session_bytes.size())) {
        *why = "the game's world start is not what Kiln expects on this build (another tool patched it first?)";
        return false;
    }
    if (!g_start_session.install(H.api, H.owner, session, reinterpret_cast<void*>(&on_start_session), "dllmod.session-start")) {
        *why = std::string("could not hook the game's session start: ") + (g_start_session.error() ? g_start_session.error() : "?");
        return false;
    }
    if (!g_start_local.install(H.api, H.owner, local, reinterpret_cast<void*>(&on_start_local), "dllmod.world-start")) {
        g_start_session.revoke();
        *why = std::string("could not hook the game's world start: ") + (g_start_local.error() ? g_start_local.error() : "?");
        return false;
    }
    return true;
}

void set_status(const std::string& status, const std::string& reason) {
    std::lock_guard lock(g_mu);
    g_status = status;
    g_status_reason = reason;
}

// gives every mod that would run this status and reason
void mark_waiting(const std::string& status, const std::string& reason) {
    for (auto& m : g_session.mods)
        if (m.status == "waiting") {
            m.status = status;
            m.reason = reason;
        }
}

void KILN_CALL on_ready(void*) {
    g_game = H.path(KILN_PATH_GAME);
    g_kiln = H.path(KILN_PATH_KILN);
    g_state = H.state_dir();
    g_root = g_game / L"Mods" / L"EnshroudedModLoader";
    g_session.log = [](int level, const std::string& mod, const std::string& text) {
        if (mod.empty()) H.log(level, text);
        else H.mod_log(level, mod, text);
    };
    g_session.settings = &settings_for;

    // find
    const kcompat::EnginePlan plan = kcompat::plan_engine(H, kKind);
    std::set<std::string> seen;
    for (const auto& r : plan.listed) add_folder(r.path, r.id, r.enabled, "profile", &seen);
    if (plan.read_folder) add_dropins(plan, &seen);
    const bool any = std::any_of(g_session.mods.begin(), g_session.mods.end(), [](const Mod& m) { return m.status == "waiting"; });
    H.logf(KILN_LOG_INFO, "kiln-dllmod %s (mods for %s, ABI 1): %s; %zu mod(s)", kPluginVersion, kEngineName, plan.note.c_str(),
           g_session.mods.size());
    for (const auto& m : g_session.mods)
        if (m.status != "waiting") H.mod_log(m.status == "rejected" ? KILN_LOG_WARN : KILN_LOG_INFO, m.id, m.status + ": " + m.reason);

    // no mods at all: nothing to say, nothing is hooked
    if (g_session.mods.empty()) {
        set_status("idle", "");
        write_status();
        return;
    }

    // 1. Enshrouded ModLoader itself (before the build: the loader runs its own mods, Kiln hooks nothing for them)
    std::string loader = loader_in_process();
    if (!loader.empty()) loader = "it is loaded (" + loader + ")";
    if (loader.empty()) loader = loader_listed();
    if (loader.empty()) loader = loader_native_module();
    if (!loader.empty()) {
        {
            std::lock_guard lock(g_mu);
            g_aside = "Enshrouded ModLoader runs in this game: " + loader;
        }
        for (auto& m : g_session.mods) {
            if (m.status != "waiting") continue;
            if (m.origin != "profile") {
                m.status = "own-loader";
                m.reason = "Enshrouded ModLoader runs it: switch it on in the world's Edit > MODS page";
                continue;
            }
            auto rep = kcompat::aside_report(m.id, m.dll.parent_path(), true, kKind, g_root, kEngineName, "Mods\\EnshroudedModLoader");
            m.status = kcompat::aside_word(rep.state);
            m.reason = rep.reason + (rep.state == kcompat::Aside::OwnLoader ? "; switch it on in the world's Edit > MODS page" : "");
            kcompat::log_aside(H, m.id, rep);
        }
        set_status("stepped aside", g_aside);
        H.logf(KILN_LOG_INFO, "kiln-dllmod steps aside: %s. It starts its mods itself; Kiln hooks nothing for them", g_aside.c_str());
        write_status();
        return;
    }

    // 2. the side and the build
    KilnBuildInfo bi{};
    bi.size = sizeof bi;
    H.api->build_info(&bi);
    g_build = std::string(bi.exe_sha256).substr(0, 8);
    if (H.api->side() == KILN_SIDE_SERVER) {
        const std::string why = "mods for Enshrouded ModLoader are made for the game client; a dedicated server starts none";
        mark_waiting("skipped", why);
        set_status("client only", why);
        if (any) H.logf(KILN_LOG_INFO, "%s", why.c_str());
        write_status();
        return;
    }
    g_bind = binding_for(bi.exe_sha256);
    if (!g_bind) {
        const std::string why = "mods for Enshrouded ModLoader are made for client build 26430625 (enshrouded.exe 5d9ef2f1), "
                                "and this game is " + g_build + ": Kiln starts none of them on it";
        mark_waiting("skipped", why);
        set_status("unsupported", why);
        if (any) H.logf(KILN_LOG_WARN, "%s", why.c_str());
        write_status();
        return;
    }

    // 3. nothing to run: nothing is hooked
    if (!any) {
        set_status("idle", "");
        write_status();
        return;
    }

    // 4. hook the world start
    std::string why;
    if (!install_hooks(&why)) {
        mark_waiting("failed", why);
        set_status("failed", why);
        H.logf(KILN_LOG_ERROR, "kiln-dllmod: %s; no mod for %s starts", why.c_str(), kEngineName);
        write_status();
        return;
    }
    set_status("waiting", "the mods start when a world of your own starts");
    H.logf(KILN_LOG_INFO, "kiln-dllmod: %zu mod(s) start when a local world starts",
           static_cast<size_t>(std::count_if(g_session.mods.begin(), g_session.mods.end(), [](const Mod& m) { return m.status == "waiting"; })));
    write_status();
}

}  // namespace
}  // namespace kdllmod

extern "C" KILN_EXPORT KilnResult KILN_CALL kiln_plugin_main(const KilnApi* api, KilnOwner owner, KilnPluginInfo* info) {
    using namespace kdllmod;
    if (!api || api->abi_version != KILN_ABI_VERSION || api->size < offsetof(KilnApi, session) + sizeof(void*))
        return KILN_ERR_UNAVAILABLE;
    if (!kcompat::pin_module(reinterpret_cast<const void*>(&kiln_plugin_main))) return KILN_ERR_UNAVAILABLE;
    H.api = api;
    H.owner = owner;
    info->id = kcompat::kstr(kPluginId);
    info->version = kcompat::kstr(kPluginVersion);
    if (info->size >= offsetof(KilnPluginInfo, on_ready) + sizeof(void*)) info->on_ready = &on_ready;
    return KILN_OK;
}
