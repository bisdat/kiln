#include "../src/services/hooks_patch_policy.h"
#include <cstdio>
#include "services/kiln_hooks.h"
int main() {
    using kiln::hooks::chain::checked_patch_allowed;
    bool ok=checked_patch_allowed(0,0) && !checked_patch_allowed(1,0) && !checked_patch_allowed(0,1);
    std::printf("checked_patch_test: %s\n",ok?"3 PASS, 0 FAIL":"FAIL: unsafe patch accepted");
    return ok?0:1;
}
