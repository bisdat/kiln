// SPDX-License-Identifier: MIT
// Private kiln-core / kiln-shroudtopia channel over the existing service lookup. Not an SDK service or public ABI.
#pragma once
#include "../../core/abi/kiln_api.h"
namespace kiln::compat {
inline constexpr const char* kOriginalCodeService = "kiln.internal.original-code";
struct OriginalCodeApi {
    KilnServiceHeader header;
    KilnStr (KILN_CALL* rule)(KilnStr mod);  // shroudtopia:<id> -> matching rule id, empty when none
    void (KILN_CALL* used)(uint32_t rva);   // first shim substitution, for kiln.log
};
}
