// SPDX-License-Identifier: MIT
#pragma once
#include "kiln_api.h"
#include "services/kiln_tools.h"
#include "services/kiln_hooks.h"
#include "action_trace.h"

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
// An unexpected continuation fault permanently closes tool admission, without removing existing undo guards.
bool kiln_tools_actions_faulted() noexcept;
uint64_t kiln_tools_actions_dispatched() noexcept;
uint64_t kiln_tools_actions_refused() noexcept;
// Every exit of an attempted tool stroke, for the tools service's Pre publisher (kiln.log, tools.actions text).
kiln::tools::ActionTrace& kiln_tools_action_trace() noexcept;
// Tool programs bound in this process (two per family with both actions): 0 means no family was published here.
uint32_t kiln_tools_bound_programs() noexcept;
// Production callback exposed privately for native host tests; it never invokes managed code.
uintptr_t KILN_CALL kiln_tools_impact_detour(KilnHookCall2* call, void* user);
