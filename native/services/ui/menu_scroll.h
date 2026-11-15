// SPDX-License-Identifier: MIT
// kiln.ui: the pause menu in the game's own scroll view (menu_scroll.cpp; docs/re/game-ui.md "Layout and scrolling").
// Plain logic: no Windows, no game memory, so tests/menu_scroll_test.cpp runs it on the build machine.
//
// The game's menus do not scroll, and mods' rows before Settings pushed Quit off a 1080p screen (2026-11-13). The
// pause menu's builder lays its entries out in a column frame (a vertical stack it opens at one place and closes at
// one place, with no way out of the function in between: static analysis of 26430625). Kiln begins the game's own
// scrolling layout (keen::GameUiScrollingLayout, as the credits use it) right after the column is set up and ends it
// right before the column closes, so the entries, the spacers and the build number go into the scroll view.
//
// - Balanced by construction: a begin happens only at the column's open site, an end only at its close site, and the
//   close site ends exactly what the open site of the same builder call began (Pair). An open site that finds a begin
//   still open (the close site did not run: something this analysis missed) says "unbalanced", and the experiment
//   stops for the process.
// - A menu that fits is drawn as it always was: since dev.29 Kiln wraps the column only while the menu's rows are
//   taller than the column (read from the game's layout: "wrap by height" below), the case where Quit leaves the
//   screen. Until a height is read (the first frames; a build where it never reads) the count rule decides: more mods'
//   rows than menu_rows.h's kFew in the modes that draw them all.
// - A crash guard: while Kiln's calls in the game's menu (the height probes, the scroll view) run and have not passed
//   their trial of kTrialFrames builder calls in this process, a "trying" marker is on the disk (a wrong nesting is
//   fatal within the first frames; no wall-clock time). It goes when the trials pass, when the menu has been closed
//   for a few frames or Quit was chosen (the trial goes on at the next opening), and when the game ends in order (a
//   world destroyed, the exit with code 0): Guard below. A start that finds it (the game ended in or right after
//   Kiln's calls) turns it into an "off" record naming the Kiln version and the game build it happened with:
//   scrolling (and its probes) stays off on every start with that version and build; another version or build tries
//   once more. Deleting the file tries again. A marker that exists but cannot be read counts as there (off).
// - After a begin that succeeded, every way out closes what was opened, in order (Nest, unwind_order); when even that
//   faults, nesting cannot be restored and the process ends on purpose (the marker is on the disk: the next start
//   stays off) rather than handing the game a column it would fail on.
#pragma once

#include <cmath>
#include <cstring>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "menu_rows.h"

