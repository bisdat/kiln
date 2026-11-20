// SPDX-License-Identifier: MIT
// kiln.ui.values: native lines inside the game's own item detail view (capability ui.tooltip.lines), the detail's stat
// numbers scaled (ui.tooltip.values), and the tooltip probe of the guided client session (equipment wave P8b; the
// wave's TOOLTIPS.md and docs/re/game-ui.md "Native tooltip lines"; client checks equip.c5..equip.c9 unplayed).
//
//   mid ui/DetailAfterStats (H2)      between the stats block and the description: the lines placed AfterStats
//   mid ui/DetailContentEnd (H1)      the scrolled content list's end, before the game closes it: every line not yet
//                                     drawn (AfterStats lines too when H2 is missing or was not reached)
//   mid ui/DetailStatsBlockCall       opens the window in which ItemDetail's stats block draws its numbers; H2 closes it
//   raw ui/StatValueDraw (H4)         a number drawn inside that window is multiplied by the occurrence's scale
//   mid ui/DetailStatsModel (H3), mid ui/DetailHeaderModel (H5): the probe's logs only
//
// Each line is built as the game builds the detail's description (TextDraw in ui_values_internal.h: the same style,
// alignment and size mode {1, 0}, so the game's shaper wraps it at the panel's width) and drawn as a child of the
// scrolled content list, so the list's own layout places it after the vanilla lines and the scroll extent grows with
// it. Colours are the game's palette entries (the header's rarity colours). Everything the recipe needs comes from
// sigdb captures of the description, compact-line, small-text and palette code (sigdb/symbols/client.d/
// equip-tooltips.json in sigdb/symbols/client.d); a missing one leaves the row off and the lines go to Kiln's details window only.
//
// Failure: a draw that faults turns the native lines off for the session (one kiln.log line, the row reported off);
// the details window keeps working. All of this runs on the client's main thread inside ItemDetail; the per-occurrence
// state lives in that occurrence's DetailCopy (ui_values_lines.cpp), never across frames.
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>

#include "ui_values_internal.h"

