// SPDX-License-Identifier: MIT
// kiln.ui.values in kiln-core: the production environment of ui_values*.cpp (owners, kiln.caps rows, sigdb, kiln.hooks
// links, kiln.client's local player and session, kiln.ui's last screen, guarded game-memory access, the log), the SEH
// guard around providers, and the service's registration.
//
// Client only: on a dedicated server init() says no (there is no game UI to hook). On the client nothing is hooked at
// boot: each kind installs its links when the first provider of that kind registers and kiln.caps shows the kind's row
// available. Slot snapshot reads also install the inspection links on first use.
#include <cstddef>
#include <cstring>
#include <string>

#include "../api.h"
#include "../common.h"
#include "../config.h"
#include "../gates.h"
#include "../crash.h"
#include "../guard.h"
#include "../image.h"
#include "../log.h"
#include "../registry.h"
#include "../sigdb.h"
#include "hooks_internal.h"
#include "service.h"
#include "services/kiln_caps.h"
#include "services/kiln_client.h"
#include "services/kiln_items.h"
#include "services/kiln_ui.h"
#include "ui_values_internal.h"

namespace kiln::hooks::seh {
bool copy(void* dst, const void* src, size_t n);  // hooks_seh.cpp: a write that may fault returns false
}  // namespace kiln::hooks::seh

