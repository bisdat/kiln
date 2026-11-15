// SPDX-License-Identifier: MIT
// Compile-only, C++: each header alone gives KilnProgramInfo as its own struct (old code keeps compiling), and both
// together in either order (the .c files) give each struct its own name.
#include <type_traits>
#if defined(KILN_TEST_ABILITIES_ONLY)
#include "services/kiln_abilities.h"
static_assert(std::is_same_v<KilnProgramInfo, KilnImpactProgramInfo>, "kiln_abilities.h alone: KilnProgramInfo is its struct");
#elif defined(KILN_TEST_WORLD_ONLY)
#include "services/kiln_world.h"
static_assert(std::is_same_v<KilnProgramInfo, KilnWorldProgramInfo>, "kiln_world.h alone: KilnProgramInfo is its struct");
#elif defined(KILN_TEST_ABILITIES_FIRST)
#include "services/kiln_abilities.h"
#include "services/kiln_world.h"
static_assert(std::is_same_v<KilnProgramInfo, KilnImpactProgramInfo> && !std::is_same_v<KilnWorldProgramInfo, KilnImpactProgramInfo>,
              "both, kiln_abilities.h first: each struct its own name, the alias the first header's");
#else
#include "services/kiln_world.h"
#include "services/kiln_abilities.h"
static_assert(std::is_same_v<KilnProgramInfo, KilnWorldProgramInfo> && !std::is_same_v<KilnWorldProgramInfo, KilnImpactProgramInfo>,
              "both, kiln_world.h first: each struct its own name, the alias the first header's");
#endif
#if !defined(KILN_TEST_ABILITIES_ONLY) && !defined(KILN_TEST_WORLD_ONLY)
static_assert(std::is_same_v<decltype(KilnAbilitiesApi::program_info), KilnResult(KILN_CALL*)(uint32_t, KilnImpactProgramInfo*)> &&
              std::is_same_v<decltype(KilnWorldApi::program_info), KilnResult(KILN_CALL*)(KilnHandle, KilnWorldProgramInfo*)>,
              "each table takes its own struct");
#endif
