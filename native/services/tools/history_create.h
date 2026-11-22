// SPDX-License-Identifier: MIT
#pragma once
#include "history_tags.h"

namespace kiln::tools {
// Reserve sidecar capacity before native creation, which can evict an older undo entry. A capacity refusal must
// leave that ring untouched. If a nonzero native result cannot be tagged, discard only its new pending entry;
// the caller assigns our null return to op.pendingHistory, so BuildFinish cannot discard it a second time.
template <size_t Capacity, class Begin, class Discard>
uintptr_t create_history(HistoryTags<Capacity>& tags, Begin begin, Discard discard,
                         uintptr_t allocator, uintptr_t storage, bool policy) {
    auto reservation = policy ? tags.reserve() : typename HistoryTags<Capacity>::Reservation{};
    if (policy && !reservation) return 0;
    const uintptr_t entry = begin(allocator, storage);
    if (!entry) return 0;
    if (policy ? reservation.created(entry) : tags.created(entry, false)) return entry;
    discard(storage);
    return 0;
}
}
