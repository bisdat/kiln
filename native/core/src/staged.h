// SPDX-License-Identifier: MIT
// kiln.staged: staged mod and Kiln updates, applied at the start before anything loads them, with rollback
// (docs/kiln/staged-updates.md). Kiln.Admin's update_* ops stage them under <server>/kiln/staged/ while the server runs;
// the next start applies them:
//   - the dinput8 proxy, at its entry gate and before it loads kiln-core.dll: unit "kiln" (boot_kiln), so a new
//     kiln-core.dll, kiln/host, kiln/services ... are what this start loads;
//   - kiln-core, right after kiln.log opens and before kiln.json, the profile or any mod is read: units "mod.<folder>"
//     (boot_mods), so kiln/mods/<folder> is the new version when the native mods and Kiln.Host look at it.
// One transaction per unit: every change is a rename (no copy), the journal tx/<unit>/journal.json is written before the
// first rename, and a start that finds a journal finishes a committed one or puts an uncommitted one back, so a crash or
// a power cut at any point leaves the old or the new version. Checksums are verified when the update is applied; a path
// outside the unit's folder, a link or a file the manifest does not list is refused. The replaced version is kept
// (kiln/staged/previous/<unit>) for one rollback; a mod that fails to load right after its update is rolled back once at
// the next start (Kiln.Host's probation), never in a loop. Only on a dedicated server.
//
// Portable C++ (Win32 file calls under _WIN32, POSIX for the host test) with no other core header than the SHA-256 of
// services/boot_gamedata.h, so the proxy and the core build the same file (native/core/tests/test-staged.sh).
#pragma once
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace kiln::staged {

enum class Level { Info, Warn, Error };

struct Env {
    std::filesystem::path game;  // the server folder (enshrouded_server.exe, dinput8.dll, kiln/)
    bool server = true;          // false: a game client, where nothing is applied
    std::string by;              // "proxy" | "core": who applied it (results)
    std::string kiln;            // this binary's Kiln version (results)
    uint64_t pid = 0;            // this process (results)
    std::function<void(Level, const std::string&)> log;  // one line, no newline
};

struct Summary {
    int applied = 0;      // updates applied
    int rolled_back = 0;  // rollbacks applied (requested or automatic)
    int failed = 0;       // refused or failed (each logged with its reason)
    int recovered = 0;    // interrupted transactions finished or put back
    int held = 0;         // staged updates that wait for free space (they stay staged)
};

inline constexpr int kFormat = 1;
inline constexpr int kMaxAttempts = 3;     // starts that may try one staged change before it is dropped
inline constexpr size_t kResultsKept = 50;  // results/*.json kept, newest first
inline constexpr size_t kMaxFiles = 20000;  // files in one staged unit
inline constexpr size_t kMaxRelPath = 240;  // bytes of one relative path
inline constexpr uintmax_t kMaxManifest = 16u << 20;  // bytes of staged.json, kept.json, journal.json

// The proxy: unit "kiln" (a staged Kiln update, its rollback, its automatic rollback). Cheap when kiln/staged is absent.
Summary boot_kiln(const Env& env);
// kiln-core: every "mod.<folder>" unit, then the stale staging folders Kiln.Admin left (kiln/staged/.stage-*).
Summary boot_mods(const Env& env);
// kiln/staged/kiln/staged.json exists: a Kiln update is still staged (the core says so when the proxy did not apply it).
bool kiln_update_waiting(const std::filesystem::path& game);
// The Kiln update this start booted is kept: kiln/staged/probation/kiln.json goes, so the next start does not roll it
// back. Kiln.Host does this once the mods loaded; kiln-core does it at the end of its boot when .NET is off (kiln.json
// "clr": false), where no mod loads. True when there was a probation to end.
bool confirm_kiln(const std::filesystem::path& game);
// The proxy's lines of this start, kept for kiln-core (it copies them into kiln.log): kiln/staged/proxy-lines.json.
void save_proxy_lines(const std::filesystem::path& game, uint64_t pid, const std::vector<std::pair<Level, std::string>>& lines);
std::vector<std::pair<Level, std::string>> take_proxy_lines(const std::filesystem::path& game, uint64_t pid);

// ---- parts the host test checks one by one ----
// A relative path in a manifest: '/'-separated, no empty, "." or ".." segment, no drive, no ':' (no alternate data
// stream), no character Windows refuses, no segment ending in '.' or ' ', no device name (CON, NUL, COM1 ...), at most
// kMaxRelPath bytes of valid UTF-8.
bool rel_path_ok(const std::string& rel, std::string* why);
// A path a Kiln update may write or remove: dinput8.dll, or under kiln/ but not kiln/staged, kiln/data, kiln/config,
// kiln/logs, kiln/state, kiln/backups, kiln/control or kiln/crash-reports (compared without regard to case).
bool kiln_path_ok(const std::string& rel, std::string* why);
// A folder name under kiln/mods a staged mod may use: one path segment, at most 64 bytes, not starting with '.'.
bool mod_folder_ok(const std::string& folder);

// A start applies a staged update only when the volume of kiln/staged has this much free plus the size of the world's
// savegame folder (enshrouded_server.json saveDirectory, default <game>/savegame); Kiln.Admin's update_stage asks the
// same after its copy. Rollbacks never wait for it.
inline constexpr uint64_t kFreeHeadroom = uint64_t{2} << 30;

// Tests only: the free bytes of the volume holding `dir` (null in Kiln: the volume is asked).
extern uint64_t (*g_free_bytes_hook)(const std::filesystem::path& dir);

// Tests only: called after every change a transaction makes (a rename, a journal write); a test ends the process in it
// to stand for a crash or a power cut at that point. Null in Kiln.
extern void (*g_step_hook)(const char* step);

}  // namespace kiln::staged
