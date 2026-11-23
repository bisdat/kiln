// SPDX-License-Identifier: MIT
// Registry owner cleanup uses the core's existing lifecycle notification, not a world-tick sweep.
#pragma once
#include "services/kiln_hooks.h"
#include "stage.h"

namespace kc::registry::stage {

// The runner and the observer's service live for the process. A revoked service's observer goes with it.
// No game calls or callback-quiescence wait on the revoking thread.
inline KilnResult bind_owner_cleanup(const KilnHooksApi* hooks, KilnOwner observer, Runner* runner) {
    if (!observer || !runner) return KILN_ERR_INVALID_ARGUMENT;
    if (!hooks || hooks->header.size < offsetof(KilnHooksApi, subscribe_owner_retiring) + sizeof(hooks->subscribe_owner_retiring) ||
        !hooks->subscribe_owner_retiring)
        return KILN_ERR_UNAVAILABLE;
    KilnHandle handle = 0;
    return hooks->subscribe_owner_retiring(observer, [](KilnOwner owner, void* user) {
        static_cast<Runner*>(user)->owner_gone(owner);
    }, runner, &handle);
}

}  // namespace kc::registry::stage
