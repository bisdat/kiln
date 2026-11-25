// SPDX-License-Identifier: MIT
// The game's popup panels (popups.cpp, GitHub issue 117) against stand-ins for the game, under ASan/UBSan: a fake popup-panel
// builder that returns its frame, a frame whose panel node sits at +0x1a8 and a node whose laid-out rectangle sits at
// +0x114 (client 26430625's offsets), a fake UI system block with the error popup byte, and kiln.hooks' test RawHook.
// Checks: the feature or a constant missing, odd constants and a refused hook leave it off (popups() UNAVAILABLE,
// available 0: marks as before); a panel drawn in a frame is published with its rectangle after that frame and gone the
// frame after; an unreadable, empty or not finite rectangle and a fifth panel say UNKNOWN; a builder call outside a UI
// frame is not counted; the error popup; the caller's size.
#define KILN_UI_POPUPS_TESTS 1
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <limits>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "../popups.cpp"

namespace kui {
const KilnApi* g_api = nullptr;
KilnOwner g_owner = 7;
static std::vector<std::string> g_log;
void logf(int, const char* fmt, ...) {
    char b[512];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    g_log.emplace_back(b);
}
void popups_reset_for_tests();
}  // namespace kui

static int g_fail = 0, g_checks = 0;
#define CHECK(cond)                                                     \
    do {                                                                \
        ++g_checks;                                                     \
        if (!(cond)) {                                                  \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++g_fail;                                                   \
        }                                                               \
    } while (0)

// ---- the fake game ----
struct Panel {
    alignas(16) uint8_t frame[0x500];
    alignas(16) uint8_t node[0x200];
};
static Panel g_panels[6];
alignas(16) static uint8_t g_system[0x800];  // the UI system: GameUiFrame gets system + 0x10
static int g_begin_calls = 0;
static uintptr_t fake_begin(uintptr_t frame, void*, void*, void*) {
    ++g_begin_calls;
    return frame;
}
static void set_rect(Panel& p, float x, float y, float w, float h) {
    const float r[4] = {x, y, w, h};
    std::memcpy(p.node + 0x114, r, sizeof r);
    const uintptr_t node = reinterpret_cast<uintptr_t>(p.node);
    std::memcpy(p.frame + 0x1a8, &node, sizeof node);
}
static uintptr_t sys10() { return reinterpret_cast<uintptr_t>(g_system) + 0x10; }

// ---- the fake KilnApi (sigdb) ----
static std::set<std::string> g_features;
static std::map<std::string, uintptr_t> g_symbols;
static std::map<std::string, int64_t> g_constants;
static std::string str(KilnStr s) { return std::string(s.ptr, s.len); }
static bool KILN_CALL api_feature(KilnStr n) { return g_features.count(str(n)) > 0; }
static KilnResult KILN_CALL api_symbol(KilnStr n, uintptr_t* out) {
    auto it = g_symbols.find(str(n));
    if (it == g_symbols.end()) return KILN_ERR_NOT_FOUND;
    *out = it->second;
    return KILN_OK;
}
static KilnResult KILN_CALL api_constant(KilnStr n, int64_t* out) {
    auto it = g_constants.find(str(n));
    if (it == g_constants.end()) return KILN_ERR_NOT_FOUND;
    *out = it->second;
    return KILN_OK;
}
static void sigdb_26430625() {
    g_features = {"ui.popups", "ui.error_popup"};
    g_symbols = {{"ui/PopupWindowBegin", reinterpret_cast<uintptr_t>(&fake_begin)}};
    g_constants = {{"ui/popup.subframe", 0x1a0}, {"ui/popup.panel", 0x1a8}, {"ui/node.rect", 0x114},
                   {"ui/sys.popups", 0x120},     {"ui/popups.error", 0x550}};
}

static uint64_t g_frame = 0;
// One UI frame as kiln.ui's GameUiFrame link runs it: begin, the game's frame (the panels it builds), the after-frame.
static void ui_frame(std::vector<Panel*> panels) {
    kui::popups_frame_begin();
    for (Panel* p : panels) {  // through Kiln's link while it is in, else the game's builder alone
        const uintptr_t f = reinterpret_cast<uintptr_t>(p->frame);
        if (kui::g_begin_hook) kui::hk_begin(f, nullptr, nullptr, nullptr);
        else fake_begin(f, nullptr, nullptr, nullptr);
    }
    kui::popups_after_frame(sys10(), ++g_frame);
}
static KilnUiPopups state(KilnResult* rc = nullptr) {
    KilnUiPopups s{};
    s.size = sizeof s;
    const KilnResult r = kui::popups_state(&s);
    if (rc) *rc = r;
    return s;
}
static bool fresh() {
    kui::popups_reset_for_tests();
    kiln::test::fail_all = false;
    std::memset(g_panels, 0, sizeof g_panels);
    std::memset(g_system, 0, sizeof g_system);
    g_frame = 0;
    g_begin_calls = 0;
    return kui::popups_install();
}

