// SPDX-License-Identifier: MIT
// kiln.ui: Kiln's line in the main menu's footer (menu_footer.cpp; docs/kiln/ui.md "Kiln's line in the main menu's
// footer", docs/re/game-ui.md "The main menu's footer"). Plain logic: no Windows, no game memory, so
// tests/menu_footer_test.cpp runs it on the build machine.
//
// The main menu ends with the game's footer (its window "LegalLine": the build number and the legal line, bottom
// aligned). Kiln's line goes right above the build number, where XHL-Core puts its "All mods loaded" line, and takes that
// line's place: Kiln runs its own copy of the footer function (code_clone.h), so a footer line another module patched
// into the game's code is not drawn while Kiln's is.
//
// The line: "Kiln <version> - <N> Mods Loaded" ("1 Mod Loaded"), the version without a "+build" suffix. With the overlay
// drawing, it starts with a gap of no-break spaces where the overlay draws Kiln's mark (the logo is a picture; the game's
// text cannot show one), sized from the line's own rectangle in the game's layout.
//
// With XHL-Core (owner's decision, 2026-11-19): the line reads "Kiln <version> with XHL-Core - <N> Mods Loaded", and
// XHL-Core's own line, its only warning channel (red, clickable: "XHL: Game updated · Click to view", "Mod conflict
// detected. Click to view", which open its diagnostics), is drawn again while XHL-Core reports a problem: Kiln's copy
// then calls XHL-Core's patched targets of the footer's two calls, so XHL-Core draws its line, and takes its clicks, as it
// does without Kiln. Its green "All mods loaded" stays left out (Kiln's line says XHL-Core runs).
#pragma once

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>

#include "ui_text.h"

