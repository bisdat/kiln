// SPDX-License-Identifier: MIT
// Private kiln-core / kiln-shroudtopia channel over the existing service lookup. Not an SDK service or public ABI.
#pragma once
#include "../../core/abi/kiln_api.h"
namespace kiln::compat {
inline constexpr uint32_t kOriginalCodeVersion = 2;
inline constexpr const char* kOriginalCodeService = "kiln.internal.original-code";
struct OriginalCodeApi {
    KilnServiceHeader header;
    KilnStr (KILN_CALL* rule)(KilnStr mod);  // shroudtopia:<id> -> matching rule id, empty when none
    void (KILN_CALL* used)(uint32_t rva);   // first shim substitution, for kiln.log
    bool (KILN_CALL* read)(uintptr_t address, uint8_t* out, size_t size); // bounded, relocated file bytes from sigdb
};
}
