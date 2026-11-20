// SPDX-License-Identifier: MIT
// kiln.repl.net's item remap (repl.h "remap.cpp").
// Not built yet (package 15 F0): every function answers KILN_ERR_UNAVAILABLE and the capability bits stay 0.
#include "repl.h"

namespace repl {

KilnResult KILN_CALL t_remap_set(const KilnReplItemSite*, uint32_t, const uint32_t*, uint32_t, const KilnReplItemMap*, uint32_t) {
    return KILN_ERR_UNAVAILABLE;
}
KilnResult KILN_CALL t_remap_peers(const uint32_t*, uint32_t) { return KILN_ERR_UNAVAILABLE; }
KilnResult KILN_CALL t_remap_stats(KilnReplRemapStats*) { return KILN_ERR_UNAVAILABLE; }

}  // namespace repl
