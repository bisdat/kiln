/* SPDX-License-Identifier: MIT */
/* Compile-only (test-abi-headers.sh, and the native build's kiln-abi-headers target): kiln_world.h, then
 * kiln_abilities.h. Each header's program-info struct has its own name; KilnProgramInfo is the first header's. */
#include "services/kiln_world.h"
#include "services/kiln_abilities.h"
_Static_assert(sizeof(KilnWorldProgramInfo) == 32, "KilnWorldProgramInfo: 32 bytes, as shipped since 0.2.0-dev.28");
_Static_assert(sizeof(KilnImpactProgramInfo) == 44, "KilnImpactProgramInfo: 44 bytes, as shipped since 0.2.0-dev.28");
_Static_assert(KILN_PROGRAM_INFO_ALIAS == 1, "kiln_world.h came first: KilnProgramInfo is its struct");
_Static_assert(sizeof(KilnProgramInfo) == sizeof(KilnWorldProgramInfo), "the alias names kiln_world.h's struct");
static KilnResult world_info(KilnHandle h, KilnWorldProgramInfo* out) { (void)h; (void)out; return KILN_OK; }
static KilnResult impact_info(uint32_t id, KilnImpactProgramInfo* out) { (void)id; (void)out; return KILN_OK; }
void kiln_abi_headers_world_first(KilnWorldApi* w, KilnAbilitiesApi* a) {
    KilnProgramInfo old_name; /* the old name still compiles for kiln_world.h's code */
    w->program_info = world_info;
    a->program_info = impact_info;
    old_name.size = sizeof old_name;
    struct KilnProgramInfo old_tag; /* C consumers also used the public struct tag. */
    old_tag.size = sizeof old_tag;
    (void)old_tag;
    (void)old_name;
}
