// SPDX-License-Identifier: MIT
// kiln.vfx's builder in the game: reads every mod's VFX blocks at boot and serves what cook/ builds through kiln.assets, before the game
// enumerates its VFX resources. See boot.cpp.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>

#include "kiln_api.h"
#include "held.h"
#include "services/kiln_capacity.h"

namespace kiln::boot {
struct Input;  // kiln_boot_collector.hpp
}

namespace kvx {

// on_ready of the service: the fingerprint, the budget, then every mod in the load order. Nothing here can abort the game: a problem turns
// a block, a mod or all Kiln VFX off with a log line. The client file-build collector retains `failures` for its later prepare;
// only that prepare has the input identifying required participants.
// `boot`: a boot generation's prepare input (chapter 08; kiln_boot_collector.hpp): excluded mods are left out, its vfx documents read.
void build_on_ready(const KilnApi* api, KilnOwner self, const kiln::boot::Input* boot = nullptr,
                    std::vector<BuildFailure>* failures = nullptr);
// A boot generation's first required participant with VFX ("vfx") or materials ("materials") in its own blocks or OnBoot documents:
// {mod, file}, or empty (kiln.vfx's empty-plan prepare refuses it where no VFX are built at all on a client).
std::pair<std::string, std::string> required_vfx_content(const KilnApi* api, const kiln::boot::Input& boot, bool vfx, bool materials);
// Takes back what a boot generation's prepare built and served (the generation was not published).
void build_forget();

// The checks that need the game's resident resources: the shader binding list and that what was served is what the game sees. Call it from
// the FxController update, before the queue runs. Cheap after its one run.
void build_on_update(void* fxc);
void build_world_ended();

// True when this GUID is a Kiln VFX that was turned off after it was registered (a binding list the modules were not made for). The guard
// treats it as unknown.
bool build_refuses(const uint8_t guid[16]);

// How long a built one-shot runs, in ns: the block's delay, duration and longest particle life. 0: not a built VFX, or one that runs until
// stopped. Those keep the rule for any other VFX, so an effect played without a lifetime still ends.
int64_t build_one_shot_ns(const uint8_t guid[16]);

// A definition from code: {"format": 1, "mod": "<id>", "dir": "<mod folder>", "vfx": {...}}, the same blocks as a JSON file. Validates and
// builds it. A block the game already has unchanged is "registered"; any other is kept for the next start ("pending"), as the game only
// enumerates VFX once. report: JSON (report_json plus "status" per block). KILN_ERR_INVALID_ARGUMENT: the document or a block is wrong.
KilnResult build_define_json(KilnOwner owner, KilnStr json, char* report, size_t cap, size_t* out_len);

// JSON: what was built, the budget, the fingerprint.
KilnResult build_status_json(char* buf, size_t cap, size_t* out_len);

// ---- the resource budget (since 0.2.0-dev.29; kiln_vfx.h resource_budget, define_vfx_result, vfx_registration; ledger.h) ----
// After the table is registered, before the mods load: the capability rows vfx.budget and blocks.vfx_registration from what this
// build can read (Game::registry_missing, bytes_missing). The start's build and the guard lower them later when they must.
void budget_start(const KilnApi* api, KilnOwner self);
KilnResult build_resource_budget(KilnCapacityInfo* out);
KilnResult build_define_vfx_result(KilnOwner owner, KilnStr name, KilnStr json, uint32_t timing, KilnDefinitionReply* out, char* report,
                                   size_t cap, size_t* out_len);
KilnResult build_vfx_registration(KilnOwner owner, const uint8_t registration_id[16], KilnDefinitionReply* out, char* report, size_t cap,
                                  size_t* out_len);

}  // namespace kvx
