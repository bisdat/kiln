// SPDX-License-Identifier: MIT
// compat_ticks.h: which EMF mods get on_tick for the server world only.
#include <cstdio>
#include <cstdlib>

#include "compat_ticks.h"

static int g_fail = 0;
static void check(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_fail;
}

int main() {
    using kemf::server_ticks_only;
    check(server_ticks_only("expeditions", {1, 2, 0}), "expeditions 1.2.0 (the release with the race)");
    check(server_ticks_only("expeditions", {1, 0, 0}), "expeditions 1.0.0");
    check(server_ticks_only("expeditions", {1, 2, 90}), "expeditions 1.2.90 (Kiln's diagnostic build)");
    check(!server_ticks_only("expeditions", {1, 2, 91}), "expeditions 1.2.91 gets every world's tick");
    check(!server_ticks_only("expeditions", {1, 3, 0}), "expeditions 1.3.0 gets every world's tick");
    check(!server_ticks_only("expeditions", {2, 0, 0}), "expeditions 2.0.0 gets every world's tick");
    check(!server_ticks_only("expedition", {1, 2, 0}), "another id: every world's tick");
    check(!server_ticks_only("Expeditions", {1, 2, 0}), "ids are lower case: no match for another spelling");
    check(!server_ticks_only("", {0, 0, 0}), "an empty id");
    std::printf("COMPAT TICKS %s\n", g_fail ? "FAIL" : "PASS");
    return g_fail ? EXIT_FAILURE : EXIT_SUCCESS;
}
