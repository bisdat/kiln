// SPDX-License-Identifier: MIT
#pragma once
#include "services/kiln_build.h"
namespace kb {
void block_ready();
void block_caps();
KilnResult KILN_CALL block_at(KilnVec3, KilnBuildBlockInfo*);
KilnResult KILN_CALL find_wall(KilnVec3, KilnVec3, uint32_t, KilnBuildBlockInfo*);
KilnResult KILN_CALL break_box(const KilnBuildBreak*, KilnBuildBreakResult*);
// SEH-only engine frame; no object with a destructor.
bool block_config_read(uintptr_t function, uintptr_t registry, uint8_t id, int64_t hardness_offset,
                       int64_t health_offset, int32_t* hardness, int32_t* health);
}
