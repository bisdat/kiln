// SPDX-License-Identifier: MIT
// The release hold of Kiln's interpreter for ShroudForge mods.
//
// The client gate's list (native/core/gates/sfi.json) holds kiln-sfi.sf18 (anything that needs ShroudForge 1.8) and,
// while Kiln's interpreter as a ShroudForge backend is a preview, kiln-sfi.interpreter; kiln.json "preview" lifts them
// like every gate row. A list with no kiln-sfi.interpreter row makes the interpreter a normal choice (Settings'
// ShroudForge card, kiln.json shroudforge.backend: "kiln"); hosted ShroudForge stays the default. On top of the list,
// this policy holds, and "preview" never lifts:
//   - kiln-sfi.sf18 while this build records no final ShroudForge 1.8.x: the immutable veto. The record is
//     kUpstream180Final in the .cpp, false in every release and preview build; only a reviewed change of that constant,
//     in a new Kiln build, opens the veto. Nothing at run time does: kiln.json, a profile, a launch plan's
//     upstream180Final and the environment are never read here. Whatever the list holds, the veto holds;
//   - kiln-sfi.interpreter when the list holds it on one side only: held on both sides instead of opened on the other
//     (a malformed list already stops the build: gates/gen.py);
//   - kiln-sfi.sf18 in a build whose veto is open, when the list does not hold it on both sides.
// gates.cpp adds these rows to what the list holds after "preview" (each replaces the list's row of the same name), so
// kiln.caps reports them unavailable for policy with a text that begins with KILN_CAP_GATE_TEXT (kiln_caps_held() is
// true for them), KilnApi.feature() answers false for them, and launch_boot.cpp sees kiln-sfi.interpreter held.
//
// Portable C++ (no windows.h): native/core/tests/test-sfi-release-policy.sh builds it on the host as it is.
#pragma once
#include <string_view>
#include <vector>

#include "gates.h"

namespace kiln::sfi_release_policy {

inline constexpr std::string_view kInterpreter = "kiln-sfi.interpreter";
inline constexpr std::string_view kSf18 = "kiln-sfi.sf18";
// The texts of the policy's rows. Both begin with KILN_CAP_GATE_TEXT; kiln-manage's sfi_release.rs uses the same words.
// kUnlistedText: kiln-sfi.interpreter held on one side only by the list, or kiln-sfi.sf18 not on both sides (veto open).
inline constexpr std::string_view kVetoText =
    "game test pending: the release policy (this Kiln build records no final ShroudForge 1.8.x; kiln.json \"preview\" "
    "does not lift it)";
inline constexpr std::string_view kUnlistedText =
    "game test pending: the release policy (this build's gate list does not hold it on both sides; kiln.json "
    "\"preview\" does not lift it)";

// This build records a final ShroudForge 1.8.x: false in every release and preview build.
bool upstream_180_final();
// The kiln-sfi.sf18 veto is open: upstream_180_final(). launch_boot.cpp hands it to decide() as
// Inputs::release_allows_180.
bool release_allows_180();
// The rows this policy holds, on both sides, for the compiled list `list` (gates.h), sorted by name: none for
// kiln-sfi.interpreter when `list` has no row of that name or holds it on both sides. Any thread.
std::vector<gates::Held> forced(const gates::Gate* list);

}  // namespace kiln::sfi_release_policy
