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
}  // namespace ka::bootpack
