// SPDX-License-Identifier: MIT
// Host test of menu_scroll.h: the pairing of the scroll view's begin and end, when the pause menu is wrapped, the crash
// guard's marker and trial, and the rectangle sanity check.
#include <cmath>
#include <cstring>
#include <cstdio>
#include <limits>
#include <string>

#include "../menu_scroll.h"
#include "services/kiln_ui.h"

using namespace kui::menu_scroll;
namespace rows = kui::menu_rows;

static_assert(rows::show_auto == uint32_t{KILN_UI_MENU_SHOW_AUTO}, "menu_rows.h follows kiln_ui.h (auto)");

namespace {
int failures = 0, checks = 0;
void check(bool ok, const char* what) {
    ++checks;
    if (!ok) { ++failures; std::printf("FAIL %s\n", what); }
}

// ---- a model of the game's pause menu layout, for the decision by height (dev.29) ----
// Numbers, in UI units, from two measurements (nothing here is read from the game's files):
//  - the owner's screenshot at 1280x720 (dev29/reports/2026-11-15-owner-pausemenu): entry frames every 40 px, the
//    first at 20 px, gaps of 53 px and 27 px between the menu's three groups, Quit's frame at 700..740 px (cut);
//  - kiln.log of the first try at 3440x1440 (docs/re/game-ui.md): column 1020, view 311 = (1020 - 87) / 3.
// Both agree with a UI 1080 units high at any resolution (1.5 units a pixel at 720p): column at y 30, 1020 tall, a row
// 60, the two gaps 80 + 40. Below Quit the column holds the game's fixed spacer and the build number (87 + 22 in the
// model: only the screenshot's cut Quit and the first try's view constrain them; the decision does not use them).
struct Look {
    float col_y = 30, space = 1020;
    float pitch = 60, gap1 = 80, gap2 = 40;  // a row; the gaps before the second group (6 rows) and the last (2 rows)
    float trailing = 109;   // below Quit, in the column (not wrapped)
    float view_fixed = 87;  // the column's fixed children beside Kiln's view (wrapped)
    float build = 22;       // the build number, inside Kiln's stack when wrapped
    Look scaled(float s) const {  // every length times s (pixels of another resolution, or another UI scale)
        Look k = *this;
        k.col_y *= s, k.space *= s, k.pitch *= s, k.gap1 *= s, k.gap2 *= s, k.trailing *= s, k.view_fixed *= s,
            k.build *= s;
        return k;
    }
};
// The owner's dev build at 1920x1080 (2026-11-15 screenshot, 1 unit a pixel): rows every 60 from 30, gaps 50 + 25.
Look look_1080() {
    Look k;
    k.gap1 = 50, k.gap2 = 25;
    return k;
}

struct Tree {  // what one builder call built
    bool wrapped = false, probed = false;
    int rows = 0;
};
struct Laid {  // its layout: the column's and the three probes' rectangles (before the first row, after it, after the last)
    float col[4] = {}, top[4] = {}, first[4] = {}, end[4] = {};
};

float rows_height(const Look& k, int rows) { return static_cast<float>(rows) * k.pitch + k.gap1 + k.gap2; }

// The top of row i (from 0) below the first row's top: the second group (6 rows) and the last (2 rows) after a gap.
float row_top(const Look& k, int rows, int i) {
    float y = static_cast<float>(i) * k.pitch;
    if (i >= rows - 8) y += k.gap1;
    if (i >= rows - 2) y += k.gap2;
    return y;
}

// `relative`: the game's rectangles relative to their parent instead of absolute (the probes are siblings either way).
// `cull`: what the owner's dev build showed: not wrapped, what lies below the column is not laid out where it is drawn;
// the probe after Quit reads the top of the first row that ends below the column (978 of 1020 in his kiln.log).
Laid lay_out(const Look& k, const Tree& t, float offset, bool relative = false, bool cull = false) {
    Laid l;
    const float rows_h = rows_height(k, t.rows);
    const float col[4] = {40, k.col_y, 420, k.space};
    std::memcpy(l.col, col, sizeof col);
    float parent_y = 0;  // the probes' parent's top (the column, or Kiln's stack in the view)
    if (!t.wrapped) {
        // the flexible spacers (one above, one below the rows) share what is left; none once the column overflows
        const float flex = std::fmax(0.0f, k.space - rows_h - k.trailing);
        parent_y = k.col_y;
        l.top[1] = k.col_y + flex / 2;
    } else {
        // the view (relative height 1000) and the two flexible spacers (1 each) share the free height
        const float free = k.space - k.view_fixed;
        const float view_y = k.col_y + free / 1002.0f, view_h = free * 1000.0f / 1002.0f;
        const float content = rows_h + k.build;
        const float off = std::fmin(std::fmax(offset, 0.0f), std::fmax(0.0f, content - view_h));
        parent_y = view_y - off;
        l.top[1] = parent_y;
    }
    l.first[1] = l.top[1] + k.pitch;
    l.end[1] = l.top[1] + rows_h;
    if (cull && !t.wrapped)
        for (int i = 0; i < t.rows; ++i)
            if (l.top[1] + row_top(k, t.rows, i) + k.pitch > k.col_y + k.space) {
                l.end[1] = l.top[1] + row_top(k, t.rows, i);
                break;
            }
    if (relative) l.top[1] -= parent_y, l.first[1] -= parent_y, l.end[1] -= parent_y;
    l.top[0] = l.first[0] = l.end[0] = relative ? 0 : 40;
    return l;
}

// One process's pause menu over frames, with the hooks' glue as menu_scroll.cpp and game.cpp have it: the decision at
// the open site (wants), every mod's row in AUTO while the menu can scroll, the probes before the first entry, after it
// and after the entry that was last in the previous call (none in the first call), the rows drawn (the renderer's
// calls and Kiln's rows), and at the close site the rectangles of the previous frame's layout (a node made anew, or
// one whose tree changed, reads zero: it was not laid out).
struct Sim {
    Look look;
    uint32_t mode = rows::show_auto;
    size_t mods = 0;      // mods' rows (a row Kiln.Rules adds, like Creative, is one too: not Kiln's own Mods row)
    int game_rows = 13;   // the game's entries, Return to Game to Quit to Desktop
    Decider d;
    Tree prev;
    Laid prev_laid;
    bool have_prev = false;
    uint32_t last_entries = 0;
    bool wrapped = false, started = false;
    int flips = 0;
    float offset = 0;      // the wheel's scroll offset (wrapped)
    bool broken_probe = false;   // a build where the probes never read (always zero)
    bool relative = false;       // the game's rectangles relative to their parent
    bool cull = false;           // rows below the column not laid out (the owner's dev build)
    float wrapped_probe_scale = 1;  // a layout that disagrees with the model when wrapped (oscillation test)
    uint64_t frame_no = 0;

