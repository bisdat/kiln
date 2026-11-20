// SPDX-License-Identifier: MIT
// kiln-dllmod's host of Enshrouded ModLoader packages for one game process: which mods it has, and what happens when a
// local world starts (load each package, required ones first; give each kind=mod package its settings through
// EnshroudedModConfigure, then call EnshroudedModInitialize) and when the game joins another machine's world. No Kiln
// API here: src/plugin.cpp connects it to Kiln's hooks and files, tests/host_test.cpp drives it with test mods.
#pragma once

#include <windows.h>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include "package.h"

namespace kdllmod {

// The context a mod's EnshroudedModInitialize receives (ABI 1, 24 bytes on x64): its own size, the ABI version (1),
// the world's id (32 lower-case hex digits, valid until the process ends) and a log function taking one NUL-terminated
// UTF-8 line. Both exports use the C calling convention and return 1 for success, anything else for failure.
struct ModContext {
    uint32_t size;
    uint32_t abi_version;
    const char* world_id;
    void(__cdecl* write_log)(const char* message);
};
static_assert(sizeof(ModContext) == 24, "ABI 1 context layout");
using InitializeFn = int(__cdecl*)(const ModContext* context);
using ConfigureFn = int(__cdecl*)(const char* world_id, const char* settings_utf8);

constexpr const char* kInitializeExport = "EnshroudedModInitialize";
constexpr const char* kConfigureExport = "EnshroudedModConfigure";

struct Mod {
    std::string id;       // the profile entry's id (the Library id) or, for a mod found in the folder, its package id
    std::string package;  // the package id: the DLL's path below the package root, lower case ("carryon/carryon.dll")
    std::string name, version;
    std::string origin;   // "profile" or "folder"
    std::filesystem::path dll, root;  // root: the folder the package id (and its require lines) are relative to
    Manifest manifest;
    bool enabled = true;
    // waiting (starts with the next world), started, failed, rejected (cannot run: reason), disabled (switched off),
    // skipped (not on this game or side), own-loader / not-loaded (Enshrouded ModLoader itself runs this start)
    std::string status = "waiting";
    std::string reason;
    Values settings;            // what Configure was given
    std::string settings_from;  // where those values came from
};

// Where a mod's settings come from when a world starts: false with the reason when they cannot be used.
using SettingsSource = std::function<bool(const Mod& mod, const std::string& world_id, Values* values, std::string* from,
                                          std::string* error)>;
// A log line: level (1 error .. 5 trace, as KILN_LOG_*), the mod's id (empty for the host's own lines) and the text.
using LogSink = std::function<void(int level, const std::string& mod, const std::string& text)>;

class Session {
public:
    std::vector<Mod> mods;
    LogSink log;
    SettingsSource settings;

    // A local world is starting (on the game's thread, before the game's own start runs). True: let it start. False:
    // the world must not start, *why says why (a mod that cannot run, a mod that failed, another world's mods resident).
    bool start_world(const std::string& world_id, std::string* why);
    // The game starts a session that joins another machine's world: false (with *why) while mods are loaded.
    bool may_join(std::string* why) const;
    bool resident() const;   // a package was loaded into the process
    std::string world() const;  // the world the mods were started for ("" before)
    size_t started() const;
    size_t failed() const;

private:
    mutable std::mutex mu_;
    bool committed_ = false;  // every package loaded and started for world_
    bool tainted_ = false;    // a start failed part way: packages may be resident half started
    bool resident_ = false;
    std::string world_;
};

// The package ids of the mods and libraries in a folder: every *.dll directly in it with a <file>.dll.mod beside it.
std::vector<std::filesystem::path> package_dlls(const std::filesystem::path& folder);
// Reads a <dll>.mod manifest (false with the reason).
bool read_manifest(const std::filesystem::path& dll, Manifest* out, std::string* error);
// Whether the manifest file beside a DLL starts with the package header (cheap: the first line only).
bool has_package_header(const std::filesystem::path& dll);

}  // namespace kdllmod
