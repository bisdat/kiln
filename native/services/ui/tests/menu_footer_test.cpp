// SPDX-License-Identifier: MIT
// menu_footer.h: the text of Kiln's line in the main menu's footer, its mark's place, when the copy runs.
#include <cmath>
#include <cstdio>
#include <string>

#include "../menu_footer.h"

static int g_fail = 0;
#define CHECK(c)                                                         \
    do {                                                                 \
        if (!(c)) {                                                      \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);     \
            ++g_fail;                                                    \
        }                                                                \
    } while (0)

int main() {
    using namespace menu_footer;
    // the version a player sees
    CHECK(release_version("0.2.0-dev.29.11") == "0.2.0-dev.29.11");
    CHECK(release_version("0.2.0-dev.29.11+20261116.ab12cd") == "0.2.0-dev.29.11");
    CHECK(release_version("0.2.0-dev\n") == "0.2.0-dev");
    CHECK(release_version("") == "");
    // the line
    CHECK(line_text("0.2.0-dev.29.11+x", 5, false) == "Kiln 0.2.0-dev.29.11 - 5 Mods Loaded");
    CHECK(line_text("0.2.0-dev.29.11", 1, false) == "Kiln 0.2.0-dev.29.11 - 1 Mod Loaded");
    CHECK(line_text("0.2.0-dev.29.11", 0, false) == "Kiln 0.2.0-dev.29.11 - 0 Mods Loaded");
    CHECK(line_text("0.2.0-dev.29.11", 1234, false) == "Kiln 0.2.0-dev.29.11 - 1234 Mods Loaded");
    CHECK(line_text("", 2, false) == "Kiln - 2 Mods Loaded");
    CHECK(line_text("0.2.0", -1, false).empty());  // no count yet: no line
    const std::string gap = line_text("0.2.0", 3, true);
    std::string seven;
    for (int i = 0; i < kGapSpaces; ++i) seven += "\xC2\xA0";
    CHECK(gap == seven + "Kiln 0.2.0 - 3 Mods Loaded");
    // the gap is wider than the mark
    CHECK(kGapSpaces * kSpaceEm > kMarkEm + 0.2f);
    // the mark: 1 em square at the line's left edge, centred on the line, in pixels
    const float r[4] = {100.0f, 1000.0f, 400.0f, 24.0f};  // a line 24 units high: 20 units per em
    Box b;
    CHECK(mark_box(r, 10.0f, 5.0f, 2.0f, &b));
    CHECK(std::fabs(b.x0 - (10.0f + 200.0f)) < 0.01f);
    CHECK(std::fabs((b.x1 - b.x0) - 40.0f) < 0.01f);
    CHECK(std::fabs((b.y1 - b.y0) - 40.0f) < 0.01f);
    CHECK(std::fabs((b.y0 + b.y1) * 0.5f - (5.0f + 1012.0f * 2.0f)) < 0.01f);
    const float bad[4] = {NAN, 0, 10, 10}, flat[4] = {0, 0, 100, 0}, huge[4] = {0, 0, 100, 1000};
    CHECK(!mark_box(bad, 0, 0, 1, &b));
    CHECK(!mark_box(flat, 0, 0, 1, &b));
    CHECK(!mark_box(huge, 0, 0, 1, &b));
    CHECK(!mark_box(r, 0, 0, 0, &b));
    // recent: drawn in the last two UI frames
    CHECK(!recent(0, 0));
    CHECK(recent(10, 10) && recent(10, 12) && !recent(10, 13));
    // when the copy runs
    CHECK(take(true, true, 3, false) == Take::copy);
    CHECK(take(true, true, 0, false) == Take::copy);
    CHECK(take(false, true, 3, false) == Take::original);  // no copy (build, fault at start)
    CHECK(take(true, false, 3, false) == Take::original);  // hidden by the player
    CHECK(take(true, true, -1, false) == Take::original);  // no count yet
    CHECK(take(true, true, 3, true) == Take::original);    // Kiln's line faulted
    std::printf("menu_footer_test: %s\n", g_fail ? "FAIL" : "ok");
    return g_fail ? 1 : 0;
}
