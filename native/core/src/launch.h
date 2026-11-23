// SPDX-License-Identifier: MIT
// kiln.launch (abi/services/kiln_launch.h): the ShroudForge backend of this start, decided once and frozen at boot.
// Kiln-internal: the core and Kiln's own loaders use it (the table names ShroudForge backends).
//
//   decide()  the pure validator: kiln.json's setting, the environment override, the held capability, the sf18 veto and
//             the launch plan the profile names -> one Decision. No file, clock, Windows or Kiln call of its own: the
//             plan comes through the reader it is handed (launch_boot.cpp reads the real file; tests hand in bytes).
//   freeze()  once per process, before any native mod, early plugin or service runs (boot.cpp): the record kiln.launch
//             serves from then on, kiln.json native.policy included (launch_boot.cpp sets it from config(), the value
//             native mods follow) and whether the build is verified (boot's own answer; 0 for backend none). Never
//             changes afterwards.
//   table()   kiln.launch v1, served by api.cpp's lookup in every view, safe idle included.
//   state_json()  the same record as kiln/state/launch.json, written once per boot for the launcher (never read here).
//   designate()   the ShroudForge-dialect owner (launch_internal.h): api.cpp's load_one designates the owner of
//                 kiln/plugins/kiln-sfi.dll of this build, backend kiln, at most once; internal_table() serves it as
//                 kiln.internal.launch to Kiln's services only.
//
// Portable C++ (no windows.h): native/core/tests/test-launch.sh builds this file on the host as it is.
#pragma once
#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>

#include "kiln_api.h"
#include "launch_internal.h"
#include "services/kiln_launch.h"

