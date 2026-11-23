// SPDX-License-Identifier: MIT
// Host test of the check before kiln-shroudforge starts ShroudForge's own windows (src/helper_version.h): an app older
// than the runtime Kiln runs, or one whose version cannot be read, opens no window and kiln.log says why in one line; the
// same or a newer app starts them as before. tests/test-helper-version.sh builds and runs it.
#include <cstdio>
#include <string>
#include <string_view>

#include "helper_version.h"

static int failures = 0;

static void expect_line(std::string_view app, std::string_view runtime, const std::string& want) {
    const std::string got = ksf::helper_windows_refusal(app, runtime);
    if (got != want) {
        std::printf("FAIL app %.*s, runtime %.*s:\n  got  \"%s\"\n  want \"%s\"\n", int(app.size()), app.data(),
                    int(runtime.size()), runtime.data(), got.c_str(), want.c_str());
        ++failures;
    }
}

static void expect_order(std::string_view a, std::string_view b, int want) {
    const auto x = ksf::parse_sf_version(a), y = ksf::parse_sf_version(b);
    const int got = ksf::compare_sf_versions(x, y);
    if (!x.ok || !y.ok || (got > 0) - (got < 0) != want) {
        std::printf("FAIL %.*s vs %.*s: %d, want %d\n", int(a.size()), a.data(), int(b.size()), b.data(), got, want);
        ++failures;
    }
}

int main() {
    const std::string tail =
        " than the ShroudForge Kiln runs (1.8.1): update ShroudForge to use its Modloader and World Editor windows. Mods "
        "still load.";
    // older: the player's 1.7.9 app beside the bundled 1.8.1 runtime (0.2.0-dev.30.5)
    expect_line("1.7.9", "1.8.1", "ShroudForge's own app (version 1.7.9) is older" + tail);
    expect_line(" 1.7.9\r\n", "1.8.1", "ShroudForge's own app (version 1.7.9) is older" + tail);
    expect_line("1.8.0", "1.8.1", "ShroudForge's own app (version 1.8.0) is older" + tail);
    expect_line("1.8.1-dev.44571709069", "1.8.1", "ShroudForge's own app (version 1.8.1-dev.44571709069) is older" + tail);
    // equal and newer: as before
    expect_line("1.8.1", "1.8.1", "");
    expect_line("v1.8.1", "1.8.1", "");
    expect_line("1.8.1+20261120", "1.8.1", "");
    expect_line("1.8", "1.8.0", "");
    expect_line("1.8.2", "1.8.1", "");
    expect_line("1.10.0", "1.8.1", "");
    expect_line("2.0.0-beta.1", "1.8.1", "");
    // unreadable: no version.json, no "version" in it, or text that is no version
    for (const std::string_view app : {"", " ", "unknown", "1.8.x", "1..1", "1.8.1-", "-1.8.1", "1.8.1-dev..2", "v"})
        expect_line(app, "1.8.1", "ShroudForge's own app (version unknown) may be older" + tail);
    // text that would not be one plain log line is no version either: it never reaches kiln.log
    const std::string long_version = "1.7.9-" + std::string(59, 'a');  // 65 characters
    for (const std::string_view app : {std::string_view("1.7.9-a\nb"), std::string_view("1.7.9 beta"),
                                       std::string_view("1.7.9-\x1b[31m"), std::string_view("1.7.9+\xc3\xa9"),
                                       std::string_view(long_version)})
        expect_line(app, "1.8.1", "ShroudForge's own app (version unknown) may be older" + tail);
    expect_line(long_version.substr(0, 64), "1.8.1",
                "ShroudForge's own app (version " + long_version.substr(0, 64) + ") is older" + tail);
    // a runtime without a readable version (never selected): the windows start as before
    expect_line("1.7.9", "", "");
    // the order the check uses
    expect_order("1.7.9", "1.8.1", -1);
    expect_order("1.9.0", "1.10.0", -1);
    expect_order("1.8.1", "1.8.1.0", 0);
    expect_order("1.8.1-alpha", "1.8.1", -1);
    expect_order("1.8.1-alpha", "1.8.1-alpha.1", -1);
    expect_order("1.8.1-alpha.1", "1.8.1-alpha.beta", -1);
    expect_order("1.8.1-beta.2", "1.8.1-beta.11", -1);
    expect_order("1.8.1-rc.1", "1.8.1-beta.11", 1);
    if (failures) {
        std::printf("%d failure(s)\n", failures);
        return 1;
    }
    std::printf("helper_version_test: all passed\n");
    return 0;
}
