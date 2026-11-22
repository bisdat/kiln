// SPDX-License-Identifier: MIT
#pragma once
#include "kiln_api.h"
#include "services/kiln_caps.h"
namespace kt {
struct ActionAvailability {uint32_t state,reason;const char* why;};
inline ActionAvailability action_availability(bool host,bool qualified,uint32_t session,bool world,bool faulted=false) {
    if(!host)return {KILN_CAP_UNAVAILABLE,KILN_CAP_REASON_OFFLINE,"no game service host"};
    if(faulted)return {KILN_CAP_UNAVAILABLE,KILN_CAP_REASON_FAULTED,
        "terrain action continuation faulted; further tool actions are disabled until restart"};
    if(!qualified)return {KILN_CAP_UNAVAILABLE,KILN_CAP_REASON_SYMBOL_MISSING,
        "one or more required authoritative terrain action hooks are not installed"};
    if(session==KILN_SESSION_REMOTE)return {KILN_CAP_UNAVAILABLE,KILN_CAP_REASON_WRONG_SIDE,
        "terrain edits are decided by the authoritative host/server, not a joined client"};
    if(!world)return {KILN_CAP_PENDING,KILN_CAP_REASON_SESSION,"no authoritative world session"};
    return {KILN_CAP_AVAILABLE,KILN_CAP_REASON_NONE,""};
}
} // namespace kt
