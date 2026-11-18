// SPDX-License-Identifier: MIT
// Host test of kiln-shroudforge's log mirror level (src/log_level.h): ShroudForge's own error and warning lines become
// kiln.log errors and warnings. Lines as ShroudForge writes them (modules/logging format_log_line.rs) and as a player's
// kiln.log showed them mirrored (inbox 84). tests/test-log-level.sh builds and runs it.
#include <cstdio>
#include <string_view>

#include "log_level.h"

static int failures = 0;

static void expect(std::string_view line, int32_t want) {
    const int32_t got = ksf::sf_line_level(line);
    if (got != want) {
        std::printf("FAIL %.*s: level %d, want %d\n", int(line.size()), line.data(), got, want);
        ++failures;
    }
}

int main() {
    // ShroudForge's tracing lines
    expect("[E 2026-11-18 17:07:47,591 UTC] [startup-assets] ShroudForge: Unified EML Tweaks failed in the pregame pass", KILN_LOG_ERROR);
    expect("[W 2026-11-18 17:07:47,591 UTC] [network] Network service is waiting for Steam Networking Messages available=false", KILN_LOG_WARN);
    expect("[I 2026-11-18 16:34:22,103 UTC] [startup-assets] Early startup asset pass completed in 557 ms", KILN_LOG_INFO);
    expect("[D 2026-11-18 16:34:22,103 UTC] [dll_manager] Inspected native components", KILN_LOG_INFO);
    expect("[T 2026-11-18 16:34:22,103 UTC] [runtime] tick", KILN_LOG_INFO);
    // a message that only mentions a level letter is not that level
    expect("[I 2026-11-18 16:34:22,103 UTC] [unified-tweaks] [W 2] slot", KILN_LOG_INFO);
    expect("[I 2026-11-18 16:34:22,103 UTC] [mod] Error count 0", KILN_LOG_INFO);
    // the forms the mirror read before
    expect("[E] fake runtime failed", KILN_LOG_ERROR);
    expect("[W] fake runtime slow", KILN_LOG_WARN);
    expect("[I] fake runtime created", KILN_LOG_INFO);
    expect("bootstrap: ERROR could not open", KILN_LOG_ERROR);
    expect("bootstrap: WARN slow disk", KILN_LOG_WARN);
    // too short to be a tag
    expect("[E", KILN_LOG_INFO);
    expect("", KILN_LOG_INFO);
    if (failures) return 1;
    std::printf("sf log level: all lines as expected\n");
    return 0;
}