    int rows_drawn() const {
        return game_rows + 1 + (rows::draws_mods(mode, mods, true) ? static_cast<int>(mods) : 0);
    }

    void frame() {
        ++frame_no;
        const bool w = wants(d.fit, mode, mods);
        if (started && w != wrapped) ++flips;
        wrapped = w, started = true;
        const uint32_t entries = static_cast<uint32_t>(game_rows);  // the renderer's calls (Kiln's rows: inside Settings')
        const bool probed = last_entries == entries;
        last_entries = entries;
        const Tree t{w, probed, rows_drawn()};
        Laid r;  // what the close site reads
        if (have_prev) std::memcpy(r.col, prev_laid.col, sizeof r.col);  // the game's own column: always there
        if (have_prev && prev.wrapped == t.wrapped && prev.probed && t.probed && !broken_probe) {
            std::memcpy(r.top, prev_laid.top, sizeof r.top);
            std::memcpy(r.first, prev_laid.first, sizeof r.first);
            std::memcpy(r.end, prev_laid.end, sizeof r.end);
        }
        measure(d, w, probed, frame_no, r.col, r.top, r.first, r.end, static_cast<uint32_t>(t.rows));
        prev = t;
        prev_laid = lay_out(look, t, offset, relative, cull);
        if (t.wrapped && wrapped_probe_scale != 1) {
            Laid& l = prev_laid;
            l.first[1] = l.top[1] + (l.first[1] - l.top[1]) * wrapped_probe_scale;
            l.end[1] = l.top[1] + (l.end[1] - l.top[1]) * wrapped_probe_scale;
        }
        have_prev = true;
    }
    void run(int n) {
        for (int i = 0; i < n; ++i) frame();
    }
    void closed(uint64_t frames) {  // the menu is closed: no builder calls, the UI frames go on
        frame_no += frames;
        have_prev = false;
    }
};

// Where Quit must end up: wrapped when the rows are taller than the column, drawn as the game draws it when they fit.
bool quit_reachable(const Sim& s) {
    const float rows_h = rows_height(s.look, s.rows_drawn());
    return s.wrapped || rows_h <= s.look.space;
}

std::string label(const char* res, int extra, size_t mods, uint32_t mode, const char* what) {
    char b[160];
    std::snprintf(b, sizeof b, "%s, %d extra rows (%zu mods' rows, mode %u): %s", res, extra, mods, mode, what);
    return b;
}
}  // namespace

