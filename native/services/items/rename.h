// SPDX-License-Identifier: MIT
// kiln.items v9 (since 0.2.0-dev.30.3, GitHub issue #112): renaming a container through the game's own rename, queued
// from any thread and run on the tick thread in Pre. ABI: kiln_items.h "renaming a container"; rules without game
// memory in rename_logic.h (host test tests/rename_test.cpp); evidence docs/re/items.md section 14.
#pragma once
#include <cstdint>

#include "kiln_api.h"
#include "services/kiln_items.h"

namespace kitems {

// resolve() decided E.rename_ok; logs what renames need when they are off.
void rename_resolved();
// Tick thread, Pre: runs waiting renames (up to 64 per step), times out ones no server world ran, delivers results.
void rename_pre(uint64_t step);
// The server world closed: waiting renames end DROPPED.
void rename_world_closed();
// cancel(ticket) / cancel_owner(owner): drop callbacks (the renames still run).
bool rename_cancel(uint64_t ticket);
void rename_cancel_owner(KilnOwner owner);

KilnResult rename_submit(KilnOwner owner, const KilnItemsRenameRequest* request, KilnItemsRenameDoneFn done, void* user,
                         uint64_t* out_ticket, KilnItemsRenameResult* out_refusal);
KilnResult rename_status(KilnItemsRenameStatus* out);

}  // namespace kitems