namespace kui::menu_scroll {

// ---- one builder call: the column's open site and close site ----
struct Pair {
    bool open = false;  // a scroll view was begun at the open site and not ended yet
};

enum class At {
    nothing,     // nothing to do
    begin,       // begin the scroll view now
    end,         // end the scroll view now (the one this call began)
    unbalanced,  // the open site came twice without the close site: stop, begin nothing
};

// The column's open site. `want`: wrap the column in this frame.
constexpr At at_open(Pair& p, bool want) {
    if (p.open) return At::unbalanced;
    if (!want) return At::nothing;
    p.open = true;
    return At::begin;
}

// The column's close site: ends exactly what the open site of this call began.
constexpr At at_close(Pair& p) {
    if (!p.open) return At::nothing;
    p.open = false;
    return At::end;
}

// The count rule (0.2.0-dev.27 and dev.28; since dev.29 only the fallback while no height has been read, see below):
// wrap where the modes that draw every mod's row (ALWAYS, AUTO) have more of them than the rule lets a menu show.
constexpr bool wants(uint32_t mode, size_t mods_rows) {
    return (mode == menu_rows::show_always || mode == menu_rows::show_auto) && mods_rows > menu_rows::kFew;
}

// ---- dev.29: wrap by height ----
// The count rule missed the owner's menu (2026-11-15, 1280x720): the game's 13 entries, Kiln's Creative and Mods and
// SET's tools entry are 16 rows, and Quit to Desktop left the screen with only 2 mods' rows counted. The game's 13
// entries alone nearly fill the column, so what decides is the rows' height, not the mods' count.
//
// What is measured (UI units, the layout of the previous frames: a node's rectangle is the one the game laid out last):
//  - the space: the column's height. The column is as tall as its parent (size {0, 1}) whether Kiln wraps it or not.
//  - the rows: from the top of the first row to the bottom of the last one (Quit). Kiln opens and closes an empty
//    stack of no size (a probe) right before the menu's first entry and another right after its last, as two more
//    children where the entries are (the column, or Kiln's stack in the scroll view): rows = end.y - top.y. Two
//    siblings: the same number whether the game's rectangles are absolute or relative to their parent, with any
//    scroll offset, wrapped or not.
// So both states read the same height for the same rows (the rows do not change with the wrap: AUTO draws every row
// while the menu can scroll). The game's own spacer and build number below Quit do not count.
//
// The owner's dev build (2026-11-15, 1920x1080, hosted, 17 rows) read 978 of 1020 and did not scroll, with Quit below
// the screen: the probe after Quit (which starts below the column) read the top of Return to Main Menu. The entry hook
// sees every entry (Return to Main Menu and Quit are the builder's last two calls of the renderer, 0x140cd69b2 and
// 0x140cd6a0d on 26430625), so the probe was after Quit: what lies below the column is not laid out where it is drawn,
// or not at all. So the bottom probe is believed only where it can be checked:
//  - a third probe right after the first entry gives one row's height (the pitch), near the top where the layout is
//    whole; the rows are at least (entries drawn) x pitch (the game's renderer calls plus Kiln's rows; the gaps between
//    the groups only add);
//  - the bottom probe's reading counts when it is at least that (minus half a row): "proven". Else the rows are the
//    count times the pitch, and the count rule may still wrap (wants: either one).
// And the decision leans to scrolling (a menu that fits but scrolls costs nothing; a cut one loses Quit): wrap when the
// rows are taller than the space minus one row; unwrap only kRoom below that; in between: as it is (hysteresis). A
// bottom probe that reads the top of the first row ending below the column (the owner's case) reads more than the space
// minus one row: it wraps too.
inline constexpr float kOver = 0.0f;    // rows taller than (space - one row) by more than this: wrap
inline constexpr float kTiny = 1.0f;    // a height below this is no reading
inline constexpr float kRoom = 12.0f;   // rows shorter than (space - one row) by more than this: unwrap
inline constexpr float kRowFallback = 60.0f;  // a row (UI units, the owner's 1080p screenshot) when no pitch reads
inline constexpr uint32_t kSettle = 2;  // builder calls in a row before a reading counts: the third reads a laid-out tree
inline constexpr uint64_t kFlipWindow = 120;  // UI frames: a decision that flips back sooner is an oscillation

struct Fit {
    bool known = false;   // a reading decided (else the count rule)
    bool wrap = false;    // the decision once known
    bool proven = false;  // the last reading's bottom was checked against the rows' count (else the count rule may wrap)
    bool locked = false;  // it flipped back within kFlipWindow frames: wrapped for the rest of the process
    uint32_t flips = 0;
    uint64_t flipped_at = 0;  // the UI frame of the last change
};

// Wrap the pause menu's column in this builder call?
// By height once a reading decided (with the count rule's yes besides while the bottom is not proven); the count rule
// until then (the first frames of a process, or a build where the probes read nothing plausible).
constexpr bool wants(const Fit& f, uint32_t mode, size_t mods_rows) {
    if (!f.known) return wants(mode, mods_rows);
    return f.wrap || (!f.proven && wants(mode, mods_rows));
}

// Consecutive builder calls with the probe in the same state (wrapped or not), on consecutive UI frames: a reading is
// trusted from the (kSettle + 1)-th on. A node made in this frame for the first time has no layout yet, and a state
// change moves the nodes: their first rectangles are not this tree's.
struct Track {
    int8_t state = -1;  // 0 not wrapped, 1 wrapped, -1 no probe
    uint64_t frame = 0;
    uint32_t run = 0;
};

constexpr bool fresh(Track& t, bool wrapped, bool probed, uint64_t frame) {
    const int8_t s = probed ? (wrapped ? 1 : 0) : -1;
    if (s >= 0 && s == t.state && frame == t.frame + 1) ++t.run;
    else t.run = s >= 0 ? 1 : 0;
    t.state = s;
    t.frame = frame;
    return t.run > kSettle;
}

// One reading from the rectangles (x, y, w, h): the column's and the three probes' (before the first entry, after it,
// after the last), and `entries`: the rows the builder call drew (the renderer's calls and Kiln's rows).
struct Heights {
    bool ok = false;      // the column and the first row read: a decision can be made
    bool proven = false;  // the bottom probe's reading is at least the rows' count times the pitch
    float rows = 0;       // the height decided with: the bottom probe's when proven, else entries x pitch
    float space = 0;      // the column's height
    float pitch = 0;      // one row (the first entry's), or kRowFallback
    float measured = 0;   // the bottom probe's reading (0: none)
};

inline bool finite4(const float r[4]) {
    for (int i = 0; i < 4; ++i)
        if (!std::isfinite(r[i]) || std::fabs(r[i]) > 100000.0f) return false;
    return true;
}

inline Heights heights(const float col[4], const float top[4], const float first[4], const float end[4],
                       uint32_t entries) {
    Heights h;
    if (!finite4(col) || col[3] <= 0.0f || !finite4(top) || entries == 0) return h;
    h.space = col[3];
    // one row: the first entry's height; a reading that is no row's (never laid out, absurd) leaves the fallback
    const float pitch = finite4(first) ? first[1] - top[1] : 0.0f;
    const bool pitch_ok = pitch > kTiny && pitch < h.space / 2;
    h.pitch = pitch_ok ? pitch : kRowFallback;
    const float least = static_cast<float>(entries) * h.pitch;  // the rows are at least this tall
    if (finite4(end)) {
        const float m = end[1] - top[1];
        if (m > kTiny && m < 100000.0f) h.measured = m;
    }
    h.proven = pitch_ok && h.measured > 0.0f && h.measured + h.pitch / 2 >= least;
    h.rows = h.proven ? h.measured : least > h.measured ? least : h.measured;
    h.ok = true;
    return h;
}

enum class Change { none, wrap, unwrap, lock };

// A trusted reading in UI frame `frame` of a builder call that was `wrapped`: the decision for the next calls (`pitch`:
// one row; the limit is the space minus one row). A change that comes back within kFlipWindow frames of the one
// before means two readings disagree (the layout is not as analysed): wrapped for the rest of the process, the safe
// side (Quit reachable either way).
constexpr Change fit_step(Fit& f, bool wrapped, float rows, float space, float pitch, uint64_t frame) {
    const float limit = space - pitch;
    bool want = wrapped;  // in the band: as it is
    if (f.locked || rows > limit + kOver) want = true;
    else if (rows + kRoom < limit) want = false;
    f.known = true;
    f.wrap = want;
    if (want == wrapped) return Change::none;
    if (f.flips > 0 && frame - f.flipped_at < kFlipWindow) {
        f.locked = true;
        f.wrap = true;
        return wrapped ? Change::none : Change::lock;
    }
    ++f.flips;
    f.flipped_at = frame;
    return want ? Change::wrap : Change::unwrap;
}

// What the close site does with one builder call's rectangles (menu_scroll.cpp; the host test runs the same).
struct Decider {
    Fit fit;
    Track track;
};

struct Measured {
    bool trusted = false;  // a reading of a settled tree
    Heights h;             // its heights (h.ok: plausible)
    Change change = Change::none;
};

// `probed`: the probes were placed in this call, the last after its last entry; `entries`: the rows it drew.
inline Measured measure(Decider& d, bool wrapped, bool probed, uint64_t frame, const float col[4], const float top[4],
                        const float first[4], const float end[4], uint32_t entries) {
    Measured m;
    if (!fresh(d.track, wrapped, probed, frame)) return m;
    m.trusted = true;
    m.h = heights(col, top, first, end, entries);
    if (!m.h.ok) return m;
    d.fit.proven = m.h.proven;
    m.change = fit_step(d.fit, wrapped, m.h.rows, m.h.space, m.h.pitch, frame);
    return m;
}

// ---- what a begin opened, and the order to close it in ----
struct Nest {
    bool layout = false;  // the game's scroll view was begun
    bool inner = false;   // Kiln's vertical stack inside it was opened
};

enum class Close { inner, layout };

// The closes that restore the nesting, innermost first; `n` is how many (0..2).
constexpr int unwind_order(const Nest& nest, Close out[2]) {
    int n = 0;
    if (nest.inner) out[n++] = Close::inner;
    if (nest.layout) out[n++] = Close::layout;
    return n;
}

// ---- the crash guard ----
inline constexpr uint32_t kTrialFrames = 48;  // wrapped builder calls that reached their close site: the trial passed

struct Record {
    std::string kiln, build;  // Kiln's version, the game exe's sha256
};

inline std::string marker_text(std::string_view kiln, std::string_view build) {
    std::string s = "kiln=";
    s.append(kiln);
    s += "\nbuild=";
    s.append(build);
    s += '\n';
    return s;
}

inline bool parse_marker(std::string_view text, Record& out) {
    out = {};
    bool k = false, b = false;
    while (!text.empty()) {
        const size_t nl = text.find('\n');
        std::string_view line = text.substr(0, nl);
        text = nl == std::string_view::npos ? std::string_view{} : text.substr(nl + 1);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        if (line.substr(0, 5) == "kiln=") out.kiln = std::string(line.substr(5)), k = true;
        else if (line.substr(0, 6) == "build=") out.build = std::string(line.substr(6)), b = true;
    }
    return k && b && !out.kiln.empty() && !out.build.empty();
}

enum class Start {
    try_it,           // no marker, or an off record of another version or build (the caller deletes it)
    off_after_crash,  // the "trying" marker of an earlier start: the game ended while trying (it becomes the off record)
    off_recorded,     // an off record of this version and build
};

// What reading a marker file found.
enum class File {
    absent,       // not there (file or folder not found)
    read,         // read; its text is in hand
    unreadable,   // there, but it could not be read (access denied, a sharing error, ...): counts as there
};

// `trying`: the trying marker; `off` and `off_text`: the off record (an unreadable or unparsable off record counts as
// this version and build: off, never a loop).
inline Start decide(File trying, File off, const std::string& off_text, std::string_view kiln, std::string_view build) {
    if (trying != File::absent) return Start::off_after_crash;
    if (off == File::absent) return Start::try_it;
    Record r;
    if (off == File::unreadable || !parse_marker(off_text, r)) return Start::off_recorded;
    return r.kiln == kiln && r.build == build ? Start::off_recorded : Start::try_it;
}

// Turning the trying marker into the off record: the trying marker goes only once the off record is on the disk (moved
// there, or written there), so the evidence is never lost; when neither worked both stay as they are (the next start
// finds the trying marker and stays off again).
constexpr bool may_delete_trying(bool moved, bool off_written) { return !moved && off_written; }

// The trial: once per builder call that reached its close site with Kiln's calls of one kind in it (Guard). Returns
// true once, when that kind has passed.
struct Trial {
    uint32_t frames = 0;
    bool done = false;
};

constexpr bool trial_step(Trial& t) {
    if (t.done) return false;
    if (++t.frames < kTrialFrames) return false;
    t.done = true;
    return true;
}

// Since dev.29 the guard covers two kinds of Kiln's calls in the game's menu: the height probes (every pause-menu frame
// while scrolling is on) and the scroll view (only where the menu is wrapped). Each kind has its own trial of
// kTrialFrames builder calls that reached their close site, counted over the whole process (every opening of the menu
// adds to it). The marker is on the disk only while Kiln's calls of a kind that has not passed run: it is written
// before such a call when it is not there, and it goes
//  - when every kind begun so far has passed its trial (guard_step);
//  - when no such call ran for kIdleFrames UI frames (the menu closed: guard_frame), or at the end of the frame in which
//    the player chose the menu's last entry, Quit, when the frame before already drew the same calls (guard_leaving):
//    the calls are not running any more; the next opening writes it again and the trial goes on;
//  - when a world is being destroyed (guard_world_stops; at frame end if a frame is still running), or exits with code 0
//    (guard_exit): the game ends its session in order, which a crash in Kiln's calls never reaches.
// So only a game that ends while Kiln's calls run, or within kIdleFrames frames of them with nothing orderly in
// between, leaves the marker: the next start keeps scrolling off, as before.
enum class Try : uint8_t { probe = 0, wrap = 1 };

inline constexpr uint32_t kIdleFrames = 8;  // UI frames without Kiln's calls: the frames they were in are drawn

struct Guard {
    Trial trial[2];
    bool begun[2] = {false, false};
    bool on_disk = false;   // the marker is (being) written and not cleared
    bool busy = false;      // Kiln's calls of a kind that has not passed ran in this UI frame
    bool was_busy = false;  // and in the frame before
    bool leaving = false;   // the player chose the menu's last entry (Quit) in this frame
    bool world_stopped = false;  // an orderly world stop during this frame: clear once the frame completes
    uint32_t idle = 0;      // UI frames without such calls, while the marker is on the disk
};

// Before Kiln's call of kind `k` (every one): true when the marker must be written now (the caller writes it before
// the call; when that fails, scrolling goes off).
constexpr bool guard_begin(Guard& g, Try k) {
    const int i = static_cast<int>(k);
    g.begun[i] = true;
    if (g.trial[i].done) return false;
    g.busy = true;
    if (g.on_disk) return false;
    g.on_disk = true;
    return true;
}

// A builder call in which Kiln's calls of kind `k` ran reached its close site: true once the marker may go (every kind
// begun so far has passed its trial).
constexpr bool guard_step(Guard& g, Try k) {
    const int i = static_cast<int>(k);
    if (!g.begun[i]) return false;
    trial_step(g.trial[i]);
    if (!g.on_disk) return false;
    for (int j = 0; j < 2; ++j)
        if (g.begun[j] && !g.trial[j].done) return false;
    g.on_disk = false;
    return true;
}

// The player chose the menu's last entry (Quit to Desktop) in this frame.
constexpr void guard_leaving(Guard& g) { g.leaving = true; }

// After each UI frame: true when the marker may go (the menu closed kIdleFrames frames ago, or Quit was chosen in a
// frame whose calls the frame before had already drawn, or an orderly world stop during the completed frame).
constexpr bool guard_frame(Guard& g) {
    const bool busy = g.busy, was = g.was_busy, leaving = g.leaving;
    const bool world_stopped = g.world_stopped;
    g.was_busy = busy;
    g.busy = g.leaving = g.world_stopped = false;
    if (!g.on_disk) {
        g.idle = 0;
        return false;
    }
    if (busy) {
        g.idle = 0;
        if (!world_stopped && !(leaving && was)) return false;
        g.on_disk = false;
        return true;
    }
    if (++g.idle < kIdleFrames) return false;
    g.on_disk = false;
    g.idle = 0;
    return true;
}

// A world is being destroyed (KILN_WORLD_STOPPING): true when the marker may go. Not while a frame with Kiln's calls
// is unfinished: remember it until that frame finishes, so a quick orderly quit needs no further frames or exit hook.
constexpr bool guard_world_stops(Guard& g) {
    if (!g.on_disk) return false;
    if (g.busy) {
        g.world_stopped = true;
        return false;
    }
    g.on_disk = false;
    g.idle = 0;
    return true;
}

// The process exits with code 0 (on_unload): true when the marker may go.
constexpr bool guard_exit(Guard& g) {
    if (!g.on_disk) return false;
    g.on_disk = false;
    return true;
}

// ---- the parameter block (keen::GameUiScrollingLayoutParameterData, 0x104 bytes; docs/re/game-ui.md) ----
// After the game's defaults (ScrollParamsInit), what every list caller of the game sets and Kiln sets the same way:
// the scrollbar's look from the UI config (as the credits and the recipe info panel: without it the bar is drawn
// with the default block, a plain white bar), and for a list whose entries take the keyboard and gamepad focus, the
// global focus (useGlobalFocus: the scroll view's end looks the focused frame up in the UI's global focus instead of
// in its own builder's scope, and auto-scrolling brings it into view when it changes) and the scroll keys (mask 0x18).
inline constexpr size_t kParamsSize = 0x104;
inline constexpr size_t kUseGlobalFocus = 0x32, kHotKeyMask = 0x40, kScrollBar = 0x44, kScrollBarSize = 0xc0;
inline constexpr uint8_t kHotKeys = 0x18;
// in the scrollbar block (GameUiScrollbarParameterData), as both callers fill it
inline constexpr size_t kBarFromCfgA = 0x44, kBarFromCfgB = 0x24, kBarFromCfgC = 0x4;  // 32, 32 and 8 bytes of the config
inline constexpr size_t kBarFlagA = 0x7c, kBarFlagB = 0x90, kBarSizes = 0x80;  // flags set to 1; sizes 24, 24, 24, 20

// Fills the scrollbar block `bar` (after the game's ScrollbarParamsInit) from the UI config `cfg`, then puts it and the
// list settings into the parameter block `params` (after ScrollParamsInit). Plain bytes: the host test runs it.
inline void style_params(uint8_t* params, uint8_t* bar, const uint8_t* cfg, size_t cfg_a, size_t cfg_b, size_t cfg_c) {
    std::memcpy(bar + kBarFromCfgA, cfg + cfg_a, 32);
    std::memcpy(bar + kBarFromCfgB, cfg + cfg_b, 32);
    std::memcpy(bar + kBarFromCfgC, cfg + cfg_c, 8);
    bar[kBarFlagA] = 1;
    bar[kBarFlagB] = 1;
    const float sizes[4] = {24.0f, 24.0f, 24.0f, 20.0f};
    std::memcpy(bar + kBarSizes, sizes, sizeof sizes);
    std::memcpy(params + kScrollBar, bar, kScrollBarSize);
    params[kUseGlobalFocus] = 1;
    params[kHotKeyMask] |= kHotKeys;
}

// ---- the view's height ----
// The builder adds three spacers to the column itself (b2a8e0 / b2a810 create children of the column's node, not of the
// current frame): two flexible ones (relative height 1) and a fixed one. With the view at relative height 1 too the
// three flexible children shared the column's free height: a view a third of the column (2026-11-13, 3440x1440: column
// 1020, view 311, entries 1522). Kiln gives the view a much larger relative height, so it takes nearly all of it.
inline constexpr float kViewWeight = 1000.0f;

// A view much shorter than its column while the entries do not fit: the height went wrong (a build where the
// spacers or the sizing differ). Kiln says so and stops.
constexpr bool view_too_short(float column_h, float view_h, float entries_h) {
    return entries_h > view_h + 1.0f && view_h < 0.6f * column_h;
}

// ---- sanity of what the game's layout says (a frame's rectangle, UI units) ----
inline bool sane_rect(const float r[4]) {
    for (int i = 0; i < 4; ++i)
        if (!std::isfinite(r[i]) || std::fabs(r[i]) > 100000.0f) return false;
    return r[2] > 0.0f && r[3] > 0.0f;
}

}  // namespace kui::menu_scroll
