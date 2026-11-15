// SPDX-License-Identifier: MIT
#pragma once
#include "kiln_boot_collector.hpp"

namespace kvx {
// This release has no public boot staging for VFX/materials. Keep ordinary client files in
// on_ready; changing the client predicate here re-enables prepare builds once staging exists.
// Non-client lab routes retain their previous gate-dependent choice; real servers exit earlier.
inline bool boot_builds_in_prepare(uint32_t side, bool held) {
    return side != KILN_SIDE_CLIENT && !held;
}
struct BuildFailure {
    std::string mod, kind, key, reason;
};
inline void record_cook_failures(std::vector<BuildFailure>* failures,
                                 const std::map<std::string, std::vector<std::string>>& errors) {
    if (!failures) return;
    for (const auto& [id, errs] : errors)
        for (const auto& e : errs) {
            const size_t a = e.find(": "), b = a == std::string::npos ? a : e.find(": ", a + 2);
            const std::string key = b == std::string::npos ? id : e.substr(a + 2, b - a - 2);
            failures->push_back({id, "vfx", key, "kiln.vfx: " + e});
        }
}
// The on_ready build has no boot input. Prepare applies the generation's required/excluded policy later.
inline bool held_build_refusal(bool built_on_ready, const std::vector<BuildFailure>& failures, const kiln::boot::Input& input,
                               KilnBootPreparedV1* out, KilnBootIssueV1* issue) {
    if (built_on_ready)
        for (const auto& f : failures)
            if (!input.out(f.mod) && input.strict(f.mod)) {
                kiln::boot::refuse(out, issue, f.mod, f.kind, f.key, f.reason);
                return true;
            }
    return false;
}
}  // namespace kvx
