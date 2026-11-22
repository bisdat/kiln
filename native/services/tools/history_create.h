// SPDX-License-Identifier: MIT
#pragma once
#include "history_tags.h"

namespace kiln::tools {
// A full sidecar must fail the native history creation, before any subsequent terrain mutation. Discard the entry
// just allocated by the engine (not some preceding entry), so BuildFinish can roll the already-staged inventory
// transaction back normally. The native caller assigns this return to op.pendingHistory before finishing.
template <size_t Capacity, class Begin, class Discard>
uintptr_t create_history(HistoryTags<Capacity>& tags, Begin begin, Discard discard,
                         uintptr_t allocator, uintptr_t storage, bool policy) {
    const uintptr_t entry = begin(allocator, storage);
    if (!entry) return 0;
    if (tags.created(entry, policy)) return entry;
    discard(storage);
    return 0;
}
}