namespace kiln::uivalues {
namespace tt {  // named: the host tests compile every ui_values file into one unit

constexpr const char* kLinesRow = "ui.tooltip.lines";
constexpr const char* kValuesRow = "ui.tooltip.values";
// Q6 of the client session decides these: until then Positive is the game's Uncommon (green) rarity colour and Negative
// the detail text's own colour. The probe also draws the stats block's two buff tints to compare.
constexpr uint32_t kPositiveFrom = kColorUncommon;
constexpr uint32_t kNegativeFrom = kColorDefault;
constexpr float kScaleMax = 1000.0f;

struct TtLayout {
    uintptr_t h1 = 0, h2 = 0, gate = 0, h3 = 0, h4 = 0, h5 = 0;
    uintptr_t line = 0, draw = 0, align = 0, size = 0, ui_app = 0, rect = 0;
    int64_t config = 0, style = 0, small_size = 0;
    int32_t node = 0, shown = 0, align_field = 0, wrap = 0, flags = 0, flags2 = 0, color = 0, bits = 0, size_field = 0;
    int64_t pal[kColors] = {};  // bundle offsets of the 8-byte palette entries (-1: none)
    int64_t content_scope = -1, scroll_scope = -1, header_model = -1, header_color = -1;
    int64_t detail_mode = -1, detail_compact = -1, detail_available = -1, detail_unlocked = -1;
    int64_t model_rows = -1, model_stride = 0, model_available = -1, model_unlocked = -1, model_base = -1;
    ViewLayout view;
};
TtLayout g_l;

std::mutex g_mu;
int g_lines_state = 0;   // 0 not tried, 1 on, -1 off
int g_values_state = 0;  // the same for the values
std::atomic<bool> g_lines_on{false}, g_values_on{false}, g_probe_on{false};
std::atomic<uint64_t> g_drawn{0}, g_scaled{0};
std::string g_lines_why, g_values_why;

bool cst(const char* n, int64_t* out) { return sig_constant(n, out); }

// An offset inside the game's text object from two captures of the same code (field minus object).
bool field(const char* f, const char* base, int32_t width, int32_t* out) {
    int64_t a = 0, b = 0;
    if (!cst(f, &a) || !cst(base, &b)) return false;
    const int64_t d = a - b;
    if (d < 0 || d + width > kTextObjectBytes) return false;
    *out = static_cast<int32_t>(d);
    return true;
}

bool load_lines_layout(std::string* why) {
    TtLayout l;
    if (!load_view_layout(&l.view)) return *why = "the item view constants did not resolve", false;
    if (!sig_symbol("ui/DetailContentEnd", &l.h1) || !sig_symbol("ui/NativeTextLine", &l.line) ||
        !sig_symbol("ui/NativeTextDraw", &l.draw) || !sig_symbol("ui/UiNodeAlign", &l.align) ||
        !sig_symbol("ui/UiNodeSize", &l.size) || !sig_symbol("ui/UiApp", &l.ui_app))
        return *why = "a symbol of the detail view's text recipe did not resolve", false;
    if (!sig_symbol("ui/DetailAfterStats", &l.h2)) l.h2 = 0;  // optional: AfterStats lines then go last
    if (!sig_symbol("ui/UiNodeRect", &l.rect)) l.rect = 0;    // optional: the probe's rectangles
    int64_t small_flags = 0, small_field = 0;
    if (!cst("ui/ui.config", &l.config) || !cst("ui/tt.descStyle", &l.style) || !cst("ui/tt.smallSize", &l.small_size) ||
        !field("ui/tt.descNode", "ui/tt.descText", 8, &l.node) || !field("ui/tt.descShown", "ui/tt.descText", 1, &l.shown) ||
        !field("ui/tt.descAlign", "ui/tt.descText", 2, &l.align_field) ||
        !field("ui/tt.descWrap", "ui/tt.descText", 1, &l.wrap) ||
        !field("ui/tt.colorFlags", "ui/tt.colorText", 2, &l.flags) ||
        !field("ui/tt.colorFlags2", "ui/tt.colorText", 2, &l.flags2) ||
        !field("ui/tt.colorValue", "ui/tt.colorText", 8, &l.color) ||
        !field("ui/tt.colorBits", "ui/tt.colorText", 1, &l.bits) || !cst("ui/tt.smallFlags", &small_flags) ||
        !cst("ui/tt.smallField", &small_field))
        return *why = "a field of the detail view's text recipe did not resolve or lies outside the text object", false;
    // the small text's size field, from the small-text code's own pair (flags, size) applied to the colour's flags
    const int64_t sf = l.flags + (small_field - small_flags);
    if (sf < 0 || sf + 4 > kTextObjectBytes) return *why = "the small text's size field lies outside the text object", false;
    l.size_field = static_cast<int32_t>(sf);
    if (l.config < 0 || l.style < 0 || l.small_size < 0) return *why = "a UI bundle offset is negative", false;
    int64_t base = 0;
    if (!cst("ui/tt.palette", &base) || base < 0) return *why = "the rarity palette did not resolve", false;
    struct Entry {
        uint32_t color;
        const char* name;
    };
    for (auto& p : l.pal) p = -1;
    for (const Entry& e : {Entry{kColorCommon, "ui/tt.palCommon"}, Entry{kColorUncommon, "ui/tt.palUncommon"},
                           Entry{kColorRare, "ui/tt.palRare"}, Entry{kColorEpic, "ui/tt.palEpic"},
                           Entry{kColorLegendary, "ui/tt.palLegendary"}, Entry{kColorUnique, "ui/tt.palUnique"}}) {
        int64_t o = 0;
        if (!cst(e.name, &o) || o < 0 || o > 0x100) return *why = "a rarity palette entry did not resolve", false;
        l.pal[e.color] = base + o;
    }
    l.pal[kColorMuted] = base;  // the header's colour for an item without a rarity (the palette's first entry)
    l.pal[kColorPositive] = kPositiveFrom == kColorDefault ? -1 : l.pal[kPositiveFrom];
    l.pal[kColorNegative] = kNegativeFrom == kColorDefault ? -1 : l.pal[kNegativeFrom];
    int64_t o = 0;
    l.pal[kColorBuffPositive] = cst("ui/tt.buffPositive", &o) && o >= 0 ? o : -1;
    l.pal[kColorBuffNegative] = cst("ui/tt.buffNegative", &o) && o >= 0 ? o : -1;
    // the probe's reads (optional)
    if (!cst("ui/detail.contentScope", &l.content_scope)) l.content_scope = -1;
    if (!cst("ui/detail.scrollScope", &l.scroll_scope)) l.scroll_scope = -1;
    g_l.view = l.view;
    const TtLayout keep = g_l;
    g_l = l;
    // keep what the values loader already set
    g_l.gate = keep.gate, g_l.h4 = keep.h4;
    if (!g_l.h2) g_l.h2 = keep.h2;
    return true;
}

void load_probe_layout() {
    if (!g_l.view.item) load_view_layout(&g_l.view);  // the DETAIL line's item fields, also with both features off
    if (!sig_symbol("ui/DetailStatsModel", &g_l.h3)) g_l.h3 = 0;
    if (!sig_symbol("ui/DetailHeaderModel", &g_l.h5)) g_l.h5 = 0;
    auto opt = [](const char* n, int64_t* out) {
        if (!cst(n, out)) *out = -1;
    };
    opt("ui/detail.headerModel", &g_l.header_model);
    opt("ui/headerModel.color", &g_l.header_color);
    opt("ui/detail.mode", &g_l.detail_mode);
    opt("ui/detail.compact", &g_l.detail_compact);
    opt("ui/detail.available", &g_l.detail_available);
    opt("ui/detail.unlocked", &g_l.detail_unlocked);
    opt("ui/statsModel.rows", &g_l.model_rows);
    opt("ui/statsModel.available", &g_l.model_available);
    opt("ui/statsModel.unlocked", &g_l.model_unlocked);
    opt("ui/statsModel.baseLevel", &g_l.model_base);
    if (!cst("ui/statsModel.rowStride", &g_l.model_stride)) g_l.model_stride = 0;
}

void lines_off(const char* why) {
    {
        std::lock_guard lk(g_mu);
        if (g_lines_state < 0) return;
        g_lines_state = -1;
        g_lines_why = why;
    }
    g_lines_on.store(false);
    logf(KILN_LOG_ERROR, "ui.tooltip: native tooltip lines are off for this session: %s. Their lines show in Kiln's "
                         "Item details window only", why);
    if (env().row_off) env().row_off(kLinesRow, why);
}

void values_off(const char* why) {
    {
        std::lock_guard lk(g_mu);
        if (g_values_state < 0) return;
        g_values_state = -1;
        g_values_why = why;
    }
    g_values_on.store(false);
    logf(KILN_LOG_ERROR, "ui.tooltip: tooltip values are off for this session: %s. The game's own numbers show", why);
    if (env().row_off) env().row_off(kValuesRow, why);
}

// ---- the probe (declared here, defined below) ----
void probe_fill(DetailCopy& c);
void probe_site(const char* site, const KilnRegisters* r);
void probe_after_draw(const DetailCopy& c, const KilnRegisters* r);
float probe_scale();
void probe_value(float before, float after);

// ---- drawing ----
uintptr_t bundle() {
    uintptr_t app = 0, cfg = 0;
    if (!game_read(g_l.ui_app, &app) || !app || !game_read(app + static_cast<uintptr_t>(g_l.config), &cfg)) return 0;
    return cfg;
}

// The native lines of an occurrence, gathered once: the probe's (provider lines join here in the API slice).
void prepare(DetailCopy& c) {
    if (c.native_ready) return;
    c.native_ready = true;
    collect_once(c);
    if (g_probe_on.load(std::memory_order_relaxed)) probe_fill(c);
}

void draw(DetailCopy& c, const KilnRegisters* r, bool at_after_stats) {
    if (!g_lines_on.load(std::memory_order_acquire) || !c.native_count) return;
    const uintptr_t cfg = bundle();
    if (!cfg) {
        lines_off("the UI bundle could not be read");
        return;
    }
    for (uint32_t i = 0; i < c.native_count; ++i) {
        const NativeLine& l = c.native[i];
        if (c.drawn[i] || (at_after_stats && !(l.flags & kLineAfterStats))) continue;
        TextDraw d;
        d.line_fn = g_l.line, d.draw_fn = g_l.draw, d.align_fn = g_l.align, d.size_fn = g_l.size;
        d.style = cfg + static_cast<uintptr_t>(g_l.style);
        d.text = l.text.data(), d.text_len = l.text.size(), d.text_fnv = text::fnv64(l.text.data(), l.text.size());
        d.align = r->r15 ? 0x101 : 0x102;  // as the description: one way with the item's info, the other without
        d.node = g_l.node, d.shown = g_l.shown, d.align_field = g_l.align_field, d.wrap = g_l.wrap;
        d.flags = g_l.flags, d.flags2 = g_l.flags2, d.color = g_l.color, d.bits = g_l.bits, d.size_field = g_l.size_field;
        const uint32_t color = l.flags & kLineColorMask;
        const int64_t entry = color < kColors ? g_l.pal[color] : -1;
        if (entry >= 0 && game_read(cfg + static_cast<uintptr_t>(entry), &d.color_value)) d.colored = true;
        if ((l.flags & kLineSmall) && game_read(cfg + static_cast<uintptr_t>(g_l.small_size), &d.small_size) &&
            std::isfinite(d.small_size) && d.small_size > 0)
            d.small = true;
        uintptr_t node = 0;
        if (!env().draw_text || !env().draw_text(&d, &node)) {
            lines_off("drawing a line into the game's item detail view faulted");
            return;
        }
        c.drawn[i] = true;
        c.nodes[i] = node;
        g_drawn.fetch_add(1, std::memory_order_relaxed);
    }
}

void KILN_CALL on_after_stats(KilnRegisters* r, void*) {
    DetailCopy* c = current_detail();
    if (!c || !c->ok) return;
    if (g_probe_on.load(std::memory_order_relaxed)) probe_site("H2", r);
    prepare(*c);
    draw(*c, r, true);
}

void KILN_CALL on_content_end(KilnRegisters* r, void*) {
    DetailCopy* c = current_detail();
    if (!c || !c->ok) return;
    if (g_probe_on.load(std::memory_order_relaxed)) probe_site("H1", r);
    prepare(*c);
    draw(*c, r, false);
    if (g_probe_on.load(std::memory_order_relaxed)) probe_after_draw(*c, r);
}

// ---- values ----
thread_local DetailCopy* t_gate = nullptr;  // the occurrence whose stats block is drawing now
using StatFn = void (*)(const void*, float);
StatFn g_stat_next = nullptr;

// The value providers over one occurrence, once: the product of their scales (each sees the product before it).
float run_values(const DetailCopy& c, float s) {
    if (!any(kListValues) || !env().call_scale) return s;
    KilnUiItemView v{};
    v.size = sizeof v;
    fill_view(c.raw, g_l.view, KUI_DETAIL_COPY, &v);
    if (v.item == 0) return s;
    ChainStamp st;
    ClientNow now{};
    if (env().client && env().client(&now)) {
        v.local_player = now.local_player;
        st.session = now.session;
        st.step = now.step;
    }
    KilnUiScreen screen{};
    if (env().screen && env().screen(&screen)) {
        v.screen = screen;
        st.frame = screen.frame + 1;  // the frame being built
    }
    v.session = st.session;
    v.frame = st.frame;
    const auto list = providers(kListValues);
    for (const auto& sp : *list) {
        Provider& p = *sp;
        if (!provider_live(p) || (p.item != 0 && p.item != v.item)) continue;
        CallState cs;
        cs.magic = kCallMagic;
        cs.provider = &p;
        KilnUiProviderContext ctx{};
        ctx.size = sizeof ctx;
        ctx.kind = kListValues;
        ctx.session = st.session;
        ctx.frame = st.frame;
        ctx.client_step = st.step;
        ctx.owner = p.owner;
        ctx.text = text_ops();
        ctx.reserved = &cs;
        KilnUiItemView view = v;
        float out = s;
        KilnResult rc = KILN_ERR_INTERNAL;
        bool called;
        {
            CallScope scope(&cs);
            called = env().call_scale(reinterpret_cast<KilnUiScaleFn>(p.fn), &ctx, &view, &out, p.user, &rc);
        }
        if (!env().owner_alive(p.owner)) continue;
        if (!called || rc == KILN_ERR_INTERNAL) {
            fault(p, called ? "returned an internal error" : "faulted");
            continue;
        }
        if (rc != KILN_OK) {
            refused(p, KUI_REJECTED);  // the scale before it stays
            continue;
        }
        if (!std::isfinite(out) || out < 0 || out > kScaleMax) {
            fault(p, "returned a scale outside 0..1000");
            continue;
        }
        s = out;
        succeeded(p);
    }
    return s;
}

float occurrence_scale(DetailCopy& c) {
    if (c.scaled) return c.scale;
    c.scaled = true;
    float s = run_values(c, 1.0f);
    if (g_probe_on.load(std::memory_order_relaxed)) s *= probe_scale();
    c.scale = std::isfinite(s) && s >= 0 && s <= kScaleMax ? s : 1.0f;
    return c.scale;
}

void KILN_CALL on_stats_open(KilnRegisters* r, void*) {
    DetailCopy* c = current_detail();
    if (g_probe_on.load(std::memory_order_relaxed)) probe_site("gate", r);
    t_gate = c && c->ok && g_values_on.load(std::memory_order_acquire) ? c : nullptr;
    if (t_gate) occurrence_scale(*t_gate);
}

void KILN_CALL on_stats_close(KilnRegisters*, void*) { t_gate = nullptr; }

void hk_stat_value(const void* label, float value) {
    DetailCopy* c = t_gate;
    if (c && c == current_detail() && c->scale != 1.0f && std::isfinite(value)) {
        const float after = value * c->scale;
        if (g_probe_on.load(std::memory_order_relaxed)) probe_value(value, after);
        g_scaled.fetch_add(1, std::memory_order_relaxed);
        value = after;
    }
    g_stat_next(label, value);
}

// ---- installs ----
using Allowed = bool (*)(const char*);

bool install_lines_natives(Allowed allowed) {
    std::lock_guard lk(g_mu);
    if (g_lines_state) return g_lines_state > 0;
    std::string why;
    if (!env().layout_supported || !env().layout_supported()) why = "private UI layout is not measured for this client image";
    else if (!allowed(kLinesRow)) why = "the capability is off or held";
    else if (!load_lines_layout(&why)) {
    } else if (!detail_hook_install()) why = "the item detail view could not be hooked";
    else {
        KilnResult rc = env().hook_mid(g_l.h1, "ui.tooltip/DetailContentEnd", &on_content_end);
        if (rc == KILN_OK && g_l.h2) rc = env().hook_mid(g_l.h2, "ui.tooltip/DetailAfterStats", &on_after_stats);
        if (rc != KILN_OK) why = "hooking the detail view's content end failed";
    }
    if (!why.empty()) {
        g_lines_state = -1;
        g_lines_why = why;
        logf(KILN_LOG_WARN, "ui.tooltip: no native tooltip lines: %s; lines show in Kiln's Item details window", why.c_str());
        return false;
    }
    g_lines_state = 1;
    g_lines_on.store(true, std::memory_order_release);
    logf(KILN_LOG_INFO, "ui.tooltip: native tooltip lines on (inside the item detail's scrolled content%s)",
         g_l.h2 ? ", AfterStats between the stats and the description" : "; no AfterStats place: such lines go last");
    return true;
}

bool install_values_natives(Allowed allowed) {
    std::lock_guard lk(g_mu);
    if (g_values_state) return g_values_state > 0;
    std::string why;
    uintptr_t gate = 0, h2 = 0, h4 = 0;
    if (!env().layout_supported || !env().layout_supported()) why = "private UI layout is not measured for this client image";
    else if (!allowed(kValuesRow)) why = "the capability is off or held";
    else if (!load_view_layout(&g_l.view)) why = "the item view constants did not resolve";
    else if (!sig_symbol("ui/DetailStatsBlockCall", &gate) || !sig_symbol("ui/DetailAfterStats", &h2) ||
             !sig_symbol("ui/StatValueDraw", &h4))
        why = "a symbol of the detail view's stats block did not resolve";
    else if (!detail_hook_install()) why = "the item detail view could not be hooked";
    else {
        KilnResult rc = env().hook_mid(gate, "ui.tooltip/DetailStatsBlockCall", &on_stats_open);
        if (rc == KILN_OK) rc = env().hook_mid(h2, "ui.tooltip/DetailStatsBlockEnd", &on_stats_close);
        if (rc == KILN_OK)
            rc = env().hook_raw(h4, "ui.tooltip/StatValueDraw", 0, reinterpret_cast<void*>(&hk_stat_value),
                                reinterpret_cast<void**>(&g_stat_next));
        if (rc != KILN_OK || !g_stat_next) why = "hooking the stats block's number draw failed";
    }
    if (!why.empty()) {
        g_values_state = -1;
        g_values_why = why;
        logf(KILN_LOG_WARN, "ui.tooltip: no tooltip values: %s; the game's own numbers show", why.c_str());
        return false;
    }
    g_l.gate = gate, g_l.h4 = h4;
    if (!g_l.h2) g_l.h2 = h2;
    g_values_state = 1;
    g_values_on.store(true, std::memory_order_release);
    logf(KILN_LOG_INFO, "ui.tooltip: tooltip values on (the item detail's stats block numbers)");
    return true;
}

bool feature_allowed(const char* name) { return env().feature_open && env().feature_open(name); }
bool row_allowed(const char* name) { return env().row_available && env().row_available(name); }

// kiln.ui.values' installer of the values list: the first tooltip_values provider installs the value sites.
bool install_values_list(uint32_t list) { return list == kListValues && install_values_natives(&row_allowed); }
struct ValuesInstaller {
    ValuesInstaller() { set_installer(kListValues, &install_values_list); }
} g_values_installer;

// ============================================================================================ the tooltip probe
// The guided client session's build (TOOLTIPS.md §9): a fixed test block drawn through the native path above, in
// phases the player steps through with one key, and kiln.log lines that answer the session's questions. Switched on
// only by KILN_TOOLTIP_PROBE or kiln.json "tooltipProbe" (ui_values_service.cpp); never in a normal start.
enum Phase : uint32_t { kPhOff, kPhShort, kPhSixteen, kPhLongEnglish, kPhLongGerman, kPhColors, kPhAfterStats, kPhValues, kPhases };
const char* const kPhaseNames[kPhases] = {"off", "short", "16-lines", "long-english", "long-german", "colours",
                                          "after-stats", "values-x2"};

struct Probe {
    uint32_t vk = 0;
    bool was_down = false;
    uint32_t phase = kPhOff;
    uint64_t changes = 0;  // phase changes (a RECT line is written once per phase and item)
    // what was logged last (each line only when its content changes)
    std::string detail, site[5], drawn, model, header, rect_key;
    uint32_t stable = 0;  // H1 occurrences since the item or the phase changed
    uint32_t values_logged = 0;
    std::string values_key;
    float values_seen[8] = {};
};
Probe g_probe;

const char* const kSixteenLines[16] = {
    "Kiln probe line 1 of 16",  "Kiln probe line 2 of 16",  "Kiln probe line 3 of 16",  "Kiln probe line 4 of 16",
    "Kiln probe line 5 of 16",  "Kiln probe line 6 of 16",  "Kiln probe line 7 of 16",  "Kiln probe line 8 of 16",
    "Kiln probe line 9 of 16",  "Kiln probe line 10 of 16", "Kiln probe line 11 of 16", "Kiln probe line 12 of 16",
    "Kiln probe line 13 of 16", "Kiln probe line 14 of 16", "Kiln probe line 15 of 16", "Kiln probe line 16 of 16 (the last)"};
constexpr const char* kLongEnglish =
    "Kiln probe, long English line: this sentence is deliberately much longer than the item detail panel is wide, so the "
    "game has to wrap it onto several lines at the panel's border; no word may run past the edge, and the scrollbar has "
    "to reach its very last word.";
constexpr const char* kLongGerman =
    "Kiln-Probe, lange deutsche Zeile: Dieser Satz ist absichtlich viel länger als das Detailfenster breit ist, damit "
    "das Spiel ihn am Rand umbricht; Umlaute wie ä, ö, ü und das ß müssen lesbar bleiben, kein Wort darf über den Rand "
    "hinauslaufen, und der Bildlauf muss bis zum letzten Wort reichen.";

void push_line(DetailCopy& c, const char* s, uint32_t flags) {
    if (c.native_count >= kNativeLines) return;
    c.native[c.native_count++] = NativeLine{std::string_view(s), flags};
}

void probe_fill(DetailCopy& c) {
    switch (g_probe.phase) {
        case kPhShort: push_line(c, "Kiln probe: one short line", 0); break;
        case kPhSixteen:
            for (const char* s : kSixteenLines) push_line(c, s, 0);
            break;
        case kPhLongEnglish: push_line(c, kLongEnglish, 0); break;
        case kPhLongGerman: push_line(c, kLongGerman, 0); break;
        case kPhColors:
            push_line(c, "Default colour (the description's)", kColorDefault);
            push_line(c, "Positive: +12 % fire damage (the Uncommon colour)", kColorPositive);
            push_line(c, "Negative: -5 % attack speed (the default colour until Q6)", kColorNegative);
            push_line(c, "Common", kColorCommon);
            push_line(c, "Uncommon", kColorUncommon);
            push_line(c, "Rare", kColorRare);
            push_line(c, "Epic", kColorEpic);
            push_line(c, "Legendary", kColorLegendary);
            push_line(c, "Unique", kColorUnique);
            push_line(c, "Muted (the no-rarity colour)", kColorMuted);
            push_line(c, "Small text in the default colour", kColorDefault | kLineSmall);
            push_line(c, "Buff positive tint (raw 8 bytes)", kColorBuffPositive);
            push_line(c, "Buff negative tint (raw 8 bytes)", kColorBuffNegative);
            break;
        case kPhAfterStats:
            push_line(c, "Kiln probe: between the stats and the description (AfterStats)", kLineAfterStats);
            push_line(c, "Kiln probe: after the last vanilla line (End)", 0);
            break;
        case kPhValues: push_line(c, "Kiln probe: the stat numbers above show doubled", kColorPositive); break;
        default: break;
    }
}

float probe_scale() { return g_probe.phase == kPhValues ? 2.0f : 1.0f; }

uint32_t tid() { return env().thread_id ? env().thread_id() : 0; }

template <class T>
T rd(uintptr_t at, T fallback = T{}) {
    T v{};
    return at && game_read(at, &v) ? v : fallback;
}

void key_tick() {
    if (!g_probe.vk || !env().key_down) return;
    const bool down = env().key_down(g_probe.vk);
    if (down && !g_probe.was_down) {
        g_probe.phase = (g_probe.phase + 1) % kPhases;
        ++g_probe.changes;
        g_probe.stable = 0;
        g_probe.values_logged = 0;
        logf(KILN_LOG_INFO, "tooltip-probe: PHASE %s (%u of %u)", kPhaseNames[g_probe.phase], g_probe.phase, kPhases - 1);
    }
    g_probe.was_down = down;
}

std::string detail_key(const DetailCopy& c, bool* has) {
    KilnUiItemView v{};
    v.size = sizeof v;
    fill_view(c.raw, g_l.view, KUI_DETAIL_COPY, &v);
    const uintptr_t rec = c.record;
    const uint32_t compact = g_l.detail_compact >= 0 ? rd<uint32_t>(rec + g_l.detail_compact) : 0xffffffffu;
    uintptr_t mode_ptr = g_l.detail_mode >= 0 ? rd<uintptr_t>(rec + g_l.detail_mode) : 0;
    char mode[16] = "none";
    if (mode_ptr) std::snprintf(mode, sizeof mode, "%u", rd<uint32_t>(mode_ptr, 0xffffffffu));
    const uint32_t avail = g_l.detail_available >= 0 ? rd<uint8_t>(rec + g_l.detail_available) : 0;
    const uint32_t unl = g_l.detail_unlocked >= 0 ? rd<uint8_t>(rec + g_l.detail_unlocked) : 0;
    char s[200];
    std::snprintf(s, sizeof s, "mode=%s compact=%u item=%u pide=%u lvl=%u rar=%u perks=%u/%u", mode, compact, v.item,
                  v.client_entity, v.level, v.rarity, avail, unl);
    *has = v.item != 0;
    return s;
}

void probe_detail(DetailCopy& c) {
    key_tick();
    bool has = false;
    const std::string k = detail_key(c, &has);
    if (k != g_probe.detail) {
        g_probe.detail = k;
        g_probe.stable = 0;
        g_probe.values_logged = 0;
        logf(KILN_LOG_INFO, "tooltip-probe: DETAIL %s phase=%s", k.c_str(), kPhaseNames[g_probe.phase]);
    }
}

int site_index(const char* s) {
    return !std::strcmp(s, "H1") ? 0 : !std::strcmp(s, "H2") ? 1 : !std::strcmp(s, "H3") ? 2 : !std::strcmp(s, "H5") ? 3 : 4;
}

void probe_site(const char* site, const KilnRegisters* r) {
    const std::string key = g_probe.detail + " " + kPhaseNames[g_probe.phase];
    std::string& last = g_probe.site[site_index(site)];
    if (last == key) return;
    last = key;
    logf(KILN_LOG_INFO, "tooltip-probe: SITE %s tid=%u r15=%s %s", site, tid(), r && r->r15 ? "info" : "0",
         g_probe.detail.c_str());
}

void KILN_CALL on_probe_model(KilnRegisters* r, void*) {
    DetailCopy* c = current_detail();
    if (!c || !c->ok) return;
    probe_site("H3", r);
    if (g_probe.model == g_probe.detail || g_l.model_rows < 0 || g_l.model_stride <= 0) return;
    g_probe.model = g_probe.detail;
    const uintptr_t m = r->rax;
    char s[700];
    int n = std::snprintf(s, sizeof s, "tooltip-probe: MODEL available=%u unlocked=%u base=%u",
                          g_l.model_available >= 0 ? rd<uint8_t>(m + g_l.model_available) : 0,
                          g_l.model_unlocked >= 0 ? rd<uint8_t>(m + g_l.model_unlocked) : 0,
                          g_l.model_base >= 0 ? rd<uint32_t>(m + g_l.model_base) : 0);
    for (int i = 0; i < 5 && n > 0 && n < static_cast<int>(sizeof s) - 120; ++i) {
        const uintptr_t row = m + static_cast<uintptr_t>(g_l.model_rows + i * g_l.model_stride);
        uint32_t w[6] = {};
        for (int k = 0; k < 6; ++k) w[k] = rd<uint32_t>(row + 0x48 + 4 * k);
        float f = 0;
        std::memcpy(&f, &w[0], 4);
        n += std::snprintf(s + n, sizeof s - n, " | row%d lvl=%u +48=%08x(%g) %08x %08x %08x %08x %08x", i,
                           rd<uint32_t>(row + 8), w[0], f, w[1], w[2], w[3], w[4], w[5]);
    }
    logf(KILN_LOG_INFO, "%s", s);
}

void KILN_CALL on_probe_header(KilnRegisters* r, void*) {
    DetailCopy* c = current_detail();
    if (!c || !c->ok) return;
    probe_site("H5", r);
    if (g_probe.header == g_probe.detail || g_l.header_model < 0 || g_l.header_color < 0) return;
    g_probe.header = g_probe.detail;
    const uint64_t color = rd<uint64_t>(r->rbp + static_cast<uintptr_t>(g_l.header_model + g_l.header_color));
    logf(KILN_LOG_INFO, "tooltip-probe: HEADER colour=%016llx %s", static_cast<unsigned long long>(color),
         g_probe.detail.c_str());
}

void rect_text(char* out, size_t cap, uintptr_t node) {
    float q[4] = {NAN, NAN, NAN, NAN};
    if (!node || !g_l.rect || !env().node_rect || !env().node_rect(g_l.rect, node, q)) {
        std::snprintf(out, cap, "none");
        return;
    }
    std::snprintf(out, cap, "%.1f,%.1f %.1fx%.1f", q[0], q[1], q[2], q[3]);
}

uintptr_t scope_node(uintptr_t rbp, int64_t scope) {
    return scope < 0 ? 0 : rd<uintptr_t>(rbp + static_cast<uintptr_t>(scope) + 8);  // the UiNode at scope object +8
}

void probe_after_draw(const DetailCopy& c, const KilnRegisters* r) {
    uint32_t n = 0;
    for (uint32_t i = 0; i < c.native_count; ++i) n += c.drawn[i];
    char d[96];
    std::snprintf(d, sizeof d, "n=%u of %u phase=%s", n, c.native_count, kPhaseNames[g_probe.phase]);
    if (g_probe.drawn != d) {
        g_probe.drawn = d;
        logf(KILN_LOG_INFO, "tooltip-probe: DRAW %s lines_on=%d", d, g_lines_on.load() ? 1 : 0);
    }
    // the layout settles over a few frames: one RECT line per item and phase, 30 occurrences after it changed
    if (++g_probe.stable != 30) return;
    char a[64], b[64];
    rect_text(a, sizeof a, scope_node(r->rbp, g_l.content_scope));
    rect_text(b, sizeof b, scope_node(r->rbp, g_l.scroll_scope));
    std::string s = std::string("tooltip-probe: RECT list=") + a + " scroll=" + b;
    for (uint32_t i = 0; i < c.native_count; ++i) {
        char l[80];
        rect_text(l, sizeof l, c.nodes[i]);
        s += " line[" + std::to_string(i) + "]=" + l;
    }
    logf(KILN_LOG_INFO, "%s", s.c_str());
}

void probe_value(float before, float after) {
    if (g_probe.values_key != g_probe.detail) {
        g_probe.values_key = g_probe.detail;
        g_probe.values_logged = 0;
    }
    for (uint32_t i = 0; i < g_probe.values_logged; ++i)
        if (g_probe.values_seen[i] == before) return;  // each distinct number once per item
    if (g_probe.values_logged >= 8) return;
    g_probe.values_seen[g_probe.values_logged++] = before;
    logf(KILN_LOG_INFO, "tooltip-probe: VALUE %g -> %g (%s)", before, after, g_probe.detail.c_str());
}

}  // namespace tt

using tt::g_lines_on;
using tt::g_probe_on;
using tt::g_values_on;

bool tooltip_lines_on() { return g_lines_on.load(std::memory_order_acquire); }
bool tooltip_values_on() { return g_values_on.load(std::memory_order_acquire); }
bool tooltip_probe_on() { return g_probe_on.load(std::memory_order_acquire); }

bool tooltip_lines_request() { return tt::install_lines_natives(&tt::row_allowed); }

KilnResult tooltip_native_state(KilnUiTooltipNativeState* out) {
    const uint32_t size = out->size;
    *out = KilnUiTooltipNativeState{};
    out->size = size;
    out->lines_on = g_lines_on.load() ? 1 : 0;
    out->values_on = g_values_on.load() ? 1 : 0;
    out->probe = g_probe_on.load() ? 1 : 0;
    out->drawn_lines = tt::g_drawn.load();
    out->scaled_values = tt::g_scaled.load();
    std::lock_guard lk(tt::g_mu);
    out->after_stats = out->lines_on && tt::g_l.h2 ? 1 : 0;
    const std::string& why = !out->lines_on && !tt::g_lines_why.empty() ? tt::g_lines_why : tt::g_values_why;
    if (!out->lines_on || !out->values_on) std::snprintf(out->why, sizeof out->why, "%s", why.c_str());
    return KILN_OK;
}

void tooltip_detail_enter(DetailCopy& c) {
    if (g_probe_on.load(std::memory_order_relaxed)) tt::probe_detail(c);
}

uint32_t probe_key_of(const char* spec) {
    if (!spec || !*spec) return 0x72;  // F3
    std::string s(spec);
    for (char& ch : s)
        if (ch >= 'a' && ch <= 'z') ch = static_cast<char>(ch - 'a' + 'A');
    if (s.size() == 1 && s[0] >= 'A' && s[0] <= 'Z') return static_cast<uint32_t>(s[0]);
    if (s.size() >= 2 && s.size() <= 3 && s[0] == 'F') {
        int n = 0;
        for (size_t i = 1; i < s.size(); ++i) {
            if (s[i] < '0' || s[i] > '9') return 0;
            n = n * 10 + (s[i] - '0');
        }
        if (n >= 1 && n <= 24) return 0x70 + static_cast<uint32_t>(n - 1);  // VK_F1..VK_F24
    }
    return 0;
}

bool probe_start(KilnOwner, const char* spec) {
    if (!env().client_side || !env().client_side()) return false;
    const uint32_t vk = probe_key_of(spec);
    if (!vk) {
        logf(KILN_LOG_WARN, "tooltip-probe: the key '%s' is not F1..F24 or a letter; the probe stays off", spec ? spec : "");
        return false;
    }
    using namespace tt;
    g_probe = Probe{};
    g_probe.vk = vk;
    g_probe_on.store(true, std::memory_order_release);
    const bool lines = install_lines_natives(&feature_allowed);
    const bool values = install_values_natives(&feature_allowed);
    load_probe_layout();
    if (!detail_hook_install()) {
        g_probe_on.store(false);
        logf(KILN_LOG_WARN, "tooltip-probe: the item detail view could not be hooked; the probe stays off");
        return false;
    }
    if (g_l.h3) env().hook_mid(g_l.h3, "ui.tooltip/probe.DetailStatsModel", &on_probe_model);
    if (g_l.h5) env().hook_mid(g_l.h5, "ui.tooltip/probe.DetailHeaderModel", &on_probe_header);
    logf(KILN_LOG_INFO, "tooltip-probe: on (key %s steps the phases: %s); native lines %s, values %s, AfterStats %s, "
                        "rectangles %s",
         spec && *spec ? spec : "F3",
         "off, short, 16-lines, long-english, long-german, colours, after-stats, values-x2",
         lines ? "on" : ("off: " + g_lines_why).c_str(), values ? "on" : ("off: " + g_values_why).c_str(),
         g_l.h2 ? "on" : "off", g_l.rect ? "on" : "off");
    return true;
}

void tooltip_reset_for_tests() {
    using namespace tt;
    std::lock_guard lk(g_mu);
    g_l = TtLayout{};
    g_lines_state = g_values_state = 0;
    g_lines_on.store(false), g_values_on.store(false), g_probe_on.store(false);
    g_drawn.store(0), g_scaled.store(0);
    g_lines_why.clear(), g_values_why.clear();
    g_probe = Probe{};
    g_stat_next = nullptr;
}

}  // namespace kiln::uivalues
