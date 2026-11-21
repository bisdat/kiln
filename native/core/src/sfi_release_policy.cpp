// SPDX-License-Identifier: MIT
// The release hold of Kiln's interpreter for ShroudForge mods (sfi_release_policy.h).
#include "sfi_release_policy.h"

#include <string>

namespace kiln::sfi_release_policy {
namespace {
// The record of a final ShroudForge 1.8.x in this Kiln build. false: ShroudForge has no final 1.8.x release (1.7.9 is
// the latest final one; 1.8.0 stayed a pre-release). Change it only in a reviewed commit that names the upstream
// release it was checked against, together with UPSTREAM_180_FINAL in crates/kiln-manage/src/sfi_release.rs (a test
// there pins the two to the same value).
constexpr bool kUpstream180Final = false;
}  // namespace

bool upstream_180_final() { return kUpstream180Final; }

bool release_allows_180() { return kUpstream180Final; }

std::vector<gates::Held> forced(const gates::Gate* list) {
    bool interp_named = false, interp_both = false, sf18_both = false;  // what the list holds of each
    for (const gates::Gate* g = list; g && g->name; ++g) {
        if (kInterpreter == g->name) {
            interp_named = true;
            interp_both = interp_both || g->side == gates::kBoth;
        }
        if (kSf18 == g->name && g->side == gates::kBoth) sf18_both = true;
    }
    std::vector<gates::Held> out;  // sorted by name: kInterpreter before kSf18
    // kiln-sfi.interpreter: a row on both sides holds it as any gate row ("preview" lifts it), no row leaves it a normal
    // choice, a row on one side only holds it on both
    if (interp_named && !interp_both) out.push_back({std::string(kInterpreter), std::string(kUnlistedText)});
    // kiln-sfi.sf18: the veto whatever the list holds; with the veto open, its row on both sides still holds it
    if (!release_allows_180())
        out.push_back({std::string(kSf18), std::string(kVetoText)});
    else if (!sf18_both)
        out.push_back({std::string(kSf18), std::string(kUnlistedText)});
    return out;
}

}  // namespace kiln::sfi_release_policy
