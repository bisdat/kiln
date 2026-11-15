/* SPDX-License-Identifier: MIT */
/* Compile-only: kiln_abilities.h, then kiln_world.h (see world_first.c). */
#include "services/kiln_abilities.h"
#include "services/kiln_world.h"
_Static_assert(sizeof(KilnWorldProgramInfo) == 32, "KilnWorldProgramInfo: 32 bytes");
_Static_assert(sizeof(KilnImpactProgramInfo) == 44, "KilnImpactProgramInfo: 44 bytes");
_Static_assert(KILN_PROGRAM_INFO_ALIAS == 2, "kiln_abilities.h came first: KilnProgramInfo is its struct");
_Static_assert(sizeof(KilnProgramInfo) == sizeof(KilnImpactProgramInfo), "the alias names kiln_abilities.h's struct");
static KilnResult world_info(KilnHandle h, KilnWorldProgramInfo* out) { (void)h; (void)out; return KILN_OK; }
static KilnResult impact_info(uint32_t id, KilnImpactProgramInfo* out) { (void)id; (void)out; return KILN_OK; }
void kiln_abi_headers_abilities_first(KilnWorldApi* w, KilnAbilitiesApi* a) {
    KilnProgramInfo old_name; /* the old name still compiles for kiln_abilities.h's code */
    w->program_info = world_info;
    a->program_info = impact_info;
    old_name.size = sizeof old_name;
    (void)old_name;
}
