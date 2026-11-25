// SPDX-License-Identifier: MIT
// kiln.ui: the game's popup panels over its UI (feature ui.popups, GitHub issue 117; docs/re/game-ui.md "Popup panels
// over the inventory", sigdb/symbols/client.d/ui-popups.json). Static analysis of client 26430625; nothing here has
// been seen running yet.
//
// The game builds each popup panel (the item menu an RMB on an inventory slot opens: Equip, Salvage, Delete; drop-downs;
// the map's and the settings' context menus) through one builder, PopupWindowBegin(frame, closed*, params*, name*),
// which opens a window at the params' layer and the panel inside it as a child frame at frame + popup.subframe; that
// child's layout node is at frame + popup.panel (= subframe + 8). A raw link calls the builder and then reads the node's
// laid-out rectangle (node + node.rect, the offset UiNodeRect returns): the previous UI frame's layout, in UI units,
// the space of the slots' rectangles Ui.Inventory marks. Nothing is written into the game and no game function is called.
// After each UI frame the panels seen in it (and the game's error popup byte, feature ui.error_popup) are published;
// kiln.ui's popups() hands them to Kiln.Ui, which leaves out the marks under them.
//
// Fail safe: without the feature (a pattern or constant that does not resolve, a refused hook) nothing is published and
// marks are drawn as before. A panel whose rectangle is not readable or not plausible (its first frame: the node has no
// layout yet), and more than KILN_UI_POPUPS_MAX panels, set KILN_UI_POPUP_UNKNOWN: Kiln.Ui then hides every mark while
// it lasts. Threads: the builder and the after-frame on the game's UI thread (a builder call outside a UI frame is not
// counted); popups() on any (a mutex around a copy). popups_install runs while the UI frame link is already in (game_install
// hooks GameUiFrame first), so the layout is filled in before g_on says so (release / acquire): a UI frame never reads a
// half-written layout.
#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <mutex>

#include "kiln_hook.hpp"
#include "ui.h"