int main() {
    KilnApi api{};
    api.feature = &api_feature;
    api.symbol = &api_symbol;
    api.constant = &api_constant;
    kui::g_api = &api;
    KilnResult rc = KILN_OK;

    // ---- off: marks as before ----
    sigdb_26430625();
    g_features.erase("ui.popups");
    CHECK(!fresh());
    ui_frame({&g_panels[0]});
    CHECK(state(&rc).available == 0 && rc == KILN_ERR_UNAVAILABLE && state().flags == 0);
    sigdb_26430625();
    g_constants.erase("ui/node.rect");
    CHECK(!fresh());
    sigdb_26430625();
    g_constants["ui/popup.panel"] = 0x1b0;  // not the child frame's node word: another build's layout
    CHECK(!fresh());
    CHECK(state(&rc).available == 0 && rc == KILN_ERR_UNAVAILABLE);
    sigdb_26430625();
    kiln::test::fail_all = true;  // the hook refused
    kui::popups_reset_for_tests();
    CHECK(!kui::popups_install());
    kiln::test::fail_all = false;
    CHECK(state(&rc).available == 0 && rc == KILN_ERR_UNAVAILABLE);

    // ---- on ----
    sigdb_26430625();
    CHECK(fresh());
    CHECK(state(&rc).available == 1 && rc == KILN_OK && state().frame == 0 && state().flags == 0);
    ui_frame({});
    CHECK(state().flags == 0 && state().count == 0 && state().frame == 1);
    // the item menu: a panel at (900, 400) 330 x 180 UI units, through the original builder
    set_rect(g_panels[0], 900, 400, 330, 180);
    ui_frame({&g_panels[0]});
    KilnUiPopups s = state(&rc);
    CHECK(rc == KILN_OK && s.flags == KILN_UI_POPUP_PANEL && s.count == 1 && s.frame == 2);
    CHECK(s.rect[0].x == 900 && s.rect[0].y == 400 && s.rect[0].width == 330 && s.rect[0].height == 180);
    CHECK(g_begin_calls == 1);  // the original ran, once
    // closed: the next frame draws none
    ui_frame({});
    s = state();
    CHECK(s.flags == 0 && s.count == 0 && s.rect[0].width == 0 && s.frame == 3);
    // two panels (a drop-down over a menu)
    set_rect(g_panels[1], 10, 20, 30, 40);
    ui_frame({&g_panels[0], &g_panels[1]});
    s = state();
    CHECK(s.flags == KILN_UI_POPUP_PANEL && s.count == 2 && s.rect[1].x == 10 && s.rect[1].height == 40);
    // its first frame: the node has no layout yet
    ui_frame({&g_panels[2]});
    s = state();
    CHECK(s.flags == (KILN_UI_POPUP_PANEL | KILN_UI_POPUP_UNKNOWN) && s.count == 0);
    // a node pointer the builder did not set (null): unknown, no read through it
    set_rect(g_panels[3], 1, 1, 1, 1);
    std::memset(g_panels[3].frame + 0x1a8, 0, 8);
    ui_frame({&g_panels[3], &g_panels[0]});
    s = state();
    CHECK(s.flags == (KILN_UI_POPUP_PANEL | KILN_UI_POPUP_UNKNOWN) && s.count == 1 && s.rect[0].x == 900);
    // not finite, negative size
    set_rect(g_panels[4], std::numeric_limits<float>::quiet_NaN(), 0, 10, 10);
    ui_frame({&g_panels[4]});
    CHECK(state().flags == (KILN_UI_POPUP_PANEL | KILN_UI_POPUP_UNKNOWN) && state().count == 0);
    set_rect(g_panels[4], 0, 0, -10, 10);
    ui_frame({&g_panels[4]});
    CHECK(state().flags == (KILN_UI_POPUP_PANEL | KILN_UI_POPUP_UNKNOWN));
    // five panels: four rectangles, the fifth unknown
    for (int i = 0; i < 5; ++i) set_rect(g_panels[i], 100.0f * i, 0, 50, 50);
    ui_frame({&g_panels[0], &g_panels[1], &g_panels[2], &g_panels[3], &g_panels[4]});
    s = state();
    CHECK(s.count == KILN_UI_POPUPS_MAX && s.flags == (KILN_UI_POPUP_PANEL | KILN_UI_POPUP_UNKNOWN) && s.rect[3].x == 300);
    // a builder call outside a UI frame is not counted (and the original still runs)
    ui_frame({});
    const int calls = g_begin_calls;
    kui::hk_begin(reinterpret_cast<uintptr_t>(g_panels[0].frame), nullptr, nullptr, nullptr);
    CHECK(g_begin_calls == calls + 1);
    kui::popups_after_frame(sys10(), ++g_frame);
    CHECK(state().flags == 0 && state().count == 0);
    // the game's error popup
    g_system[0x10 + 0x120 + 0x550] = 1;
    ui_frame({});
    CHECK(state().flags == KILN_UI_POPUP_ERROR && state().count == 0);
    g_system[0x10 + 0x120 + 0x550] = 0;
    ui_frame({});
    CHECK(state().flags == 0);
    // without ui.error_popup the byte is not read
    sigdb_26430625();
    g_features.erase("ui.error_popup");
    CHECK(fresh());
    g_system[0x10 + 0x120 + 0x550] = 1;
    ui_frame({});
    CHECK(state().flags == 0);

    // ---- the caller's struct ----
    CHECK(kui::popups_state(nullptr) == KILN_ERR_INVALID_ARGUMENT);
    KilnUiPopups small{};
    small.size = sizeof small - 1;
    CHECK(kui::popups_state(&small) == KILN_ERR_INVALID_ARGUMENT);
    KilnUiPopups big[2]{};
    big[0].size = sizeof big;  // a newer caller's bigger struct: our part, size set to ours
    CHECK(kui::popups_state(big) == KILN_OK && big[0].size == sizeof(KilnUiPopups) && big[0].available == 1);

    std::printf("popups host test: %d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
