// SPDX-License-Identifier: MIT
// kiln.vfx's resource budget and its result-bearing definitions (package 17a S2; chapter 17.1 §5, I1, I5; the C members
// resource_budget, define_vfx_result and vfx_registration of native/core/abi/services/kiln_vfx.h). One ledger for the
// process: boot.cpp tells it what the start built and served (boot), the FxController update what it read of the game's
// VFX table (observe), Kiln.Blocks' calls which mods are loaded (release, activate). The table's functions read and
// decide from it; a reader copies the published snapshot, nothing here reads game memory or calls the game. No Windows:
// the host test runs it (tests/test_ledger.cpp).
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#ifdef KILN_VFX_LEDGER_TEST
#include <functional>
#endif
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "kiln_api.h"
#include "services/kiln_capacity.h"

namespace kvx::ledger {

using Id = std::array<uint8_t, 16>;

constexpr size_t kReportMax = 4096;      // a define or receipt report never needs more: the caller's room is checked first
constexpr size_t kReceiptsMax = 16384;   // refused receipts beyond this are forgotten first (a mod that retries forever)

// A Kiln VFX this start built and served (kvb::BuiltBlock).
struct BuiltVfx {
    Id guid{};                // the game's byte order
    std::string mod, name;
    uint64_t definition = 0;  // the builder's hash of the block's JSON
};

// A definition a mod stored at an earlier start (<data>/vfx/pending/<mod>/<name>.json with its "registration"), as this
// start's build read it back. built: this start built it; otherwise code and why say what refused it.
struct Stored {
    Id registration{}, guid{};
    std::string mod, name;
    uint64_t definition = 0;
    bool built = false;
    uint32_t code = 0;
    std::string why;
};

// What the start's build decided (on_ready).
struct Boot {
    uint32_t admission = 0;     // the capacity the build admitted against: the game's count limit as its code says it
                                // (sigdb caps/vfx.map_limit_request), the cook's own number when that did not resolve
    uint32_t game_vfx = 0;      // the game's own VfxResources
    bool game_counted = false;  // counted in the game's archive (false: the cook's known number, so other loaders' use
                                // cannot be told from the game's)
    std::vector<BuiltVfx> built;
    std::vector<Stored> stored;
    uint32_t reserved = 0;       // external VFX keys admitted by kiln.assets before the cooker started
};

// What a Kiln VFX is in the game's table.
enum Verdict : uint8_t {
    kMissing = 0,    // its GUID is not in the map
    kOwn = 1,        // in the map, and the table's resource holds the bytes Kiln served
    kOther = 2,      // in the map with other bytes (another resource has its GUID)
    kUnchecked = 3,  // in the map; its bytes could not be compared
};

// What the FxController update read of the game's VFX table.
struct Seen {
    uint32_t limit = 0;       // the count limit the game enforces (sys + caps/vfx.map + caps/vfx.map_limit_relative)
    uint64_t count = 0;       // resources in its map
    uint32_t init_state = 0;  // the system's state byte: 2 = ready (its resources are listed)
    std::vector<std::pair<Id, uint8_t>> kiln;  // a Verdict for each BuiltVfx
};

// One define_vfx_result call, after the builder validated the block.
struct Request {
    std::string mod, name;
    Id guid{};
    uint64_t definition = 0;   // as BuiltVfx::definition
    std::string canonical;     // the block's JSON with sorted keys: the registration id hashes it
    std::string block;         // the block's JSON as the builder read it: what a pending file stores
    uint32_t timing = KILN_DEFINE_REQUIRE_REGISTERED;
    bool owner_live = true;    // the caller's owner is not revoked
    std::vector<std::string> warnings;  // the builder's, for the report
};

// ---- the process (vfx.cpp, boot.cpp) ----
#ifdef KILN_VFX_LEDGER_TEST
void set_writer_for_tests(std::function<bool(const std::filesystem::path&, std::string_view)> writer);
#endif
void reset();  // a new process (the host test)
void set_pending_root(const std::filesystem::path& root);  // <data>/vfx/pending
// The capability rows: vfx.budget (the count limit can be read on this build) and blocks.vfx_registration (also Kiln's
// VFX are built this session and their bytes can be compared). Off: the members answer KILN_ERR_UNAVAILABLE.
void set_available(bool budget, bool registration);
bool budget_available();
bool registration_available();
void boot(Boot b);         // once per start, after the build served (also with nothing built)
void forget();             // a prepared build taken back: nothing is built
void observe(const Seen& s);  // the update thread; invalidates the snapshot unless init_state is 2
bool observed();
uint32_t release(const std::string& mod);  // the mod is no longer loaded: how many of its built VFX are kept for it now
void activate(const std::string& mod);     // a block of the mod compiled in this lifetime of it
// The two-argument define_json stored a definition of (mod, name) for the next start: a receipt whose stored definition
// that file held is superseded when the content differs.
void kept(const std::string& mod, const std::string& name, uint64_t definition);

// ---- the C members ----
KilnResult budget(KilnCapacityInfo* out);  // out->size checked by the caller
KilnResult define(const Request& q, KilnDefinitionReply* out, std::string* report);
KilnResult receipt(const uint8_t registration_id[16], KilnDefinitionReply* out, std::string* report);
uint64_t revision();

// ---- shared with boot.cpp ----
// {"reason": null | "...", "warnings": [...]} in at most kReportMax bytes: warnings go first, then the reason is cut.
std::string report_json(std::string reason, std::vector<std::string> warnings);
std::string uuid_text(const Id& id);
bool uuid_parse(std::string_view text, Id* out);
// The registration id of a VFX definition (kiln_capacity_util.hpp registration_id over the SHA-256 of `canonical`).
Id registration_id(const std::string& mod, const Id& guid, std::string_view canonical);
// The file a definition is stored in for the next start: a blocks file of one block with its registration id.
std::string pending_text(const Id& registration, const std::string& name, const std::string& block);
// A block name kiln.vfx stores (it is also the pending file's name): 1 to 128 of letters, digits, '-', '_', '.'.
bool valid_name(std::string_view name);
// A mod id that can be a folder under the pending root: 1 to 64 of letters, digits, '-', '_', '.', not "." or ".."
// (kiln.mod.schema.json: ^[a-z0-9][a-z0-9._-]{0,63}$).
bool valid_mod(std::string_view mod);

}  // namespace kvx::ledger
