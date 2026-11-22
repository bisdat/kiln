// SPDX-License-Identifier: MIT
#pragma once
#include "kiln_api.h"
#include "services/kiln_tools.h"
#include "services/kiln_hooks.h"

// Private integration points, all compiled into kiln-tools.dll. Family publication is load-time only.
bool kiln_tools_bind_programs(KilnOwner owner, const KilnToolFamilyDef& family) noexcept;
void kiln_tools_unbind_programs(KilnOwner owner, uint32_t tool) noexcept;
using KilnToolAreaCheck = bool(*)(uintptr_t build_zones, uint32_t actor, const double center[3], double radius);
bool kiln_tools_impact_install(const KilnApi* api, KilnOwner owner, KilnToolAreaCheck area_check) noexcept;
void kiln_tools_impact_world(uintptr_t authoritative_world) noexcept;
// Global service shutdown only. Stops new policy admissions and waits at most 250 ms. On false leave hooks resident.
bool kiln_tools_impact_close() noexcept;
void kiln_tools_impact_uninstall() noexcept;
bool kiln_tools_actions_ready(uint32_t mode) noexcept;
uint64_t kiln_tools_actions_dispatched() noexcept;
uint64_t kiln_tools_actions_refused() noexcept;
// Production callback exposed privately for native host tests; it never invokes managed code.
uintptr_t KILN_CALL kiln_tools_impact_detour(KilnHookCall2* call, void* user);