namespace kui {
namespace {

struct PopupLayout {
    uintptr_t begin = 0;
    int64_t subframe = 0, panel = 0, rect = 0;  // frame + subframe (the panel's frame), frame + panel -> node, node + rect
    int64_t popups = 0, error = 0;              // sys10 + popups + error: u8, the error popup (feature ui.error_popup)
    bool error_on = false;
} P;  // written by popups_install only while g_on is false; read on the UI thread after g_on's acquire
std::atomic<bool> g_on{false};

kiln::RawHook g_begin_hook;

// the UI frame being built (UI thread only)
struct Collect {
    bool active = false;
    uint32_t flags = 0, count = 0;
    KilnUiPopupRect rect[KILN_UI_POPUPS_MAX] = {};
};
thread_local Collect t_collect;

std::mutex g_mu;
KilnUiPopups g_state{};  // the last completed UI frame's

template <typename T>
bool rd(uintptr_t a, T& out) {
    if (a < 0x10000) return false;
    __try {
        out = *reinterpret_cast<const T*>(a);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

}  // namespace

bool popup_rect_plausible(const KilnUiPopupRect& r) {
    auto in = [](float v, float lo, float hi) { return std::isfinite(v) && v >= lo && v <= hi; };
    return in(r.x, -100000.0f, 100000.0f) && in(r.y, -100000.0f, 100000.0f) && in(r.width, 0.0f, 100000.0f) &&
           in(r.height, 0.0f, 100000.0f) && r.width > 0.0f && r.height > 0.0f;
}

namespace {

void note_panel(uintptr_t frame) {
    Collect& c = t_collect;
    c.flags |= KILN_UI_POPUP_PANEL;
    if (c.count >= KILN_UI_POPUPS_MAX) {
        c.flags |= KILN_UI_POPUP_UNKNOWN;
        return;
    }
    uintptr_t node = 0;
    KilnUiPopupRect r{};
    if (!frame || !rd(frame + static_cast<uintptr_t>(P.panel), node) || !rd(node + static_cast<uintptr_t>(P.rect), r) ||
        !popup_rect_plausible(r)) {
        c.flags |= KILN_UI_POPUP_UNKNOWN;
        return;
    }
    c.rect[c.count++] = r;
}

uintptr_t __fastcall hk_begin(uintptr_t frame, void* closed, void* params, void* name) {
    const uintptr_t r = g_begin_hook.call<uintptr_t>(frame, closed, params, name);
    if (t_collect.active) note_panel(r ? r : frame);  // the builder returns its frame
    return r;
}

bool sym(const char* n, uintptr_t* out) { return g_api->symbol(KilnStr{n, std::strlen(n)}, out) == KILN_OK && *out; }
bool cst(const char* n, int64_t* out) { return g_api->constant(KilnStr{n, std::strlen(n)}, out) == KILN_OK; }
bool feat(const char* n) { return g_api->feature(KilnStr{n, std::strlen(n)}); }

}  // namespace

bool popups_install() {
    g_on.store(false, std::memory_order_release);
    {
        std::lock_guard lk(g_mu);
        g_state = KilnUiPopups{};
    }
    PopupLayout l{};
    bool on = feat("ui.popups") && sym("ui/PopupWindowBegin", &l.begin) && cst("ui/popup.subframe", &l.subframe) &&
              cst("ui/popup.panel", &l.panel) && cst("ui/node.rect", &l.rect);
    // what the reads rely on: the panel's node is the child frame's second word, small offsets inside the frame and node
    if (on && (l.subframe <= 0 || l.subframe > 0x1000 || l.panel != l.subframe + 8 || l.rect <= 0 || l.rect > 0x1000)) {
        logf(KILN_LOG_WARN, "ui: ui.popups constants look wrong on this build: Ui.Inventory marks ignore the game's popups");
        on = false;
    }
    if (!on) {
        logf(KILN_LOG_INFO, "ui: the game's popup panels are not read on this build (feature ui.popups off): "
                            "Ui.Inventory marks ignore them");
        return false;
    }
    l.error_on = feat("ui.error_popup") && cst("ui/sys.popups", &l.popups) && cst("ui/popups.error", &l.error);
    P = l;  // g_on is false: no UI frame reads it (hk_begin counts nothing before popups_frame_begin saw g_on)
    g_begin_hook.install(g_api, g_owner, P.begin, reinterpret_cast<void*>(&hk_begin), "ui/PopupWindowBegin");
    if (!g_begin_hook) {
        logf(KILN_LOG_WARN, "ui: hooking the game's popup-panel builder failed (%s): Ui.Inventory marks ignore the game's popups",
             g_begin_hook.error() ? g_begin_hook.error() : "?");
        return false;
    }
    {
        std::lock_guard lk(g_mu);
        g_state.available = 1;
    }
    g_on.store(true, std::memory_order_release);
    logf(KILN_LOG_INFO, "ui: the game's popup panels are read (error popup %s): Ui.Inventory marks step aside for them",
         P.error_on ? "on" : "off");
    return true;
}

void popups_frame_begin() {
    if (!g_on.load(std::memory_order_acquire)) return;
    t_collect = Collect{};
    t_collect.active = true;
}

void popups_after_frame(uintptr_t sys10, uint64_t frame) {
    if (!g_on.load(std::memory_order_acquire)) return;
    Collect c = t_collect;
    t_collect = Collect{};
    if (!c.active) c = Collect{};  // a frame that did not begin here (not seen): publish no panel rather than a stale one
    uint8_t error = 0;
    if (P.error_on && sys10 && rd(sys10 + static_cast<uintptr_t>(P.popups + P.error), error) && error)
        c.flags |= KILN_UI_POPUP_ERROR;
    std::lock_guard lk(g_mu);
    g_state.flags = c.flags;
    g_state.count = c.count;
    g_state.frame = frame;
    std::memset(g_state.rect, 0, sizeof g_state.rect);
    std::memcpy(g_state.rect, c.rect, sizeof(KilnUiPopupRect) * c.count);
}

KilnResult popups_state(KilnUiPopups* out) {
    if (!out || out->size < sizeof(KilnUiPopups)) return KILN_ERR_INVALID_ARGUMENT;
    {
        std::lock_guard lk(g_mu);
        *out = g_state;
    }
    out->size = sizeof(KilnUiPopups);
    return out->available ? KILN_OK : KILN_ERR_UNAVAILABLE;
}

#ifdef KILN_UI_POPUPS_TESTS
void popups_reset_for_tests() {
    g_on.store(false, std::memory_order_relaxed);
    P = PopupLayout{};
    t_collect = Collect{};
    std::lock_guard lk(g_mu);
    g_state = KilnUiPopups{};
    g_begin_hook.revoke();
}
#endif

}  // namespace kui
