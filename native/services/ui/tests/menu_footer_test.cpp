// SPDX-License-Identifier: MIT
// menu_footer.h: the text of Kiln's line in the main menu's footer, its mark's place, when the copy runs.
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "../menu_footer.h"

static int g_fail = 0;
#define CHECK(c)                                                         \
    do {                                                                 \
        if (!(c)) {                                                      \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);     \
            ++g_fail;                                                    \
        }                                                                \
    } while (0)

// The token index of the last "E8" in ui/MainMenuFooter's pattern (sigdb/symbols/client.d/ui.json), -1 when not found.
static int window_call_in_pattern(const char* path) {
    std::ifstream f(path);
    if (!f) return -1;
    std::stringstream ss;
    ss << f.rdbuf();
    const std::string s = ss.str();
    size_t at = s.find("\"ui/MainMenuFooter\": {");
    if (at == std::string::npos) return -1;
    at = s.find("\"pattern\": \"", at);
    if (at == std::string::npos) return -1;
    at += 12;
    const size_t end = s.find('"', at);
    std::stringstream toks(s.substr(at, end - at));
    std::string t;
    int i = 0, last = -1;
    while (toks >> t) {
        if (t == "E8") last = i;
        ++i;
    }
    return last == i - 1 ? last : -1;  // the pattern ends with the window call's E8
}

int main(int argc, char** argv) {
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

    // with XHL-Core: Kiln's line says so (English built in; a translation comes as tmpl)
    CHECK(line_text("0.2.0-dev.29.19", 5, false, {}, true) == "Kiln 0.2.0-dev.29.19 with XHL-Core - 5 Mods Loaded");
    CHECK(line_text("0.2.0-dev.29.19", 1, false, {}, true) == "Kiln 0.2.0-dev.29.19 with XHL-Core - 1 Mod Loaded");
    CHECK(line_text("", 2, false, {}, true) == "Kiln with XHL-Core - 2 Mods Loaded");
    CHECK(line_text("0.2.0", 3, true, {}, true) == seven + "Kiln 0.2.0 with XHL-Core - 3 Mods Loaded");
    CHECK(line_text("0.2.0", 3, false, "Kiln {version} mit XHL-Core - {count} Mods geladen", true) ==
          "Kiln 0.2.0 mit XHL-Core - 3 Mods geladen");
    CHECK(line_text("0.2.0", -1, false, {}, true).empty());
    CHECK(line_key(false) == std::string("native.footer.loaded") && line_key(true) == std::string("native.footer.xhl"));

    // XHL-Core 2.1.0's state getter (dinput8.dll RVA 0x43af0): xor ecx,ecx; xor eax,eax; lock cmpxchg [rip+0xc5664],ecx;
    // ret: an atomic read of the int at RVA 0x109160 (0 = no problem to show)
    const uint8_t getter[] = {0x31, 0xC9, 0x31, 0xC0, 0xF0, 0x0F, 0xB1, 0x0D, 0x64, 0x56, 0x0C, 0x00, 0xC3, 0x0F, 0x1F, 0x00};
    uint64_t state = 0;
    CHECK(xhl_state_getter(getter, sizeof getter, 0x180043af0ull, &state) && state == 0x180109160ull);
    CHECK(!xhl_state_getter(getter, 12, 0x180043af0ull, &state));  // cut short
    uint8_t other[sizeof getter];
    std::memcpy(other, getter, sizeof getter);
    other[12] = 0x90;  // no ret: another function
    CHECK(!xhl_state_getter(other, sizeof other, 0x180043af0ull, &state));
    std::memcpy(other, getter, sizeof getter);
    other[7] = 0x15;  // lock cmpxchg [rip+..], edx: not this getter
    CHECK(!xhl_state_getter(other, sizeof other, 0x180043af0ull, &state));

    // when Kiln's copy calls XHL-Core's footer line: XHL-Core in the footer and a problem to show, or a state Kiln cannot
    // read (then XHL-Core's line shows as XHL-Core draws it: a warning is never hidden)
    CHECK(!follow_xhl(false, true, 3));
    CHECK(!follow_xhl(true, true, 0));
    CHECK(follow_xhl(true, true, 1) && follow_xhl(true, true, 6) && follow_xhl(true, true, -1));
    CHECK(follow_xhl(true, false, 0));

    // the footer's window call: the last byte of ui/MainMenuFooter's pattern, the function's first call
    CHECK(kWindowCallAt == 0x4b);
    if (argc > 1) {
        const int at = window_call_in_pattern(argv[1]);
        if (at != static_cast<int>(kWindowCallAt)) std::printf("ui/MainMenuFooter's pattern ends in E8 at %d\n", at);
        CHECK(at == static_cast<int>(kWindowCallAt));
    }
    std::printf("menu_footer_test: %s\n", g_fail ? "FAIL" : "ok");
    return g_fail ? 1 : 0;
}
