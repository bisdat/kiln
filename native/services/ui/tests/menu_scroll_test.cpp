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
    float pitch = 60, gaps = 120;
    float trailing = 109;   // below Quit, in the column (not wrapped)
    float view_fixed = 87;  // the column's fixed children beside Kiln's view (wrapped)
    float build = 22;       // the build number, inside Kiln's stack when wrapped
    Look scaled(float s) const {  // every length times s (pixels of another resolution, or another UI scale)
        Look k = *this;
        k.col_y *= s, k.space *= s, k.pitch *= s, k.gaps *= s, k.trailing *= s, k.view_fixed *= s, k.build *= s;
        return k;
    }
};

struct Tree {  // what one builder call built
    bool wrapped = false, probed = false;
    int rows = 0;
};
struct Laid {  // its layout: the column's and the two probes' rectangles (the probe before the first row, after the last)
    float col[4] = {}, top[4] = {}, end[4] = {};
};

float rows_height(const Look& k, int rows) { return static_cast<float>(rows) * k.pitch + k.gaps; }

// `relative`: the game's rectangles relative to their parent instead of absolute (the probes are siblings either way)
Laid lay_out(const Look& k, const Tree& t, float offset, bool relative = false) {
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
    l.end[1] = l.top[1] + rows_h;
    if (relative) l.top[1] -= parent_y, l.end[1] -= parent_y;
    l.top[0] = l.end[0] = relative ? 0 : 40;
    return l;
}

// One process's pause menu over frames, with the hooks' glue as menu_scroll.cpp and game.cpp have it: the decision at
// the open site (wants), every mod's row in AUTO while the menu can scroll, the probes before the first entry and
// after the entry that was last in the previous call (none in the first call), and at the close site the rectangles
// of the previous frame's layout (a node made anew, or one whose tree changed, reads zero: it was not laid out).
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
            std::memcpy(r.end, prev_laid.end, sizeof r.end);
        }
        measure(d, w, probed, frame_no, r.col, r.top, r.end);
        prev = t;
        prev_laid = lay_out(look, t, offset, relative);
        if (t.wrapped && wrapped_probe_scale != 1)
            prev_laid.end[1] = prev_laid.top[1] + (prev_laid.end[1] - prev_laid.top[1]) * wrapped_probe_scale;
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
    return s.wrapped || rows_h <= s.look.space + kOver;
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
        // extra 1: Kiln's Mods only (rows 960 of 1020: fits, as the game draws it); 2: and Creative (rows 1020 of 1020:
        // Quit's bottom on the column's: fits); 3: the owner's (1080); 4+: past the column (and past kFew from 5 on)
        const Case cases[] = {{1, 0, false}, {2, 1, false}, {3, 2, true}, {4, 3, true}, {8, 7, true}, {20, 19, true}};
        for (const Res& r : all)
            for (const Case& c : cases)
                for (const uint32_t mode : {rows::show_auto, rows::show_always})
                    for (const bool pixels : {false, true}) {
                        Sim s;
                        s.look = pixels ? Look{}.scaled(r.height / 1080.0f) : Look{};
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
            s.look.pitch *= scale, s.look.gaps *= scale, s.look.trailing *= scale, s.look.build *= scale;
            s.mods = mods;
            s.run(300);
            const float rows_h = rows_height(s.look, s.rows_drawn());
            const bool start = wants(rows::show_auto, mods);  // the count rule decided the first frames
            const bool expect = rows_h > s.look.space + kOver ? true : rows_h + kRoom < s.look.space ? false : start;
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
        // a row less (Creative hidden): 15 rows, Quit's bottom on the column's: in the band, stays wrapped
        s.mods = 1;
        s.run(300);
        check(s.wrapped && s.flips == flips, "15 rows, in the hysteresis band: stays wrapped");
        // two rows less: 14 rows fit with room: unwrapped once, then stays
        s.mods = 0;
        s.run(300);
        check(!s.wrapped && s.flips == flips + 1, "14 rows: unwrapped once");
        s.mods = 2;
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
        // in the band between kRoom and kOver nothing changes, from either side
        Fit f;
        check(fit_step(f, false, 1020.5f, 1020, 10) == Change::none && f.known && !f.wrap, "not wrapped, in the band: stays");
        Fit g;
        check(fit_step(g, true, 1010.0f, 1020, 10) == Change::none && g.known && g.wrap, "wrapped, in the band: stays");
        Fit h;
        check(fit_step(h, false, 1080, 1020, 10) == Change::wrap && h.wrap, "past the column: wrap");
        check(fit_step(h, true, 900, 1020, 300) == Change::unwrap && !h.wrap, "fits with room later: unwrap");
        check(fit_step(h, false, 1080, 1020, 330) == Change::lock && h.wrap && h.locked, "back within kFlipWindow: lock");
        check(fit_step(h, true, 500, 1020, 9000) == Change::none && h.wrap, "locked: wrapped whatever it reads");
    }
    // ---- the fallback: no reading ----
    {
        Decider d;
        const float col[4] = {40, 30, 420, 1020}, none[4] = {}, nan[4] = {0, std::numeric_limits<float>::quiet_NaN(), 0, 0};
        for (uint64_t f = 1; f <= 10; ++f) measure(d, false, true, f, col, none, none);
        check(!d.fit.known, "a probe that reads zero is no height");
        for (uint64_t f = 11; f <= 20; ++f) measure(d, false, true, f, col, none, nan);
        check(!d.fit.known, "nor one that reads NaN");
        for (uint64_t f = 21; f <= 30; ++f) measure(d, false, false, f, col, none, none);
        check(!d.fit.known, "nor a call without the probe");
        check(!wants(d.fit, rows::show_auto, 3) && wants(d.fit, rows::show_auto, 4) && wants(d.fit, rows::show_always, 4) &&
                  !wants(d.fit, rows::show_few, 12),
              "nothing read: the count rule (more than kFew mods' rows in ALWAYS and AUTO)");
        // a build where the probe never reads: the count rule for the whole process
        Sim s;
        s.broken_probe = true;
        s.mods = 2;
        s.run(300);
        check(!s.wrapped && !s.d.fit.known, "probe unreadable, 2 mods' rows: not wrapped (the count rule)");
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
        // heights from rectangles: the rows from the first probe's top to the second's, the space the column's height
        const float col[4] = {40, 30, 420, 1020}, top[4] = {40, 30, 0, 0}, end[4] = {40, 1110, 0, 0};
        const float top_w[4] = {40, -200, 0, 0}, end_w[4] = {40, 880, 0, 0};  // wrapped, scrolled by 231
        const Heights a = heights(col, top, end), b = heights(col, top_w, end_w);
        check(a.ok && a.rows == 1080 && a.space == 1020, "not wrapped: end.y - top.y");
        check(b.ok && b.rows == 1080 && b.space == 1020, "wrapped and scrolled: the same");
        const float flat[4] = {40, 30, 420, 0}, zero[4] = {};
        check(!heights(flat, top, end).ok, "a column of no height is no reading");
        check(!heights(col, zero, zero).ok, "probes never laid out (zero) are no reading");
        check(!heights(col, end, top).ok, "the end above the top is no reading");
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
        check(!guard_begin(g, Try::wrap) && !guard_begin(g, Try::probe), "nothing begins twice");
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
