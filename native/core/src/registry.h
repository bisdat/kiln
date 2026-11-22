// SPDX-License-Identifier: MIT
// Owners and everything they register: tick subscriptions, game-log subscriptions, queued tasks.
#pragma once
#include <atomic>
#include <memory>
#include <string>
#include <vector>

#include "common.h"
#include "owners_internal.h"

namespace kiln::reg {

struct TickSub {
    KilnHandle handle;
    KilnOwner owner;
    int phase;
    KilnTickFn fn;
    void* user;
    std::atomic<bool> live{true};
    uint32_t faults = 0;
};

struct LogSub {
    KilnHandle handle;
    KilnOwner owner;
    KilnLogLineFn fn;
    void* user;
    std::atomic<bool> live{true};
};

KilnOwner owner_create(std::string_view id);
// An owner Kiln.Host makes through its own KilnApi (api_for_managed_host's owner_create, api.cpp): its mods'
// "mod:<manifest id>" owners and its own. kiln.mod-values gives "mod:<id>" the manifest id as namespace only for these;
// a native owner that names itself "mod:<id>" gets a native namespace (services/mod_values.cpp).
KilnOwner owner_create_managed_host(std::string_view id);
// Alive and made by owner_create_managed_host (fixed for the owner's life; owner ids are never reused).
bool owner_managed_host(KilnOwner owner);
std::string owner_name(KilnOwner owner);
bool owner_alive(KilnOwner owner);
void owner_revoke(KilnOwner owner);

// What the managed host admitted an owner to (kiln.internal.owners, owners_internal.h): KILN_OWNER_GRANT_* bits kept
// with the owner and dropped when it is revoked. owner_grant: KILN_ERR_INVALID_ARGUMENT for owner 0, no bit or an
// unknown bit, KILN_ERR_REVOKED for an owner that is not alive. owner_grants: a live owner's bits (0 when none),
// KILN_ERR_REVOKED for one that is not alive, KILN_ERR_INVALID_ARGUMENT for owner 0 or out NULL.
KilnResult owner_grant(KilnOwner owner, uint32_t grants);
KilnResult owner_grants(KilnOwner owner, uint32_t* out);
const KilnOwnersInternalApi* owners_internal_table();

// Owner-retiring observers (kiln.hooks subscribe_owner_retiring, dev.28): a native service that keeps state for other
// owners (leases, queued work) is told when one of them is revoked. owner_revoke() calls every live observer once
// for an owner that was alive, after the owner is marked dead and its own subscriptions are gone, on the revoking
// thread, with no registry lock held, each call under a fault guard (an observer that faults is removed). An
// observer is not called for its own owner's revoke (its subscriptions go with it) and never for owner 0.
// remove_owner_retiring() may be called from inside a callback; it stops future calls, not one already running on
// another thread.
using OwnerRetiringFn = void(KILN_CALL*)(KilnOwner retiring, void* user);
KilnResult add_owner_retiring(KilnOwner observer, OwnerRetiringFn fn, void* user, KilnHandle* out);
KilnResult remove_owner_retiring(KilnHandle handle);
size_t owner_retiring_count();

KilnResult add_tick(KilnOwner owner, int phase, KilnTickFn fn, void* user, KilnHandle* out);
KilnResult add_log(KilnOwner owner, KilnLogLineFn fn, void* user, KilnHandle* out);
KilnResult remove(KilnHandle handle);

// Snapshot of live subscribers; rebuilt only when `version()` changed since the caller's last snapshot.
uint64_t version();
std::vector<std::shared_ptr<TickSub>> tick_subs(int phase);
std::vector<std::shared_ptr<LogSub>> log_subs();
bool log_subs_empty_hint();  // lock-free; may be briefly stale
size_t tick_sub_count(int phase);

// Cross-thread task queue drained on the tick thread at Pre.
KilnResult post(KilnOwner owner, KilnTaskFn fn, void* user);
size_t drain_posts();  // returns how many ran

}  // namespace kiln::reg