namespace kiln::launch {

inline constexpr std::string_view kInterpreterGate = "kiln-sfi.interpreter";  // the held capability
inline constexpr size_t kPlanMax = 8u << 20;                                   // bytes of one launch plan file
inline constexpr std::string_view kPlanFolder = "launch-plans";                // below <game>/kiln

// The launch plan file, as the reader found it.
struct PlanFile {
    enum State { Read, Missing, Unreadable };
    State state = Missing;
    std::string bytes;  // Read: the whole file (at most kPlanMax + 1 bytes are ever read)
    std::string error;  // Missing / Unreadable: what the reader saw, for the reason text
};
// `name` is "<plan id>.json"; the reader looks for it in <game>/kiln/launch-plans and nowhere else.
using PlanReader = std::function<PlanFile(std::string_view name)>;

struct Inputs {
    // A start where Kiln runs no plugin (or nothing): KILN_LAUNCH_REASON_SAFE_IDLE, _NO_MODS, _PLUGINS_OFF or
    // _PROFILE_INVALID with its text. 0: Kiln runs plugins this start. set_idle() fills it.
    uint32_t idle_code = 0;
    std::string idle_text;
    const std::string* kiln_json = nullptr;    // kiln.json as plain JSON (comments removed); nullptr: none or unreadable
    const std::string* env_runtime = nullptr;  // KILN_SHROUDFORGE_RUNTIME as kiln-shroudforge reads it; nullptr: not set
    std::string_view profile_text;             // kiln.profile's file text (ACTIVE / ONCE); empty for the other modes
    uint32_t side = KILN_SIDE_CLIENT;          // this process (KILN_SIDE_*)
    std::string_view exe_sha256;               // lower-case hex of the game executable
    bool interpreter_held = true;              // interpreter_hold().held
    std::string held_text;                     // interpreter_hold().text
    bool release_allows_180 = false;           // the kiln-sfi.sf18 veto is open (sfi_release_policy.h: never in a release)
    std::function<bool()> eml_r_present;       // checks game-folder dbghelp.dll, only when kiln is selected
    PlanReader read_plan;                      // asked only when kiln is selected and the profile names a plan
};

// The x64 dbghelp.dll carries an EML-R marker in ASCII or UTF-16LE (kiln-manage's engines.rs).
bool eml_r_proxy(std::string bytes);

// The idle class, in this order: safe idle known at boot, an invalid profile, play-once no-mods, kiln.json "plugins":
// false. `safe_idle` is boot.cpp's safe-idle text when it is known before the freeze, else empty.
void set_idle(Inputs& in, std::string_view safe_idle, uint32_t profile_mode, bool plugins_on);

// Whether kiln-sfi.interpreter holds the interpreter back. `gate_held` / `gate_text`: the client gate (gates.h) after
// kiln.json "preview", the release policy's rows included (sfi_release_policy.h): a row on one side only holds it on
// both; a list with no row of it holds nothing (the interpreter is a normal choice).
struct Hold {
    bool held = true;
    std::string text;
};
Hold interpreter_hold(bool gate_held, std::string_view gate_text);

struct Decision {
    uint32_t backend = KILN_LAUNCH_BACKEND_NONE;
    uint32_t reason_code = 0;
    std::string reason;  // one sentence: why this backend (or none)
    std::string notice;  // the player-facing notice (kiln_launch.h notice_copy); empty when there is nothing to say
    bool plan_read = false;  // plan_hash is the SHA-256 of a plan file read for this decision
    std::array<uint8_t, 32> plan_hash{};
    std::string plan_id;     // the plan's id when it was accepted, else empty
    // kiln.json native.policy (KILN_LAUNCH_NATIVE_POLICY_*): not part of the decision. launch_boot.cpp sets it from
    // config() for every decision, undecided() included, before the freeze. "never" until then: a record nobody set
    // refuses package DLLs instead of allowing them.
    uint32_t native_policy = KILN_LAUNCH_NATIVE_POLICY_NEVER;
    // Whether this game build is verified (1) or not (0), as the boot computed it before the freeze: not part of the
    // decision either. launch_boot.cpp sets it for every decision; freeze() keeps 0 for backend none.
    uint32_t build_verified = 0;
};

// No JSON exception leaves it, whatever the input (one while checking the plan refuses the plan); reason and notice
// are UTF-8.
Decision decide(const Inputs& in);
// What freeze_at_boot freezes when reading the inputs or deciding threw after all: backend none,
// KILN_LAUNCH_REASON_UNDECIDED (idle class: each loader keeps its rules from before this table), `what` in the reason.
Decision undecided(std::string_view what);

// "hosted", "installed", "kiln" or "none".
const char* backend_name(uint32_t backend);
// kiln.json's word for a KILN_LAUNCH_NATIVE_POLICY_* value: "always", "verified-only", "never"; "unknown" otherwise.
const char* native_policy_name(uint32_t policy);

// Freezes `d` as this process's record. False, and nothing changes, when a record is already frozen or launch_id is 0.
// `published`, when given, runs once, right after a successful freeze, never after a refused one: launch_boot.cpp
// writes kiln/state/launch.json there, so the file is written once per boot. An exception from `published` is
// swallowed: the record stays frozen and freeze() returns true. Any thread; boot.cpp calls it once, on the boot thread,
// before any native mod, early plugin or service runs.
bool freeze(const Decision& d, uint64_t launch_id, uint64_t freeze_stamp, const std::function<void()>& published = {});
bool frozen();
// The frozen backend (KILN_LAUNCH_BACKEND_NONE before the freeze).
uint32_t backend();

// kiln.launch v1. The same table before and after the freeze; its members answer KILN_ERR_UNAVAILABLE before it.
const KilnLaunchApi* table();

// boot.cpp, once, on the boot thread (launch_boot.cpp, Windows): reads kiln.json, KILN_SHROUDFORGE_RUNTIME, the
// profile, the client gate and the launch plan, decides, freezes and logs one line. After compat's early rules (they
// switch profile entries off), before any native mod, early plugin or service runs. `verified`: boot's build_verified
// answer (the value set_runtime_state serves later; false when it was never asked); `safe_idle`: boot's safe-idle text
// when it is known at that point, else empty.
void freeze_at_boot(uint32_t side, bool verified, const std::string& safe_idle);

// kiln/state/launch.json: the same record for the launcher and kiln-manage (format 1). No code in the process reads
// it. `kiln` is the core's version and build stamp, `pid` this process.
std::string state_json(const Decision& d, uint64_t launch_id, uint64_t freeze_stamp, uint32_t pid, std::string_view kiln);

// The state file's write, retried: on Windows (and under Wine) the replacing rename of write_file_atomic fails with
// ERROR_ACCESS_DENIED while any other handle on the old file is open, a launcher's read included, and launch.json is
// written only once per boot. `write` is tried up to `attempts` times, `sleep(wait_ms)` between tries (about half a
// second in all by default). True when one try succeeded.
inline constexpr int kStateWriteAttempts = 20;
inline constexpr uint32_t kStateWriteWaitMs = 25;
bool write_retrying(const std::function<bool()>& write, const std::function<void(uint32_t)>& sleep,
                    int attempts = kStateWriteAttempts, uint32_t wait_ms = kStateWriteWaitMs);

// ---- the ShroudForge-dialect owner (launch_internal.h) ----
// `file` is <plugins_dir>/kiln-sfi.dll: that name (ASCII case ignored) directly in that folder, after normalising.
bool is_dialect_dll(const std::filesystem::path& file, const std::filesystem::path& plugins_dir);
// Whether api.cpp's load_one needs to look at a plugin at all: frozen with backend kiln, nobody designated yet.
bool wants_dialect_owner();
// Designates `owner` (the owner load_one passes to kiln-sfi.dll's kiln_plugin_main) when the record is frozen with
// backend kiln, `stamp_trusted` (the DLL's kiln_build_stamp() is this core's build identity) and nobody was designated
// before. Never twice, never moved. False, with `why`, otherwise. Any thread.
bool designate(KilnOwner owner, bool stamp_trusted, std::string& why);
KilnOwner dialect_owner();
// kiln.internal.launch v1, served by api.cpp's lookup through the services' view only.
const KilnLaunchInternalApi* internal_table();

// Host tests only: forget the frozen record and the designation.
void reset_for_tests();

}  // namespace kiln::launch
