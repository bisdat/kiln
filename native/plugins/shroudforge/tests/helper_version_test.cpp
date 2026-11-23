// SPDX-License-Identifier: MIT
// Host test of the check before kiln-shroudforge starts ShroudForge's own windows (src/helper_version.h): an app older
// than the runtime Kiln runs opens no window and kiln.log says why in one line; the same or a newer app starts them as
// before, and so does one whose version cannot be read, with one line saying they may fail if it is older.
// tests/test-helper-version.sh builds and runs it.
#include <cstdio>
#include <string>
#include <string_view>

#include "helper_version.h"

static int failures = 0;

// `refusal`: the warning said instead of the windows ("": they start); `note`: the info line beside them ("": none)
static void expect_lines(std::string_view app, std::string_view runtime, const std::string& refusal,
                         const std::string& note = "") {
    const std::string got[] = {ksf::helper_windows_refusal(app, runtime), ksf::helper_windows_note(app, runtime)};
    const std::string* want[] = {&refusal, &note};
    const char* what[] = {"refusal", "note"};
    for (int i = 0; i < 2; ++i) {
        if (got[i] == *want[i]) continue;
        std::printf("FAIL %s, app %.*s, runtime %.*s:\n  got  \"%s\"\n  want \"%s\"\n", what[i], int(app.size()),
                    app.data(), int(runtime.size()), runtime.data(), got[i].c_str(), want[i]->c_str());
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
    expect_lines("1.7.9", "1.8.1", "ShroudForge's own app (version 1.7.9) is older" + tail);
    expect_lines(" 1.7.9\r\n", "1.8.1", "ShroudForge's own app (version 1.7.9) is older" + tail);
    expect_lines("1.8.0", "1.8.1", "ShroudForge's own app (version 1.8.0) is older" + tail);
    expect_lines("1.8.1-dev.44571709069", "1.8.1",
                 "ShroudForge's own app (version 1.8.1-dev.44571709069) is older" + tail);
    // equal and newer: as before
    expect_lines("1.8.1", "1.8.1", "");
    expect_lines("v1.8.1", "1.8.1", "");
    expect_lines("1.8.1+20261120", "1.8.1", "");
    expect_lines("1.8", "1.8.0", "");
    expect_lines("1.8.2", "1.8.1", "");
    expect_lines("1.10.0", "1.8.1", "");
    expect_lines("2.0.0-beta.1", "1.8.1", "");
    // unreadable: no version.json, no "version" in it, or text that is no version. The windows start as before (an app
    // newer than its version.json's reader works), with one info line: they may fail if the app is older.
    const std::string unknown =
        "The version of ShroudForge's own app could not be read (shroudforge/version.json): its Modloader and World "
        "Editor windows start as before, but may fail if the app is older than the ShroudForge Kiln runs (1.8.1); if "
        "they do, update ShroudForge.";
    for (const std::string_view app : {"", " ", "unknown", "1.8.x", "1..1", "1.8.1-", "-1.8.1", "1.8.1-dev..2", "v"})
        expect_lines(app, "1.8.1", "", unknown);
    // text that would not be one plain log line is no version either: it never reaches kiln.log
    const std::string long_version = "1.7.9-" + std::string(59, 'a');  // 65 characters
    for (const std::string_view app : {std::string_view("1.7.9-a\nb"), std::string_view("1.7.9 beta"),
                                       std::string_view("1.7.9-\x1b[31m"), std::string_view("1.7.9+\xc3\xa9"),
                                       std::string_view(long_version)})
        expect_lines(app, "1.8.1", "", unknown);
    // the runtime's version, as written, in that line
    expect_lines("", " 1.8.2-dev.7+b1\n", "",
                 "The version of ShroudForge's own app could not be read (shroudforge/version.json): its Modloader "
                 "and World Editor windows start as before, but may fail if the app is older than the ShroudForge Kiln "
                 "runs (1.8.2-dev.7+b1); if they do, update ShroudForge.");
    expect_lines(long_version.substr(0, 64), "1.8.1",
                 "ShroudForge's own app (version " + long_version.substr(0, 64) + ") is older" + tail);
    // a runtime without a readable version (never selected): the windows start as before, with nothing to say
    expect_lines("1.7.9", "", "");
    expect_lines("", "", "");
    expect_lines("1.7.9\nbeta", "1.8.1\nbeta", "");
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
