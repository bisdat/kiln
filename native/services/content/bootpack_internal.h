// SPDX-License-Identifier: MIT
// Private DLL protocol between kiln.quests and kiln.content before any world starts.
// Not a mod API and not a registered service table; no shared ABI table grows.
#pragma once
#include "kiln_api.h"

namespace kc::bootpack {
inline constexpr const char* kExport = "kc_nc_boot_pack_v1";
enum Operation : uint32_t { Begin = 1, Define = 2, Text = 3, Finish = 4, Abort = 5 };
using Call = KilnResult(KILN_CALL*)(uint32_t operation, KilnOwner owner, uint32_t kind, const char* json, size_t length,
                                  char* report, size_t capacity, size_t* written);
}  // namespace kc::bootpack
