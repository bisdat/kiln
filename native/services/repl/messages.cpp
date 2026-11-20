// SPDX-License-Identifier: MIT
// kiln.repl.net's message hooks and inject (repl.h "messages.cpp").
// Not built yet (package 15 F0): every function answers KILN_ERR_UNAVAILABLE and the capability bits stay 0.
#include "repl.h"

namespace repl {

void messages_start() {}
uint32_t messages_caps() { return 0; }
void messages_pre() {}
void messages_owner_retiring(KilnOwner) {}
KilnResult messages_remove(KilnHandle) { return KILN_ERR_NOT_FOUND; }
KilnResult KILN_CALL t_message_hook(KilnOwner, uint8_t, uint32_t, KilnReplMessageFn, void*, KilnHandle* out) {
    if (out) *out = 0;
    return KILN_ERR_UNAVAILABLE;
}
KilnResult KILN_CALL t_message_inject(uint32_t, uint8_t, const void*, uint32_t) { return KILN_ERR_UNAVAILABLE; }

}  // namespace repl