namespace menu_footer {

// The version a player sees: the release version, without build metadata ("0.2.0-dev.29.11+20261116.ab12" ->
// "0.2.0-dev.29.11"). Empty stays empty.
inline std::string release_version(std::string_view v) {
    const size_t plus = v.find('+');
    std::string_view s = plus == std::string_view::npos ? v : v.substr(0, plus);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\n' || s.back() == '\r')) s.remove_suffix(1);
    return std::string(s);
}

// The game font's metrics (Harmonique Display Regular, font 0 of the game's keen::UiFontResource, the default
// language group's regular face; measured from the font file with fontTools on 2026-11-16): units per em 1000, ascent
// 980, descent 220 (a line is 1.2 em), the space and the no-break space both advance 191 units.
constexpr float kLineEm = 1.2f;
constexpr float kSpaceEm = 0.191f;
// The gap for the mark: 7 no-break spaces (U+00A0) = 1.337 em, the mark is 1 em square, 0.337 em to the text.
constexpr int kGapSpaces = 7;
constexpr float kMarkEm = 1.0f;
constexpr char kNoBreakSpace[] = "\xC2\xA0";

// The plural group of the line's text: with XHL-Core in the footer, the one that names it.
inline const char* line_key(bool xhl) { return xhl ? "native.footer.xhl" : "native.footer.loaded"; }

// The line's text. `mods` < 0: not known yet (no line). `gap`: start with the mark's gap. `tmpl`: the text in the language
// of Kiln's texts (native.footer.loaded.one / .other, or native.footer.xhl.* with `xhl`; ui_text.h), with {version} and
// {count}; empty: English.
inline std::string line_text(std::string_view version, int64_t mods, bool gap, std::string_view tmpl = {}, bool xhl = false) {
    if (mods < 0) return {};
    std::string out;
    if (gap)
        for (int i = 0; i < kGapSpaces; ++i) out += kNoBreakSpace;
    const std::string v = release_version(version);
    const std::string english = kui::text::english(std::string(line_key(xhl)) + (mods == 1 ? ".one" : ".other"));
    const std::string count = std::to_string(static_cast<long long>(mods));
    std::string line = kui::text::fill(tmpl.empty() ? std::string_view(english) : tmpl, {{"version", v}, {"count", count}});
    if (v.empty()) {  // no version (a build without one): no double space where it would be
        size_t at;
        while ((at = line.find("  ")) != std::string::npos) line.erase(at, 1);
        while (!line.empty() && line.front() == ' ') line.erase(0, 1);
    }
    return out + line;
}

// A rectangle of the game's layout (UI units: x, y, w, h) that can be a line of text.
inline bool plausible_line(const float r[4]) {
    for (int i = 0; i < 4; ++i)
        if (!std::isfinite(r[i]) || std::fabs(r[i]) > 100000.0f) return false;
    return r[2] > 1.0f && r[3] > 1.0f && r[3] < 400.0f;
}

// Where the overlay draws the mark, in pixels: a square of kMarkEm, at the line's left edge (where the gap is),
// centred on the line. The canvas's origin is the viewport's (vx, vy); `ppu` pixels per UI unit. False: no mark.
struct Box {
    float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
};
inline bool mark_box(const float r[4], float vx, float vy, float ppu, Box* out) {
    if (!plausible_line(r) || !std::isfinite(ppu) || ppu <= 0.0f || !std::isfinite(vx) || !std::isfinite(vy)) return false;
    const float em = r[3] / kLineEm;
    const float side = em * kMarkEm;
    const float cy = r[1] + r[3] * 0.5f;
    out->x0 = vx + r[0] * ppu;
    out->y0 = vy + (cy - side * 0.5f) * ppu;
    out->x1 = out->x0 + side * ppu;
    out->y1 = out->y0 + side * ppu;
    return out->x1 - out->x0 >= 2.0f;
}

// Whether a drawn line is recent enough to draw its mark: the UI frame it was drawn in against the UI frames finished
// since (the render thread lags the UI thread by up to a frame; a page that no longer draws the footer loses the mark
// after two frames).
inline bool recent(uint64_t drawn_frame, uint64_t frames_now) {
    return drawn_frame != 0 && frames_now <= drawn_frame + 2;
}

// What the footer hook does in a frame.
enum class Take {
    original,  // the game's function (with any other module's line in it): Kiln hidden, not ready, or off
    copy,      // Kiln's copy: the game's lines and Kiln's, nobody else's
};
// `ready`: the copy exists and passed its check. `shown`: the player's setting. `mods`: the count, < 0 unknown.
// `failed`: Kiln's line faulted earlier in this process.
inline Take take(bool ready, bool shown, int64_t mods, bool failed) {
    return ready && shown && mods >= 0 && !failed ? Take::copy : Take::original;
}

// The footer's window call (window "LegalLine"): the function's first call, at +0x4b. ui/MainMenuFooter's pattern
// (sigdb/symbols/client.d/ui.json) starts at the function's first byte with fixed lengths and ends with this call's E8,
// so the offset holds on every build the feature resolves on (tests/menu_footer_test.cpp reads the pattern). XHL-Core
// 2.1.0 patches this call too: its detour turns the window's input on while its line can be clicked.
constexpr uint32_t kWindowCallAt = 0x4b;

// XHL-Core's footer state. Its detour of the build-number call (2.1.0: dinput8.dll RVA 0x596a0) reads its startup state
// first, through a getter that is an atomic read of one int: `xor ecx,ecx; xor eax,eax; lock cmpxchg [rip+rel32],ecx;
// ret` (RVA 0x43af0, the int at RVA 0x109160). 0: nothing to report (its green "All mods loaded" once its check is done,
// else no line); not 0: the problem its red line names. Kiln finds the getter by its shape among the first calls the
// detour makes, not by an address, and only reads the int.
constexpr size_t kXhlGetterLen = 13;
constexpr size_t kXhlDetourScan = 0x100;  // how far into the detour the getter's call is looked for
// Whether `b` (n bytes at `addr`) is the getter; `state`: the int's address.
inline bool xhl_state_getter(const uint8_t* b, size_t n, uint64_t addr, uint64_t* state) {
    static const uint8_t head[] = {0x31, 0xC9, 0x31, 0xC0, 0xF0, 0x0F, 0xB1, 0x0D};
    if (n < kXhlGetterLen || std::memcmp(b, head, sizeof head) != 0 || b[12] != 0xC3) return false;
    int32_t rel = 0;
    std::memcpy(&rel, b + 8, 4);
    *state = addr + 12 + static_cast<int64_t>(rel);
    return true;
}

// Whether Kiln's copy calls XHL-Core's footer line this frame: XHL-Core patched the footer and has a problem to report,
// or its state cannot be read (then its line shows as XHL-Core draws it, green or red: a warning is never hidden).
inline bool follow_xhl(bool xhl, bool state_known, int32_t state) { return xhl && (!state_known || state != 0); }

}  // namespace menu_footer
