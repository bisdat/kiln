#include "../src/services/hooks_patch_policy.h"
#include <cstdio>
#include "services/kiln_hooks.h"
int main() {
    using kiln::hooks::chain::checked_install_result;
    using kiln::hooks::chain::checked_patch_allowed;
    int pass = 0, fail = 0;
    auto check = [&](bool ok, const char* what) {
        std::printf("%s %s\n", ok ? "PASS" : "FAIL", what);
        ok ? ++pass : ++fail;
    };
    check(checked_patch_allowed(0, 0), "every thread checked, none in the bytes: the patch is written");
    check(!checked_patch_allowed(1, 0), "a thread that could not be checked refuses the patch");
    check(!checked_patch_allowed(0, 1), "a thread inside the bytes refuses the patch");
    // detour_typed_checked (since 0.2.0-dev.29): the same policy, its own refusal code for a later attempt
    check(checked_install_result(KILN_ERR_UNAVAILABLE, 1) == KILN_HOOKS_ERR_UNCHECKED,
          "typed: a refusal for unchecked threads is KILN_HOOKS_ERR_UNCHECKED");
    check(checked_install_result(KILN_ERR_UNAVAILABLE, 3) == KILN_HOOKS_ERR_UNCHECKED,
          "typed: ...for any number of them");
    check(checked_install_result(KILN_ERR_UNAVAILABLE, 0) == KILN_ERR_UNAVAILABLE,
          "typed: a refusal with every thread checked keeps its own code (target cap, a detour that cannot be prepared)");
    check(checked_install_result(KILN_HOOKS_ERR_SIG_MISMATCH, 0) == KILN_HOOKS_ERR_SIG_MISMATCH &&
              checked_install_result(KILN_HOOKS_ERR_SIG_UNKNOWN, 0) == KILN_HOOKS_ERR_SIG_UNKNOWN &&
              checked_install_result(KILN_ERR_INVALID_ARGUMENT, 0) == KILN_ERR_INVALID_ARGUMENT,
          "typed: the descriptor's refusals pass through unchanged");
    check(checked_install_result(KILN_OK, 0) == KILN_OK, "typed: an installation stays KILN_OK");
    std::printf("checked_patch_test: %d PASS, %d FAIL\n", pass, fail);
    return fail ? 1 : 0;
}