namespace kiln::uivalues {
namespace {

KilnOwner g_owner = 0;  // the service's own owner: its kiln.caps reports
std::atomic<const KilnUiApi*> g_ui{nullptr};

const KilnCapsApi* caps() { return static_cast<const KilnCapsApi*>(svc::find(KILN_CAPS_SERVICE, 1)); }

// A kind installs only when its capability row is available; retain policy-hold support for future gates.
bool row_available(const char* name) {
    const KilnCapsApi* c = caps();
    const KilnStr n{name, std::strlen(name)};
    if (!c || kiln_caps_held(c, n)) return false;
    KilnCapRow row{};
    row.size = sizeof row;
    return c->find(n, &row) == KILN_OK && row.state == KILN_CAP_AVAILABLE;
}

void row_off(const char* name, const char* text) {
    const KilnCapsApi* c = caps();
    if (c && g_owner)
        c->report(g_owner, KilnStr{name, std::strlen(name)}, KILN_CAP_UNAVAILABLE, KILN_CAP_REASON_FAULTED,
                  KilnStr{text, std::strlen(text)});
}

bool client_now(ClientNow* out) {
    static const auto* client = static_cast<const KilnClientApi*>(svc::find(KILN_CLIENT_SERVICE, 1));
    if (!client) return false;
    KilnClientState s{};
    s.size = sizeof s;
    if (client->state(&s) != KILN_OK) return false;
    out->local_player = s.local_player;
    out->session = s.client_session;
    out->step = s.client_steps;
    return true;
}

bool screen(KilnUiScreen* out) {
    const KilnUiApi* ui = g_ui.load(std::memory_order_acquire);
    if (!ui) {  // kiln-ui.dll publishes its table after the core started
        ui = static_cast<const KilnUiApi*>(api()->service(KilnStr{KILN_UI_SERVICE, sizeof(KILN_UI_SERVICE) - 1}, 1));
        if (!ui || ui->header.size < offsetof(KilnUiApi, screen) + sizeof(void*)) return false;
        g_ui.store(ui, std::memory_order_release);
    }
    out->size = sizeof(KilnUiScreen);
    return ui->screen(out) == KILN_OK;
}

KilnResult hook_raw(uintptr_t target, const char* label, int32_t priority, void* fn, void** original) {
    if (!target || !hooks::ensure_ready()) return KILN_ERR_UNAVAILABLE;
    hooks::chain::Order o;
    o.priority = priority;
    o.label = label;
    KilnHandle h = 0;
    return hooks::chain::install_raw(0, target, o, fn, original, &h);
}

KilnResult hook_mid(uintptr_t target, const char* label, KilnMidFn fn) {
    if (!target || !hooks::ensure_ready()) return KILN_ERR_UNAVAILABLE;
    hooks::chain::Order o;
    o.label = label;
    KilnHandle h = 0;
    return hooks::chain::install_mid(0, target, o, fn, nullptr, &h);
}

// Holds no C++ object with a destructor, as __try requires (guard.cpp's pattern).
bool call_value(KilnUiValueFn fn, const KilnUiProviderContext* c, const KilnUiItemView* v, const KilnUiBarValue* in,
                KilnUiBarEdit* e, void* user, KilnResult* rc) {
    crash::Capture cap{};
    unsigned long code = 0;
    __try {
        *rc = fn(c, v, in, e, user);
        return true;
    } __except (crash::filter(GetExceptionInformation(), &cap, &code)) {
        crash::report(cap, "kiln.ui.values provider", reinterpret_cast<const void*>(fn));
        return false;
    }
}

bool call_changed(KilnUiValueChangedFn fn, const KilnUiValueChange* c, void* user) {
    crash::Capture cap{};
    unsigned long code = 0;
    __try {
        fn(c, user);
        return true;
    } __except (crash::filter(GetExceptionInformation(), &cap, &code)) {
        crash::report(cap, "kiln.ui.values change subscriber", reinterpret_cast<const void*>(fn));
        return false;
    }
}

bool call_lines(KilnUiLinesFn fn, const KilnUiProviderContext* c, const KilnUiItemView* v, KilnUiLineWriter* w,
                void* user, KilnResult* rc) {
    crash::Capture cap{};
    unsigned long code = 0;
    __try {
        *rc = fn(c, v, w, user);
        return true;
    } __except (crash::filter(GetExceptionInformation(), &cap, &code)) {
        crash::report(cap, "kiln.ui.values lines provider", reinterpret_cast<const void*>(fn));
        return false;
    }
}

bool call_scale(KilnUiScaleFn fn, const KilnUiProviderContext* c, const KilnUiItemView* v, float* scale, void* user,
                KilnResult* rc) {
    crash::Capture cap{};
    unsigned long code = 0;
    __try {
        *rc = fn(c, v, scale, user);
        return true;
    } __except (crash::filter(GetExceptionInformation(), &cap, &code)) {
        crash::report(cap, "kiln.ui.values tooltip value provider", reinterpret_cast<const void*>(fn));
        return false;
    }
}

bool call_slots(KilnUiSlotsFn fn, const KilnUiSlotsFrame* f, const KilnUiVisibleSlot* s, uint32_t n, void* user) {
    crash::Capture cap{};
    unsigned long code = 0;
    __try {
        fn(f, s, n, user);
        return true;
    } __except (crash::filter(GetExceptionInformation(), &cap, &code)) {
        crash::report(cap, "kiln.ui.values slots subscriber", reinterpret_cast<const void*>(fn));
        return false;
    }
}
bool call_slots_v2(KilnUiSlotsFnV2 fn, const KilnUiSlotsFrame* f, const KilnUiVisibleSlotV2* s, uint32_t n, void* user) {
    crash::Capture cap{};
    unsigned long code = 0;
    __try {
        fn(f, s, n, user);
        return true;
    } __except (crash::filter(GetExceptionInformation(), &cap, &code)) {
        crash::report(cap, "kiln.ui.values slots subscriber", reinterpret_cast<const void*>(fn));
        return false;
    }
}

bool call_gemforge(KilnUiGemForgeFn fn, const KilnUiGemForgeFrame* f, const KilnUiGemForgeCandidate* rows,
                  uint32_t n, void* user) {
    crash::Capture cap{};
    unsigned long code = 0;
    __try {
        fn(f, rows, n, user);
        return true;
    } __except (crash::filter(GetExceptionInformation(), &cap, &code)) {
        crash::report(cap, "kiln.ui.values Gem Forge subscriber", reinterpret_cast<const void*>(fn));
        return false;
    }
}

// One native tooltip line, built as the game builds the detail's description (TextDraw, ui_values_internal.h), inside
// the item detail view on the UI thread. No C++ objects with destructors here (__try).
struct Float2 {
    float x, y;
};
using TextLineFn = void*(__fastcall*)(void* out, const void* text, const void* style, uint32_t mode);
using TextDrawFn = void(__fastcall*)(void* text);
using NodeAlignFn = void(__fastcall*)(void* node, const uint16_t* align);
using NodeSizeFn = void(__fastcall*)(void* node, Float2 size);
using NodeRectFn = const float*(__fastcall*)(void* node);
bool draw_text(const TextDraw* d, uintptr_t* node_out) {
    crash::Capture cap{};
    unsigned long code = 0;
    __try {
        alignas(16) uint8_t obj[0x200];  // the game's text object is 0x150 bytes (its constructor writes through +0x140)
        std::memset(obj, 0, sizeof obj);
        const text::GameText t{d->text, d->text_len, d->text_fnv};
        reinterpret_cast<TextLineFn>(d->line_fn)(obj, &t, reinterpret_cast<const void*>(d->style), 1);
        void* node = *reinterpret_cast<void**>(obj + d->node);
        *node_out = reinterpret_cast<uintptr_t>(node);
        obj[d->shown] = 1;
        const uint16_t align = d->align;
        reinterpret_cast<NodeAlignFn>(d->align_fn)(node, &align);
        std::memcpy(obj + d->align_field, &align, 2);
        obj[d->wrap] = 1;
        reinterpret_cast<NodeSizeFn>(d->size_fn)(node, Float2{1.0f, 0.0f});  // fill the width, fit the height
        uint16_t flags = 0;
        std::memcpy(&flags, obj + d->flags, 2);
        if (d->colored) {
            obj[d->bits] |= 1;
            flags |= 0x40;
            std::memcpy(obj + d->color, &d->color_value, 8);
            const uint16_t zero = 0;
            std::memcpy(obj + d->flags2, &zero, 2);
        }
        if (d->small) {
            flags |= 8;
            std::memcpy(obj + d->size_field, &d->small_size, 4);
        }
        std::memcpy(obj + d->flags, &flags, 2);
        reinterpret_cast<TextDrawFn>(d->draw_fn)(obj);
        return true;
    } __except (crash::filter(GetExceptionInformation(), &cap, &code)) {
        crash::report(cap, "kiln.ui.values native tooltip line", reinterpret_cast<const void*>(d->line_fn));
        return false;
    }
}

bool node_rect(uintptr_t fn, uintptr_t node, float out[4]) {
    __try {
        const float* r = reinterpret_cast<NodeRectFn>(fn)(reinterpret_cast<void*>(node));
        if (!r) return false;
        for (int i = 0; i < 4; ++i) out[i] = r[i];
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// The tooltip probe's switch: KILN_TOOLTIP_PROBE (1/true or a key name) or kiln.json "tooltipProbe". nullptr = off.
bool probe_switch(std::string* key) {
    wchar_t buf[32] = {};
    const DWORD n = GetEnvironmentVariableW(L"KILN_TOOLTIP_PROBE", buf, 32);
    std::string v;
    if (n > 0 && n < 32)
        for (DWORD i = 0; i < n; ++i) v += buf[i] < 128 ? static_cast<char>(buf[i]) : '?';
    else
        v = config().tooltip_probe;
    if (v.empty() || v == "0" || v == "false") return false;
    *key = v == "1" || v == "true" || v == "default" ? "" : v;
    return true;
}

// kiln.ui's UI tick (after every game UI frame, on the UI thread) drives change publication.
void (*g_frame_fn)(uint64_t, void*) = nullptr;
void* g_frame_user = nullptr;
void KILN_CALL on_ui_tick(const KilnUiScreen* now, void*) {
    if (g_frame_fn && now) g_frame_fn(now->frame, g_frame_user);
}
bool subscribe_frames(void (*fn)(uint64_t, void*), void* user) {
    KilnUiScreen probe{};
    if (!screen(&probe)) return false;  // also finds kiln.ui's table
    const KilnUiApi* ui = g_ui.load(std::memory_order_acquire);
    if (!ui || ui->header.size < offsetof(KilnUiApi, subscribe_ui_tick) + sizeof(void*) || !g_owner) return false;
    g_frame_fn = fn;
    g_frame_user = user;
    KilnHandle h = 0;
    return ui->subscribe_ui_tick(g_owner, &on_ui_tick, nullptr, &h) == KILN_OK;
}

// kiln.ui.inventory: kiln.ui's presses (subscribe_pointer_press, 0.2.0-dev.30), with the service's owner.
bool subscribe_pointer(KilnUiPointerFn fn, void* user) {
    KilnUiScreen probe{};
    if (!screen(&probe)) return false;  // also finds kiln.ui's table
    const KilnUiApi* ui = g_ui.load(std::memory_order_acquire);
    if (!ui || ui->header.size < offsetof(KilnUiApi, subscribe_pointer_press) + sizeof(void*) || !ui->subscribe_pointer_press ||
        !g_owner)
        return false;
    KilnHandle h = 0;
    return ui->subscribe_pointer_press(g_owner, fn, user, &h) == KILN_OK;
}

// kiln.ui.inventory v2: kiln.ui's screen_target (0.2.0-dev.29.15 tail; since dev.30 also a chest's), 0 without it.
uint32_t screen_target() {
    KilnUiScreen probe{};
    if (!screen(&probe)) return 0;  // also finds kiln.ui's table
    const KilnUiApi* ui = g_ui.load(std::memory_order_acquire);
    if (!ui || ui->header.size < offsetof(KilnUiApi, screen_target) + sizeof(void*) || !ui->screen_target) return 0;
    return ui->screen_target();
}

bool call_click(KilnUiSlotClickFn fn, const KilnUiSlotClick* c, void* user) {
    crash::Capture cap{};
    unsigned long code = 0;
    __try {
        fn(c, user);
        return true;
    } __except (crash::filter(GetExceptionInformation(), &cap, &code)) {
        crash::report(cap, "kiln.ui.inventory click subscriber", reinterpret_cast<const void*>(fn));
        return false;
    }
}

// The cursor in the client pixels of this process's window in front (the game's), with the client area's size.
bool cursor_client(float* x, float* y, float* w, float* h) {
    HWND wnd = GetForegroundWindow();
    DWORD pid = 0;
    if (!wnd || !GetWindowThreadProcessId(wnd, &pid) || pid != GetCurrentProcessId()) return false;
    POINT p{};
    RECT rc{};
    if (!GetCursorPos(&p) || !ScreenToClient(wnd, &p) || !GetClientRect(wnd, &rc)) return false;
    *x = static_cast<float>(p.x);
    *y = static_cast<float>(p.y);
    *w = static_cast<float>(rc.right - rc.left);
    *h = static_cast<float>(rc.bottom - rc.top);
    return *x >= 0 && *y >= 0 && *x < *w && *y < *h;
}

bool gemforge_contract() {
    uintptr_t grid = 0, lower = 0, end = 0;
    hooks::fp::Fingerprint fp;
    // The standard pass stops at 300 instructions here. Its exact measured prefix is a guard, not a full type proof.
    // The scoped LowerBoundary signature separately proves the fifth (float) stack argument beyond that prefix.
    if (!sigdb::symbol("ui/GemForgeGrid", &grid) || !sigdb::symbol("ui/GemForgeLowerBoundary", &lower) ||
        !sigdb::symbol("ui/GemForgeEnd", &end) ||
        !sigdb::fingerprint(grid, &fp) || !fp.ok || fp.thunk || fp.reads != 0x0f || fp.passes != 0xd0 ||
        fp.indirect || fp.stack != 2 || fp.stack_blind || fp.tail || fp.rets != hooks::fp::kRetRax ||
        fp.insns != 300 || !fp.truncated) return false;
    uint8_t file_entry[64]{}, live_entry[64]{};
    // Start with an untouched grid entry. A pre-existing foreign detour is refused for this first implementation.
    if (!sigdb::file_bytes(grid, file_entry, sizeof file_entry) ||
        !guard::copy(live_entry, grid, sizeof live_entry) || std::memcmp(file_entry, live_entry, sizeof file_entry))
        return false;
    if (!sigdb::file_bytes(end, file_entry, sizeof file_entry) ||
        !guard::copy(live_entry, end, sizeof live_entry) || std::memcmp(file_entry, live_entry, sizeof file_entry))
        return false;
    // Read the original file code, never a live detour. Captured table offsets must dispatch screen 45 to the menu.
    int64_t map = 0, targets = 0;
    uintptr_t screen_call = 0;
    uint8_t index = 0;
    uint32_t target = 0;
    if (!sigdb::constant("ui/gemforge.switchMap", &map) || map < 0 ||
        !sigdb::constant("ui/gemforge.switchTargets", &targets) || targets < 0 ||
        static_cast<uint64_t>(map) + 24 > image().size || static_cast<uint64_t>(targets) + 20 > image().size ||
        !sigdb::symbol("ui/GemForgeScreenCall", &screen_call) ||
        !sigdb::file_bytes(image().base + map + (45 - 22), &index, 1) || index != 4 ||
        !sigdb::file_bytes(image().base + targets + index * 4, reinterpret_cast<uint8_t*>(&target), 4) ||
        image().base + target != screen_call) return false;
    struct Edge { const char* site; const char* callee; uint32_t back; };
    const Edge edges[] = {
        {"ui/GemForgeScreenCall", "ui/GemForgeMenu", 0},
        {"ui/GemForgeListsCall", "ui/GemForgeLists", 0},
        {"ui/GemForgeGemsCall", "ui/GemForgeGrid", 0},
        {"ui/GemForgeWeaponsCall", "ui/GemForgeGrid", 0},
        {"ui/GemForgeSourceRet", "ui/GemForgeCandidates", 5},
        {"ui/GemForgeSlotRet", "ui/ItemSlot", 5}};
    for (const auto& e : edges) {
        uintptr_t site = 0, callee = 0;
        uint8_t bytes[5]{};
        int32_t relative = 0;
        if (!sigdb::symbol(e.site, &site) || site < e.back || !sigdb::symbol(e.callee, &callee) ||
            !sigdb::file_bytes(site - e.back, bytes, sizeof bytes) || bytes[0] != 0xe8) return false;
        std::memcpy(&relative, bytes + 1, 4);
        if (static_cast<int64_t>(site - e.back + 5) + relative != static_cast<int64_t>(callee)) return false;
    }
    return true;
}

void wire() {
    Env& e = env();
    e.owner_name = [](KilnOwner o) { return reg::owner_name(o); };
    e.owner_alive = [](KilnOwner o) { return reg::owner_alive(o); };
    e.registry_version = [] { return reg::version(); };
    e.now_ms = [] { return kiln::now_ms(); };
    e.tick_ms = []() -> uint64_t { return GetTickCount64(); };
    e.client_side = [] { return image().side == KILN_SIDE_CLIENT; };
    // 05a's fixed private fields and mid-hook register contracts were derived only on this image.
    // A matching code pattern alone cannot prove those layouts on another executable, even with policy=any.
    e.layout_supported = [] {
        return image().sha256 == "5d9ef2f140b8ed49e30dbee655841b89d08baec20f799a1817e171a18a850c77";
    };
    e.row_available = &row_available;
    e.row_off = &row_off;
    e.client = &client_now;
    e.screen = &screen;
    e.read = [](void* dst, uintptr_t src, size_t n) { return src != 0 && guard::copy(dst, src, n); };
    e.write = [](uintptr_t dst, const void* src, size_t n) {
        return dst != 0 && hooks::seh::copy(reinterpret_cast<void*>(dst), src, n);
    };
    e.symbol = [](const char* n, uintptr_t* out) { return sigdb::symbol(n, out) && *out != 0; };
    e.constant = [](const char* n, int64_t* out) { return sigdb::constant(n, out); };
    e.hook_raw = &hook_raw;
    e.hook_mid = &hook_mid;
    e.call_value = &call_value;
    e.call_changed = &call_changed;
    e.call_slots = &call_slots;
    e.call_slots_v2 = &call_slots_v2;
    e.call_lines = &call_lines;
    e.call_gemforge = &call_gemforge;
    e.gemforge_abi = &gemforge_contract;
    e.item_category = [](uint32_t id, uint8_t* category) {
        // kiln.items is published by kiln-items.dll, not an in-core service: svc::find never sees it (the real client
        // refused every Gem Forge frame that needed the catalog until this asked the published list).
        const auto* items = static_cast<const KilnItemsApi*>(
            api()->service(KilnStr{KILN_ITEMS_SERVICE, sizeof(KILN_ITEMS_SERVICE) - 1}, 1));
        if (!items || items->header.size < offsetof(KilnItemsApi, item_info) + sizeof(void*) || !items->item_info)
            return -1;
        KilnItemInfo info{};
        info.size = sizeof info;
        const KilnResult rc = items->item_info(id, &info);
        if (rc == KILN_ERR_NOT_FOUND) return 0;
        if (rc != KILN_OK) return -1;
        *category = info.category;
        return 1;
    };
    e.subscribe_frames = &subscribe_frames;
    e.draw_text = &draw_text;
    e.call_scale = &call_scale;
    e.node_rect = &node_rect;
    e.key_down = [](uint32_t vk) { return (GetAsyncKeyState(static_cast<int>(vk)) & 0x8000) != 0; };
    e.thread_id = []() -> uint32_t { return GetCurrentThreadId(); };
    e.feature_open = [](const char* n) { return sigdb::feature(n) && !gates::is_held(n); };
    e.subscribe_pointer = &subscribe_pointer;
    e.call_click = &call_click;
    e.cursor_client = &cursor_client;
    e.screen_target = &screen_target;
    e.log = [](int level, const char* text) { kiln::log::write(level, "kiln", text); };
}

bool init() {
    if (image().side != KILN_SIDE_CLIENT) {
        KLOG_I("ui.values: client only, not started on a server");
        return false;
    }
    wire();
    g_owner = reg::owner_create("kiln.ui.values");
    if (reg::add_owner_retiring(g_owner, [](KilnOwner owner, void*) { owner_retired(owner); }, nullptr, nullptr) != KILN_OK)
        return false;
    // Each feature resolves on its own; the rows (and the client gate) decide at registration time.
    static const char* const kFeatures[] = {"ui.values.detail", "ui.values.slot",          "ui.values.held",
                                            "ui.values.hud",    "ui.values.tooltip_lines", "ui.slots.inspect",
                                            "ui.gemforge.inspect", "gems.sockets", "ui.gemforge.sources",
                                            "ui.tooltip.lines", "ui.tooltip.values", "ui.slots.click",
                                            "ui.inventory.panels"};
    if (!env().layout_supported())
        for (uint32_t k = 0; k < kLists; ++k)
            if (row_of(k)) disable_kind(k, "private UI layout is not measured for this client image");
    std::string on, off;
    for (const char* f : kFeatures) {
        std::string& to = sigdb::feature(f) ? on : off;
        to += ' ';
        to += f;
    }
    KLOG_I(
        "ui.values: features resolved:%s; not resolved:%s. A kind hooks the game when its first provider "
        "registers and its row is available; slot inspection also hooks on first copy_slots",
        on.empty() ? " none" : on.c_str(), off.empty() ? " none" : off.c_str());
    std::string key;
    if (probe_switch(&key)) probe_start(g_owner, key.c_str());
    return true;
}

}  // namespace
}  // namespace kiln::uivalues

KILN_SERVICE(ui_values, KILN_UI_VALUES_SERVICE, KILN_UI_VALUES_VERSION, kiln::uivalues::table(), &kiln::uivalues::init);

namespace {
// kiln.ui.inventory shares kiln.ui.values' registry, environment and slot frames: available where that one is.
bool inventory_init() { return kiln::image().side == KILN_SIDE_CLIENT; }
}  // namespace
KILN_SERVICE(ui_inventory, KILN_UI_INVENTORY_SERVICE, KILN_UI_INVENTORY_VERSION, kiln::uivalues::inventory_table(),
             &inventory_init);

// Independent v1 table; no append to any shared service ABI.
KILN_SERVICE(ui_item_detail, KILN_UI_ITEM_DETAIL_SERVICE, KILN_UI_ITEM_DETAIL_VERSION,
             kiln::uivalues::item_detail_table(), &inventory_init);

KILN_SERVICE(ui_slots, KILN_UI_SLOTS_SERVICE, KILN_UI_SLOTS_VERSION, kiln::uivalues::slots_table(), &inventory_init);
