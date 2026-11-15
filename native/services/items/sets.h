// SPDX-License-Identifier: MIT
// kiln.items v6: equipment sets, the service's side (chapter 10 of the dev.28 dossier; native/core/abi/services/
// kiln_items.h "equipment sets"). The snapshot and store are setsnap.h; this file's .cpp is the game glue: the layouts
// through kiln.types, the two readers (GetComponent in Pre, the program's accessors inside kiln_items_equip), the
// window clock and kiln.decisions, and the v6 table functions.
//
// What runs for a server whose mods use no set: nothing but sets_pre's two loads and a size check of an empty store.
// The first v6 call looks up kiln.window_clock, kiln.decisions and kiln.types; no hook is installed by sets at all.
#pragma once
#include <cstdint>

#include "kiln_api.h"
#include "services/kiln_items.h"

namespace kitems {

// The server world this service works in (items.cpp): 0 when none.
uintptr_t items_server_world();

KilnResult sets_capture(KilnOwner owner, uint32_t player, KilnSetSnapshotV1* out);
KilnResult sets_release(KilnOwner owner, uint64_t token);
KilnResult sets_status(KilnSetStatusV1* out);
KilnResult sets_plan(KilnOwner owner, uint64_t token, const KilnSetOpV1* entries, uint32_t count, KilnSetPlanV1* out);

// Tick thread, Pre: expiry, and a reset of the item stamps forgets every snapshot.
void sets_pre(uint64_t step);
// The server world closed or a new one started: every snapshot is forgotten.
void sets_world_closed();
// A native owner goes away (cancel_owner): its snapshots are released.
void sets_cancel_owner(KilnOwner owner);

}  // namespace kitems