int main() {
    // ---- pairing: begin only at the open site, end only at the close site, the one this call began ----
    {
        Pair p;
        check(at_open(p, false) == At::nothing && !p.open, "a menu that fits: nothing at the open site");
        check(at_close(p) == At::nothing, "and nothing at the close site");
        check(at_open(p, true) == At::begin && p.open, "wrapped: begin at the open site");
        check(at_close(p) == At::end && !p.open, "end at the close site");
        check(at_close(p) == At::nothing, "a second close ends nothing");
        // the close site did not run (something the static analysis missed): the next open says so, begins nothing
        check(at_open(p, true) == At::begin, "begin");
        check(at_open(p, true) == At::unbalanced && p.open, "an open without its close is unbalanced");
        check(at_close(p) == At::end, "the late close still ends the open begin");
        // many frames stay balanced
        int begun = 0, ended = 0;
        for (int i = 0; i < 1000; ++i) {
            if (at_open(p, i % 3 != 0) == At::begin) ++begun;
            if (at_close(p) == At::end) ++ended;
        }
        check(begun == ended && begun == 666 && !p.open, "1000 frames: every begin ended once");
    }

    // ---- when the pause menu is wrapped ----
    check(!wants(rows::show_auto, 3) && wants(rows::show_auto, 4), "auto: wrapped past kFew mods' rows");
    check(wants(rows::show_always, 12) && !wants(rows::show_always, 0), "always: wrapped past kFew");
    check(!wants(rows::show_few, 12) && !wants(rows::show_never, 12), "few and never draw few rows: never wrapped");
    check(rows::draws_mods(rows::show_auto, 12, true) && !rows::draws_mods(rows::show_auto, 4, false) &&
              rows::draws_mods(rows::show_auto, 3, false),
          "auto draws every row where the menu scrolls, else the rule few");

    // review of 235bcebb: one decision per builder call. The open site declined (3 mods' rows); a fourth entry is added
    // before the Settings entry is drawn: the rows drawn follow this call's decision (not wrapped: the rule few, none of
    // the four), never every entry into an unwrapped menu.
    // (dev.29: game.cpp passes draws_mods whether the pause menu can scroll in the process, no longer whether this call
    // is wrapped: the wrap follows the rows' height, read the same wrapped or not, so AUTO's rows must not change with
    // it; an unwrapped menu that a new row makes too tall is wrapped from the third call on. menu_rows' rule is as here.)
    {
        Pair p;
        check(at_open(p, wants(rows::show_auto, 3)) == At::nothing, "three rows: not wrapped");
        check(!rows::draws_mods(rows::show_auto, 4, p.open), "a fourth row in the same call: the rule few, not all");
        at_close(p);
        check(at_open(p, wants(rows::show_auto, 4)) == At::begin && rows::draws_mods(rows::show_auto, 4, p.open),
              "the next call wraps and draws all four");
        at_close(p);
    }

    // ---- the owner's dev build at 1920x1080, hosted (2026-11-15, kiln-devbuild.log line 917): 17 rows (the game's 14
    // with Your Server Permissions, SET tools, Creative, Mods), gaps 50 + 25, column 30..1050. The bottom probe after
    // Quit read 978 (the top of Return to Main Menu): Quit starts below the column, and its position (and the probe's
    // after it) is not the one drawn. Quit was below the screen and the menu did not scroll ----
    {
        // his reading as the close site had it: column at 30, 1020 tall; the top probe at 30, the first row 60; the
        // bottom probe at 1008 (978 below the top)
        Decider d;
        const float col[4] = {40, 30, 420, 1020}, top[4] = {40, 30, 0, 0}, first[4] = {40, 90, 0, 0};
        const float end[4] = {40, 1008, 0, 0};
        for (uint64_t f = 1; f <= 10; ++f) measure(d, false, true, f, col, top, first, end, 17);
        check(wants(d.fit, rows::show_auto, 2), "owner 1080p dev build: 17 rows, Quit below the screen: wrapped");
        check(d.fit.known && !d.fit.proven, "978 is less than 17 rows of 60: the bottom probe is not after Quit");
        // the same menu over frames, rows below the column not laid out as in his build, and with them laid out
        for (const bool cull : {true, false}) {
            Sim s;
            s.look = look_1080();
            s.game_rows = 14;  // with Your Server Permissions (a hosted world)
            s.mods = 2;        // SET tools, Creative; and Kiln's Mods: 17 rows, 1095 UI units
            s.cull = cull;
            s.run(300);
            check(s.wrapped && quit_reachable(s), cull ? "owner 1080p, his layout: wrapped, Quit reachable"
                                                       : "owner 1080p, rows laid out: wrapped, Quit reachable");
            check(s.flips == 1, "one change, from the count rule's no");
            s.closed(100);
            s.run(1);
            check(s.wrapped, "reopened: wrapped from its first frame");
        }
        // unhosted (16 rows, no Your Server Permissions) in his layout: Quit's bottom 1065 is below the column (1050)
        Sim u;
        u.look = look_1080();
        u.mods = 2;
        u.cull = true;
        u.run(300);
        check(u.wrapped && quit_reachable(u), "owner 1080p unhosted, 16 rows, his layout: wrapped");
    }
    // ---- dev.29: wrapped by height ----
    // The owner's menu (2026-11-15, 1280x720): the game's 13 entries, Kiln's Mods, Creative (Kiln.Rules: a mods' row
    // to the native side) and SET tools: 16 rows, 2 of them mods' rows. Quit to Desktop was cut; the count rule never
    // wrapped it. The same menu at 1920x1080 (a player's report: Quit almost off the screen).
    {
        struct Res {
            const char* name;
            float height;  // pixels
        };
        const Res owner[] = {{"1280x720 (owner)", 720}, {"1920x1080 (player)", 1080}};
        for (const Res& r : owner) {
            for (const bool pixels : {false, true}) {  // as the code reads them (UI units), and in the screen's pixels
                Sim s;
                s.look = pixels ? Look{}.scaled(r.height / 1080.0f) : Look{};
                s.mods = 2;
                s.run(300);
                check(s.wrapped, label(r.name, 3, 2, s.mode, pixels ? "rows taller than the screen's column (pixels): wrapped"
                                                                    : "rows taller than the column: wrapped").c_str());
                check(quit_reachable(s), label(r.name, 3, 2, s.mode, "Quit to Desktop reachable").c_str());
                check(s.flips == 1, label(r.name, 3, 2, s.mode, "one change, from the count rule's no to the height's yes").c_str());
            }
        }
    }
    // Every resolution asked for, with 1, 2, 3 (the owner's), 4, 8 and 20 rows beyond the game's 13: in UI units the
    // four read the same numbers (a UI 1080 units high), so each is also run in its pixels.
    {
        struct Res {
            const char* name;
            float height;
        };
        const Res all[] = {{"1280x720", 720}, {"1920x1080", 1080}, {"2560x1440", 1440}, {"3440x1440", 1440}};
        struct Case {
            int extra;
            size_t mods;
            bool wrap;  // expected
        };
        // extra 1: Kiln's Mods only (rows 960 of 1020: one row of room, at the limit: drawn as the game draws it); 2: and
        // Creative (rows 1020 of 1020: Quit's bottom on the column's: within one row of it, wrapped); 3: the owner's
        // (1080); 4+: past the column (and past kFew from 5 on). Each also with the rows below the column not laid out.
        const Case cases[] = {{1, 0, false}, {2, 1, true}, {3, 2, true}, {4, 3, true}, {8, 7, true}, {20, 19, true}};
        for (const Res& r : all)
            for (const Case& c : cases)
                for (const uint32_t mode : {rows::show_auto, rows::show_always})
                    for (const bool pixels : {false, true})
                    for (const bool cull : {false, true}) {
                        Sim s;
                        s.look = pixels ? Look{}.scaled(r.height / 1080.0f) : Look{};
                        s.cull = cull;
                        s.mode = mode;
                        s.mods = c.mods;
                        s.run(400);
                        check(s.wrapped == c.wrap, label(r.name, c.extra, c.mods, mode, c.wrap ? "wrapped" : "not wrapped").c_str());
                        check(quit_reachable(s), label(r.name, c.extra, c.mods, mode, "Quit reachable").c_str());
                        check(s.flips <= 1, label(r.name, c.extra, c.mods, mode, "at most one change").c_str());
                    }
        // FEW and NEVER draw no mods' rows past kFew, but the game's and Kiln's rows still decide
        for (const uint32_t mode : {rows::show_few, rows::show_never}) {
            Sim s;
            s.mode = mode, s.mods = 2;  // FEW: both drawn (16 rows); NEVER: none (14 rows)
            s.run(300);
            check(s.wrapped == (mode == rows::show_few), "few: the owner's 16 rows wrapped; never: 14 rows fit");
            Sim t;
            t.mode = mode, t.mods = 12;  // past kFew: no mods' row in either; 14 rows fit
            t.run(300);
            check(!t.wrapped, "few and never with 12 mods' rows draw Kiln's Mods row only: 14 rows fit, not wrapped");
        }
        // a menu that fits is drawn as the game draws it, also in AUTO past kFew (the count rule wrapped it first)
        {
            Sim s;
            s.look = Look{}.scaled(1.0f);
            s.look.pitch = 36;  // a smaller UI scale: 13 + 1 + 5 rows = 804 of 1020
            s.mods = 5;
            s.frame();
            check(s.wrapped, "AUTO, 5 mods' rows, nothing read yet: the count rule wraps");
            s.run(300);
            check(!s.wrapped && s.flips == 1, "the rows fit: unwrapped once read, every row still drawn");
        }
    }
    // UI scales: whatever a row's height, the rows' height against the column's decides
    for (const float scale : {0.8f, 0.9f, 1.1f, 1.25f, 1.4f})
        for (const size_t mods : {size_t{0}, size_t{1}, size_t{2}, size_t{4}, size_t{10}}) {
            Sim s;
            s.look.pitch *= scale, s.look.gap1 *= scale, s.look.gap2 *= scale, s.look.trailing *= scale,
                s.look.build *= scale;
            s.mods = mods;
            s.run(300);
            const float rows_h = rows_height(s.look, s.rows_drawn()), limit = s.look.space - s.look.pitch;
            const bool start = wants(rows::show_auto, mods);  // the count rule decided the first frames
            const bool expect = rows_h > limit + kOver ? true : rows_h + kRoom < limit ? false : start;
            char b[128];
            std::snprintf(b, sizeof b, "UI scale %.2f, %zu mods' rows (rows %.0f of %.0f): decided by height", scale, mods,
                          rows_h, s.look.space);
            check(s.wrapped == expect && s.flips <= 1, b);
        }
    // no oscillation: the wheel moves the view, the menu closes and opens, frames go on
    {
        Sim s;
        s.mods = 2;
        s.run(50);
        int flips = s.flips;
        for (int i = 0; i < 2000; ++i) {
            s.offset = static_cast<float>((i * 37) % 200);
            s.frame();
        }
        check(s.wrapped && s.flips == flips, "2000 frames of wheel scrolling: still wrapped, no change");
        s.closed(500);
        s.run(1);
        check(s.wrapped && s.flips == flips, "the menu opened again: wrapped from its first frame (the decision is kept)");
        s.run(500);
        check(s.flips == flips, "and stays so");
        // a row less (Creative hidden): 15 rows, Quit's bottom on the column's: still past the limit, stays wrapped
        s.mods = 1;
        s.run(300);
        check(s.wrapped && s.flips == flips, "15 rows, past the space minus a row: stays wrapped");
        // two rows less: 14 rows, at the limit: in the band, stays wrapped
        s.mods = 0;
        s.run(300);
        check(s.wrapped && s.flips == flips, "14 rows, in the hysteresis band: stays wrapped");
        // a game entry less too: 13 rows fit with room: unwrapped once, then stays
        s.game_rows = 12;
        s.run(300);
        check(!s.wrapped && s.flips == flips + 1, "13 rows: unwrapped once");
        s.game_rows = 13, s.mods = 2;
        s.run(300);
        check(s.wrapped && s.flips == flips + 2, "16 rows again: wrapped once more (a change of the rows, not an oscillation)");
    }
    {
        // a layout that disagrees with itself (wrapped, the rows read half as tall: they fit; unwrapped, they do not):
        // at most two changes, then wrapped for the rest of the process
        Sim s;
        s.mods = 2;
        s.wrapped_probe_scale = 0.5f;
        s.run(2000);
        check(s.wrapped && s.d.fit.locked, "two readings that disagree: locked wrapped");
        check(s.flips <= 2, "at most two changes before the lock");
    }
    {
        // the limit is the space minus one row; in the band between kRoom below it and kOver above it nothing changes
        Fit f;
        check(fit_step(f, false, 960.0f, 1020, 60, 10) == Change::none && f.known && !f.wrap, "not wrapped, at the limit: stays");
        Fit g;
        check(fit_step(g, true, 950.0f, 1020, 60, 10) == Change::none && g.known && g.wrap, "wrapped, in the band: stays");
        Fit h;
        check(fit_step(h, false, 961, 1020, 60, 10) == Change::wrap && h.wrap, "past the space minus a row: wrap");
        check(fit_step(h, true, 900, 1020, 60, 300) == Change::unwrap && !h.wrap, "fits with room later: unwrap");
        check(fit_step(h, false, 1080, 1020, 60, 330) == Change::lock && h.wrap && h.locked, "back within kFlipWindow: lock");
        check(fit_step(h, true, 500, 1020, 60, 9000) == Change::none && h.wrap, "locked: wrapped whatever it reads");
    }
    // ---- the fallback: no reading ----
    {
        Decider d;
        const float col[4] = {40, 30, 420, 1020}, none[4] = {}, nan[4] = {0, std::numeric_limits<float>::quiet_NaN(), 0, 0};
        const float flat[4] = {40, 30, 420, 0};
        for (uint64_t f = 1; f <= 10; ++f) measure(d, false, true, f, col, nan, nan, nan, 16);
        check(!d.fit.known, "a top probe that reads NaN is no height");
        for (uint64_t f = 11; f <= 20; ++f) measure(d, false, false, f, col, none, none, none, 16);
        check(!d.fit.known, "nor a call without the probes");
        for (uint64_t f = 21; f <= 30; ++f) measure(d, false, true, f, flat, none, none, none, 16);
        check(!d.fit.known, "nor a column of no height");
        check(!wants(d.fit, rows::show_auto, 3) && wants(d.fit, rows::show_auto, 4) && wants(d.fit, rows::show_always, 4) &&
                  !wants(d.fit, rows::show_few, 12),
              "nothing read: the count rule (more than kFew mods' rows in ALWAYS and AUTO)");
        // probes that read zero (never laid out): the rows' count times a 60-unit row, and the count rule's yes besides
        Decider e;
        for (uint64_t f = 1; f <= 10; ++f) measure(e, false, true, f, col, none, none, none, 13);
        check(e.fit.known && !e.fit.wrap && !e.fit.proven, "probes reading zero, 13 rows: 780 of 1020, not wrapped");
        check(!wants(e.fit, rows::show_auto, 3) && wants(e.fit, rows::show_auto, 4), "not proven: the count rule may still wrap");
        Decider g;
        for (uint64_t f = 1; f <= 10; ++f) measure(g, false, true, f, col, none, none, none, 17);
        check(g.fit.wrap && wants(g.fit, rows::show_auto, 0), "probes reading zero, 17 rows: 1020 of 1020, wrapped");
        // a build where the probes never read: the rows' count decides with the count rule
        Sim s;
        s.broken_probe = true;
        s.mods = 2;
        s.run(300);
        check(!s.wrapped && s.d.fit.known && !s.d.fit.proven, "probes unreadable, 16 rows: 960, at the limit: not wrapped");
        Sim t;
        t.broken_probe = true;
        t.mods = 4;
        t.run(300);
        check(t.wrapped && t.flips == 0, "probe unreadable, 4 mods' rows: wrapped from the first frame (the count rule)");
    }
    // ---- a reading is trusted only from the third call in a row of the same state, frame after frame ----
    {
        Track t;
        check(!fresh(t, false, true, 1) && !fresh(t, false, true, 2) && fresh(t, false, true, 3), "the third call reads");
        check(!fresh(t, true, true, 4), "a state change starts again");
        check(!fresh(t, true, true, 5) && fresh(t, true, true, 6), "wrapped: the third reads");
        check(!fresh(t, true, true, 8), "a frame without the menu starts again");
        check(!fresh(t, true, false, 9) && !fresh(t, true, true, 10), "a call without the probe starts again");
    }
    {
        // heights from rectangles: a row from the top probe to the one after the first entry, the rows to the bottom
        // probe when that is at least the rows' count of rows, the space the column's height
        const float col[4] = {40, 30, 420, 1020}, top[4] = {40, 30, 0, 0}, first[4] = {40, 90, 0, 0};
        const float end[4] = {40, 1110, 0, 0};
        const float top_w[4] = {40, -200, 0, 0}, first_w[4] = {40, -140, 0, 0}, end_w[4] = {40, 880, 0, 0};  // scrolled
        const Heights a = heights(col, top, first, end, 17), b = heights(col, top_w, first_w, end_w, 17);
        check(a.ok && a.proven && a.rows == 1080 && a.space == 1020 && a.pitch == 60, "not wrapped: end.y - top.y");
        check(b.ok && b.proven && b.rows == 1080 && b.space == 1020, "wrapped and scrolled: the same");
        const float flat[4] = {40, 30, 420, 0}, zero[4] = {}, owner[4] = {40, 1008, 0, 0};
        check(!heights(flat, top, first, end, 17).ok, "a column of no height is no reading");
        check(!heights(col, top, first, end, 0).ok, "no entries: no reading");
        const Heights z = heights(col, zero, zero, zero, 17);
        check(z.ok && !z.proven && z.pitch == kRowFallback && z.rows == 1020, "probes never laid out: 17 rows of 60");
        const Heights o = heights(col, top, first, owner, 17);
        check(o.ok && !o.proven && o.measured == 978 && o.rows == 1020, "the owner's 978: not proven, 17 rows of 60 decide");
        const Heights up = heights(col, end, first, top, 17);
        check(up.ok && !up.proven && up.measured == 0 && up.rows == 1020,
              "the end above the top is no reading of the rows: 17 rows of 60 decide");
    }
    // the game's rectangles relative to their parents instead of absolute: the same decisions
    for (const size_t mods : {size_t{0}, size_t{1}, size_t{2}, size_t{4}, size_t{19}}) {
        Sim a, b;
        a.mods = b.mods = mods;
        b.relative = true;
        a.run(300);
        b.run(300);
        check(a.wrapped == b.wrapped && a.flips == b.flips && b.d.fit.known, "relative rectangles: the same decision");
    }

    // ---- the crash guard ----
    {
        const std::string m = marker_text("0.2.0-dev.27", "5d9ef2f1");
        Record r;
        check(parse_marker(m, r) && r.kiln == "0.2.0-dev.27" && r.build == "5d9ef2f1", "a marker reads back");
        check(parse_marker("kiln=a\r\nbuild=b\r\n", r) && r.kiln == "a" && r.build == "b", "CRLF lines read");
        check(!parse_marker("", r) && !parse_marker("kiln=a\n", r) && !parse_marker("kiln=\nbuild=b\n", r),
              "an incomplete marker does not read");
        const std::string none;
        check(decide(File::absent, File::absent, none, "v", "b") == Start::try_it, "no file: try");
        check(decide(File::read, File::absent, none, "v", "b") == Start::off_after_crash, "a trying marker: the game ended while trying");
        check(decide(File::read, File::read, m, "0.2.0-dev.27", "5d9ef2f1") == Start::off_after_crash, "trying wins over an off record");
        check(decide(File::read, File::read, m, "0.2.0-dev.28", "5d9ef2f1") == Start::off_after_crash,
              "a trying marker and an old off record (an earlier failed conversion): still off");
        check(decide(File::absent, File::read, m, "0.2.0-dev.27", "5d9ef2f1") == Start::off_recorded, "off: same version and build");
        check(decide(File::absent, File::read, m, "0.2.0-dev.28", "5d9ef2f1") == Start::try_it, "another Kiln version tries again");
        check(decide(File::absent, File::read, m, "0.2.0-dev.27", "0123") == Start::try_it, "another game build tries again");
        const std::string junk = "garbage";
        check(decide(File::absent, File::read, junk, "v", "b") == Start::off_recorded, "an unparsable off record stays off (never a loop)");
        // review of 235bcebb: a marker that is there but cannot be read is not "absent"
        check(decide(File::unreadable, File::absent, none, "v", "b") == Start::off_after_crash, "an unreadable trying marker: off");
        check(decide(File::absent, File::unreadable, none, "v", "b") == Start::off_recorded, "an unreadable off record: off");
        // review of 235bcebb: the trying marker goes only once the off record is on the disk
        check(!may_delete_trying(true, false), "moved: nothing left to delete");
        check(may_delete_trying(false, true), "written: the trying marker may go");
        check(!may_delete_trying(false, false), "neither: the trying marker stays (the evidence is kept)");
    }
    {
        // review of 235bcebb: the trial counts wrapped builder calls that reached their close site, nothing else
        Trial t;
        bool passed = false;
        uint32_t at = 0;
        for (uint32_t k = 1; k <= 100; ++k)
            if (trial_step(t)) {
                check(!passed, "the trial passes once");
                passed = true, at = k;
            }
        check(passed && at == kTrialFrames, "the marker goes on the 48th wrapped frame");
        check(kTrialFrames >= 24 && kTrialFrames <= 120, "a few dozen frames");
        Trial s;
        for (uint32_t k = 1; k < kTrialFrames; ++k) check(!trial_step(s), "fewer wrapped frames keep the marker");
    }
    {
        // dev.29: the height probe is tried too. The marker is written before the first probe; it goes once the probe
        // has run kTrialFrames builder calls
        Guard g;
        check(guard_begin(g, Try::probe) && g.on_disk, "the first probe: write the marker");
        check(!guard_begin(g, Try::probe), "only once");
        for (uint32_t k = 1; k < kTrialFrames; ++k) check(!guard_step(g, Try::probe), "fewer probed calls keep the marker");
        check(guard_step(g, Try::probe) && !g.on_disk, "the 48th probed call: the marker may go");
        check(!guard_step(g, Try::probe), "and only once");
        // the first wrap long after: the marker is written again, and goes after 48 wrapped calls (probes do not count)
        check(guard_begin(g, Try::wrap) && g.on_disk, "the first wrap after the probe's trial: the marker again");
        for (int k = 0; k < 100; ++k) check(!guard_step(g, Try::probe), "probed calls do not pass the wrap's trial");
        for (uint32_t k = 1; k < kTrialFrames; ++k) check(!guard_step(g, Try::wrap), "fewer wrapped calls keep it");
        check(guard_step(g, Try::wrap) && !g.on_disk, "the 48th wrapped call: it goes");
        check(!guard_begin(g, Try::wrap) && !guard_begin(g, Try::probe), "kinds that passed write nothing any more");
        // a wrap that begins during the probe's trial: the marker (already there) stays until both passed
        Guard h;
        check(guard_begin(h, Try::probe), "probe: marker");
        for (int k = 0; k < 10; ++k) guard_step(h, Try::probe);
        check(!guard_begin(h, Try::wrap) && h.on_disk, "the wrap begins while the marker is there: nothing to write");
        bool cleared = false;
        for (uint32_t k = 1; k <= kTrialFrames && !cleared; ++k) {
            cleared = guard_step(h, Try::probe);  // the probe's trial passes on its 48th call
            check(!cleared, "the probe's trial alone does not clear a marker the wrap still needs");
            cleared = guard_step(h, Try::wrap);
        }
        check(cleared && !h.on_disk, "cleared on the wrap's 48th call, both trials passed");
        // the guard never steps a kind that did not begin
        Guard z;
        for (int k = 0; k < 100; ++k) check(!guard_step(z, Try::wrap) && !guard_step(z, Try::probe), "no begin: no marker, nothing to clear");
        // a game that ended while the marker was there: the next start reads it and stays off (as before)
        check(decide(File::read, File::absent, std::string{}, "v", "b") == Start::off_after_crash,
              "a crash during either trial: off at the next start");
    }
    // ---- the guard over a whole process: only a crash in (or right after) Kiln's calls leaves the marker ----
    {
        // menu_scroll.cpp's glue on a fake disk: the marker written before a call, cleared where the guard says
        struct Proc {
            Guard g;
            bool trying = false;         // kiln/state/menu-scroll.trying on the disk
            bool clear_pending = false;  // guard_step passed: after_frame deletes it
            void call(Try k) {
                if (guard_begin(g, k)) trying = true;
            }
            void frame_end() {  // scroll_after_frame
                if (clear_pending) trying = false, clear_pending = false;
                if (guard_frame(g)) trying = false;
            }
            // one UI frame with the pause menu open: both probes, the scroll view when wrapped, the close site
            void menu(bool wrapped = false, bool quit_chosen = false) {
                call(Try::probe), call(Try::probe);
                if (wrapped) call(Try::wrap);
                if (quit_chosen) guard_leaving(g);
                if (guard_step(g, Try::probe)) clear_pending = true;
                if (wrapped && guard_step(g, Try::wrap)) clear_pending = true;
                frame_end();
            }
            void plain(int n) {  // UI frames without the pause menu (the game, the main menu, loading)
                for (int i = 0; i < n; ++i) frame_end();
            }
            void world_stops() {
                if (guard_world_stops(g)) trying = false;
            }
            void exit_clean() {
                if (guard_exit(g)) trying = false;
            }
            // the next start's reading of the disk
            Start next_start() const {
                return decide(trying ? File::read : File::absent, File::absent, std::string{}, "v", "b");
            }
        };
        {
            Proc p;  // the coordinator's case: open, 10 frames, close, a clean exit
            for (int i = 0; i < 10; ++i) p.menu();
            check(p.trying, "10 frames into the trial: the marker is there");
            p.plain(2);
            p.exit_clean();
            check(!p.trying && p.next_start() == Start::try_it, "open, 10 frames, close, clean exit: no off record");
        }
        {
            Proc p;  // the same, killed right after closing (no exit call, no world stop, as on native Windows)
            for (int i = 0; i < 10; ++i) p.menu();
            p.plain(kIdleFrames);
            check(!p.trying && p.next_start() == Start::try_it,
                  "open, 10 frames, close: cleared kIdleFrames frames later, whatever ends the game afterwards");
            for (int i = 0; i < 30; ++i) p.menu();
            check(p.trying && !p.g.trial[0].done, "the next opening writes it again: the trial goes on (40 of 48)");
            for (int i = 0; i < 8; ++i) p.menu();
            check(!p.trying && p.g.trial[0].done, "48 probed frames over two openings: the trial passed, cleared");
            p.menu();
            check(!p.trying, "a passed trial writes nothing at later openings");
        }
        {
            Proc p;  // the coordinator's other case: a crash inside a probe
            p.call(Try::probe);
            check(p.trying && p.next_start() == Start::off_after_crash, "a crash inside the first probe: off record");
            Proc q;
            for (int i = 0; i < 20; ++i) q.menu();
            q.plain(1);
            q.call(Try::probe);  // the next opening's first probe faults
            check(q.trying && q.next_start() == Start::off_after_crash, "a crash in a probe of a later frame: off record");
            Proc r;
            for (int i = 0; i < 20; ++i) r.menu();
            r.plain(kIdleFrames);
            r.call(Try::probe);  // the next opening's first probe faults, after the marker had gone
            check(r.trying && r.next_start() == Start::off_after_crash, "a crash at the next opening: written again, off record");
        }
        {
            Proc p;  // a crash in the frames right after Kiln's calls (the layout or the drawing of their frame)
            for (int i = 0; i < 5; ++i) p.menu();
            p.plain(kIdleFrames - 1);
            check(p.trying && p.next_start() == Start::off_after_crash, "within kIdleFrames of the calls: still a crash");
        }
        {
            Proc p;  // Esc, then Quit to Desktop within the trial, and the process ends right after that frame
            for (int i = 0; i < 6; ++i) p.menu();
            p.menu(false, true);
            check(!p.trying && p.next_start() == Start::try_it, "Quit chosen in the 7th frame: cleared at the frame's end");
            Proc q;  // Quit in the menu's very first frame (a frame nothing has drawn yet): not cleared by it
            q.menu(false, true);
            check(q.trying, "Quit in the first frame of the calls: the marker stays until another rule clears it");
            q.world_stops();
            check(!q.trying && q.next_start() == Start::try_it, "the game destroys its world on the way out: cleared");
        }
        {
            Proc p;  // the world is destroyed while a frame with Kiln's calls is not finished: that frame's end decides
            p.call(Try::probe);
            p.world_stops();
            check(p.trying, "a world stop during a frame with Kiln's calls does not clear it");
            p.exit_clean();
            check(!p.trying, "an exit with code 0 does");
        }
        {
            Proc p;  // the wrap's trial: begun after the probe's passed, counted over openings, a crash in it is an off record
            for (int i = 0; i < 48; ++i) p.menu();
            check(!p.trying, "probe passed");
            for (int i = 0; i < 30; ++i) p.menu(true);
            check(p.trying, "the first wrap writes the marker again");
            p.plain(kIdleFrames);
            check(!p.trying, "the menu closed: cleared");
            p.menu(true);
            check(p.trying, "reopened: written again");
            for (int i = 0; i < 17; ++i) p.menu(true);
            check(!p.trying && p.g.trial[1].done, "48 wrapped frames over two openings: passed, cleared");
            Proc q;
            for (int i = 0; i < 48; ++i) q.menu();
            q.call(Try::probe), q.call(Try::probe), q.call(Try::wrap);  // the first begin faults
            check(q.trying && q.next_start() == Start::off_after_crash, "a crash at the first wrap: off record");
        }
        {
            // the limit, said plainly: a process that ends hard within kIdleFrames of Kiln's calls, with no world
            // stop and no exit call between (TerminateProcess, another program's crash), cannot be told from a crash
            Proc p;
            for (int i = 0; i < 10; ++i) p.menu();
            check(p.next_start() == Start::off_after_crash, "a hard end inside the menu's frames counts as a crash");
        }
    }
    {
        // review of 235bcebb: after a begin, every way out closes what was opened, innermost first
        Close out[2];
        Nest none, layout_only{true, false}, both{true, true};
        check(unwind_order(none, out) == 0, "nothing begun: nothing to close");
        check(unwind_order(layout_only, out) == 1 && out[0] == Close::layout, "only the view begun: end it");
        check(unwind_order(both, out) == 2 && out[0] == Close::inner && out[1] == Close::layout,
              "view and stack: the stack first, then the view");
    }

    // ---- the parameter block: the scrollbar's look and the focus (owner's first try, 2026-11-13) ----
    {
        uint8_t params[kParamsSize] = {}, bar[kScrollBarSize] = {}, cfg[0x10000] = {};
        for (size_t i = 0; i < sizeof cfg; ++i) cfg[i] = static_cast<uint8_t>(i * 7 + 3);
        params[kHotKeyMask] = 1;  // the game's default
        style_params(params, bar, cfg, 0xfa84, 0xfac4, 0xfb04);
        check(std::memcmp(params + kScrollBar + kBarFromCfgA, cfg + 0xfa84, 32) == 0, "bar +0x44: 32 bytes of the config at +0xfa84");
        check(std::memcmp(params + kScrollBar + kBarFromCfgB, cfg + 0xfac4, 32) == 0, "bar +0x24: 32 bytes at +0xfac4");
        check(std::memcmp(params + kScrollBar + kBarFromCfgC, cfg + 0xfb04, 8) == 0, "bar +0x4: 8 bytes at +0xfb04");
        float sizes[4];
        std::memcpy(sizes, params + kScrollBar + kBarSizes, sizeof sizes);
        check(sizes[0] == 24 && sizes[1] == 24 && sizes[2] == 24 && sizes[3] == 20, "bar sizes 24, 24, 24, 20");
        check(params[kScrollBar + kBarFlagA] == 1 && params[kScrollBar + kBarFlagB] == 1, "bar flags +0x7c and +0x90");
        check(params[kUseGlobalFocus] == 1, "the view follows the global focus");
        check(params[kHotKeyMask] == (1 | kHotKeys), "the scroll keys added to the default mask");
        check(kScrollBar + kScrollBarSize == kParamsSize, "the bar block ends the parameter block");
    }
    // ---- the view's height ----
    check(view_too_short(1020, 311, 1522), "the owner's first try (a view a third of its column) is caught");
    check(!view_too_short(1020, 933, 1522), "a view of nearly the column's height passes");
    check(!view_too_short(1020, 311, 300), "entries that fit: no complaint");

    // ---- rectangles ----
    {
        const float ok[4] = {660, 120, 300, 840}, zero_h[4] = {0, 0, 300, 0};
        const float nan[4] = {0, std::numeric_limits<float>::quiet_NaN(), 300, 840}, huge[4] = {0, 0, 300, 1e9f};
        check(sane_rect(ok) && !sane_rect(zero_h) && !sane_rect(nan) && !sane_rect(huge), "rectangle sanity");
    }

    std::printf("menu_scroll_test: %d/%d checks passed\n", checks - failures, checks);
    return failures ? 1 : 0;
}
