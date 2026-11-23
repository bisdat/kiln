// SPDX-License-Identifier: MIT
// Private DLL protocol between kiln.quests and kiln.assets before any world starts (16f authored roster bodies).
// Not a mod API and not a registered service table; no shared ABI table grows.
//
// kiln.quests' per-mod boot pack registers an admitted body's cooked resources and blobs for that mod's own
// process-lifetime owner (bootpack:<mod>) with every check a mod's assets/assets.json gets (contained reads, the cooked
// file policy, a dedicated server's skips). Only "resource" (mode "new") and "blob" entries; paths relative to
// <mod_dir>/assets. KILN_OK only when every entry registered; otherwise the report names the first problem and the
// caller withdraws the whole mod (revert_owner of that owner, the ordinary rollback). Never pins a key.
#pragma once
#include "kiln_api.h"

namespace ka::bootpack {
inline constexpr const char* kExport = "ka_boot_body_v1";
using Call = KilnResult(KILN_CALL*)(KilnOwner owner, const char* mod_dir, size_t dir_length, const char* json, size_t length,
                                    char* report, size_t capacity, size_t* written);
// kiln.assets' verdict on bytes of a mod's assets/cooked that another service reads itself (the NPC body cache,
// assets/cooked/npc-bodies.v1.json): cooktrust.h's cooked_verdict over this game's cook receipts and kiln.json "cooked".
// 0 load, 1 load although this game's cook did not make them ("warn"), 2 refuse ("local", invalid arguments or an internal
// verdict error). An unreadable or linked receipt store supplies no digests: unrecognized bytes still warn under "warn"
// and refuse under "local" ("any" still loads).
inline constexpr const char* kVerdictExport = "ka_cooked_verdict_v1";
using Verdict = int(KILN_CALL*)(const void* data, size_t length);
}  // namespace ka::bootpack
