// SPDX-License-Identifier: MIT
// kiln.ui.inventory v1 (abi/services/kiln_ui_inventory.h): slot clicks, slot_at and the units-to-pixels mapping, on the
// slot frames of ui_values_slots.cpp and kiln.ui's presses (subscribe_pointer_press). Plain C++ over Env.
//
//   press (window thread)   on_pointer: the latest slot frame (this session, <= 250 ms, a viewport, a pointer screen),
//                           the slot under the press (outer rectangle, last drawn wins), the matching subscribers; queues
//                           one click for them and answers TAKE when one of them asked for it. Anything missing: PASS.
//   frame (UI thread)       clicks_frame_published, from the slots' publish: delivers the queued clicks to the
//                           subscribers they matched that are still live, under the SEH guard (faults count as usual).
// A click subscriber is a registration of list kListClicks (row ui.slots.click) whose item field packs the filter:
// button | mods << 8 | flags << 16.
#include <algorithm>
#include <cmath>
#include <cstring>
#include <mutex>
#include <vector>

#include "ui_values_internal.h"

namespace kiln::uivalues {
namespace {

constexpr uint64_t kMaxFrameAgeMs = 250;  // a press hit-tests a frame at most this old
constexpr size_t kMaxPending = 64;        // clicks waiting for the next frame
constexpr uint32_t kModsAll = KILN_UI_MOD_CTRL | KILN_UI_MOD_SHIFT | KILN_UI_MOD_ALT;

uint32_t pack_filter(const KilnUiSlotClickFilter& f) { return f.button | (f.mods << 8) | (f.flags << 16); }
uint32_t packed_button(uint32_t item) { return item & 0xff; }
uint32_t packed_mods(uint32_t item) { return (item >> 8) & 0xff; }
uint32_t packed_flags(uint32_t item) { return (item >> 16) & 0xff; }

struct Pending {
    KilnUiSlotClick click{};
    std::vector<KilnHandle> handles;  // the subscribers it matched at the press
};
struct ClickQueue {
    std::mutex mu;
    std::vector<Pending> items;
    uint64_t dropped = 0;
};
ClickQueue& click_queue() {
    static ClickQueue* q = new ClickQueue;  // never destroyed: the window thread may press while the process ends
    return *q;
}

uint32_t KILN_CALL on_pointer_cb(const KilnUiPointerPress* p, void*) { return on_pointer(p); }

bool install_clicks(uint32_t list) {
    if (list != kListClicks) return false;
    // the slot inspection is installed by t_subscribe_clicks first (ensure_installed holds its lock while this runs)
    if (!env().subscribe_pointer || !env().subscribe_pointer(&on_pointer_cb, nullptr)) {
        logf(KILN_LOG_ERROR, "ui.inventory: kiln.ui offers no subscribe_pointer_press (an older kiln-ui.dll?): no slot clicks");
        return false;
    }
    logf(KILN_LOG_INFO, "ui.inventory: slot clicks on (presses over the game's slots reach their subscribers)");
    return true;
}

struct ClicksInstaller {
    ClicksInstaller() { set_installer(kListClicks, &install_clicks); }
} g_clicks_installer;

// The latest frame if a press or a lookup may use it: this client session and recent.
std::shared_ptr<const SlotsSnapshot> usable_frame(uint32_t* reason) {
    auto f = slots_latest();
    if (!f) {
        *reason = KUI_NO_FRAME;
        return nullptr;
    }
    ClientNow now{};
    const uint64_t t = env().tick_ms ? env().tick_ms() : 0;
    if (!f->meta.ok || !env().client || !env().client(&now) || now.session != f->meta.session ||
        t < f->meta.capture_ms || t - f->meta.capture_ms > kMaxFrameAgeMs) {
        *reason = KUI_STALE_FRAME;
        return nullptr;
    }
    return f;
}

void inv_clear_registration(KilnUiRegistration* out) {
    out->ok = 0;
    out->reason = KUI_REASON_NONE;
    out->reserved = 0;
    out->handle = 0;
}

// ---- the table ----
KilnResult KILN_CALL t_subscribe_clicks(KilnOwner owner, const KilnUiSlotClickFilter* filter, KilnUiSlotClickFn fn,
                                        void* user, KilnUiRegistration* out) {
    if (!out || out->size < sizeof(KilnUiRegistration)) return KILN_ERR_INVALID_ARGUMENT;
    inv_clear_registration(out);
    if (owner == 0 || !fn || !filter || filter->size < sizeof(KilnUiSlotClickFilter) ||
        (filter->button != 1 && filter->button != 3) || (filter->mods & ~kModsAll) || (filter->flags & ~KUI_CLICK_TAKE))
        return KILN_ERR_INVALID_ARGUMENT;
    if (!env().client_side || !env().client_side()) return KILN_ERR_UNAVAILABLE;
    try {
        if (!ensure_installed(kListSlots)) return KILN_ERR_UNAVAILABLE;
        return register_list(owner, kListClicks, pack_filter(*filter), reinterpret_cast<void*>(fn), user, out);
    } catch (...) {
        return KILN_ERR_INTERNAL;
    }
}

KilnResult KILN_CALL t_slot_at(KilnOwner owner, float x, float y, float tw, float th, KilnUiSlotHit* out) {
    if (owner == 0 || !out || out->size < sizeof(KilnUiSlotHit)) return KILN_ERR_INVALID_ARGUMENT;
    if (!env().client_side || !env().client_side()) return KILN_ERR_UNAVAILABLE;
    if (!env().owner_alive(owner)) return KILN_ERR_REVOKED;
    const uint32_t size = out->size;
    *out = KilnUiSlotHit{};
    out->size = size;
    try {
        if (!ensure_installed(kListSlots)) return KILN_ERR_UNAVAILABLE;
        if (x < 0) {
            if (!env().cursor_client || !env().cursor_client(&x, &y, &tw, &th)) {
                out->reason = KUI_NO_SLOT;
                return KILN_OK;
            }
        }
        uint32_t reason = 0;
        const auto f = usable_frame(&reason);
        if (!f) {
            out->reason = reason;
            return KILN_OK;
        }
        out->session = f->meta.session;
        out->frame = f->meta.frame;
        out->pointer_screen = pointer_screen(f->meta.screen) ? 1 : 0;
        float sx, sy, ox, oy;
        if (!map_frame(f->meta, tw, th, &sx, &sy, &ox, &oy)) {
            out->reason = KUI_NO_VIEWPORT;
            return KILN_OK;
        }
        const int i = hit_slot(*f, x, y, tw, th);
        if (i < 0) {
            out->reason = KUI_NO_SLOT;
            return KILN_OK;
        }
        out->slot = f->slots[static_cast<size_t>(i)];
        out->ok = 1;
        return KILN_OK;
    } catch (...) {
        return KILN_ERR_INTERNAL;
    }
}

KilnResult KILN_CALL t_mapping(KilnOwner owner, float tw, float th, KilnUiSlotMapping* out) {
    if (owner == 0 || !out || out->size < sizeof(KilnUiSlotMapping)) return KILN_ERR_INVALID_ARGUMENT;
    if (!env().client_side || !env().client_side()) return KILN_ERR_UNAVAILABLE;
    if (!env().owner_alive(owner)) return KILN_ERR_REVOKED;
    const uint32_t size = out->size;
    *out = KilnUiSlotMapping{};
    out->size = size;
    try {
        if (!ensure_installed(kListSlots)) return KILN_ERR_UNAVAILABLE;
        uint32_t reason = 0;
        const auto f = usable_frame(&reason);
        if (!f) {
            out->reason = reason;
            return KILN_OK;
        }
        out->session = f->meta.session;
        out->frame = f->meta.frame;
        out->screen = f->meta.screen;
        if (!map_frame(f->meta, tw, th, &out->scale_x, &out->scale_y, &out->offset_x, &out->offset_y)) {
            out->reason = KUI_NO_VIEWPORT;
            return KILN_OK;
        }
        if (tw > 0 && th > 0 && (tw != f->meta.canvas_width || th != f->meta.canvas_height)) out->flags |= KUI_MAP_SCALED;
        out->ok = 1;
        return KILN_OK;
    } catch (...) {
        return KILN_ERR_INTERNAL;
    }
}

KilnResult KILN_CALL t_inv_unsubscribe(KilnHandle h) {
    try {
        return remove(h);
    } catch (...) {
        return KILN_ERR_INTERNAL;
    }
}

const KilnUiInventoryApi kInventoryTable = {
    {sizeof(KilnUiInventoryApi), KILN_UI_INVENTORY_VERSION},
    &t_subscribe_clicks,
    &t_slot_at,
    &t_mapping,
    &t_inv_unsubscribe,
};

}  // namespace

bool pointer_screen(const KilnUiScreen& s) {
    if (s.size == 0 || s.ui_state != KILN_UI_STATE_PLAY) return false;
    switch (s.hud_screen) {
        case 4:   // InGameMenu: the tabbed menu (inventory, its crafting and map tabs)
        case 8:   // PlayerCrafting
        case 9:   // Backpack
        case 22:  // StorageChest
        case 25:  // StationBasedCrafting
        case 27:  // NpcCrafting
        case 28:  // FactoryCrafting (the chest-style transfer screen, mode 2)
        case 41:  // the chest-style transfer screen, mode 1
            return true;
        default:  // 1 InGame: the hotbar is drawn but a click attacks; everything else unknown
            return false;
    }
}

bool map_frame(const KilnUiSlotsFrame& m, float tw, float th, float* sx, float* sy, float* ox, float* oy) {
    if (!(m.transform_flags & KUI_VIEWPORT) || !(m.canvas_width > 0) || !(m.canvas_height > 0)) return false;
    const float kx = tw > 0 ? tw / m.canvas_width : 1.0f, ky = th > 0 ? th / m.canvas_height : 1.0f;
    *sx = m.pixels_per_unit * kx;
    *sy = m.pixels_per_unit * ky;
    *ox = m.viewport_px.x * kx;
    *oy = m.viewport_px.y * ky;
    return std::isfinite(*sx) && std::isfinite(*sy) && std::isfinite(*ox) && std::isfinite(*oy) && *sx > 0 && *sy > 0;
}

int hit_slot(const SlotsSnapshot& f, float x, float y, float tw, float th) {
    float sx, sy, ox, oy;
    if (!map_frame(f.meta, tw, th, &sx, &sy, &ox, &oy)) return -1;
    for (size_t k = f.slots.size(); k-- > 0;) {
        const KilnUiVisibleSlot& s = f.slots[k];
        if (!(s.available & KUI_OUTER_RECT)) continue;
        const float x0 = ox + s.outer_ui.x * sx, y0 = oy + s.outer_ui.y * sy;
        const float x1 = x0 + s.outer_ui.width * sx, y1 = y0 + s.outer_ui.height * sy;
        if (x >= x0 && x < x1 && y >= y0 && y < y1) return static_cast<int>(k);
    }
    return -1;
}

uint32_t on_pointer(const KilnUiPointerPress* p) {
    try {
        if (!p || p->size < sizeof(KilnUiPointerPress) || !any(kListClicks)) return KILN_UI_POINTER_PASS;
        uint32_t reason = 0;
        const auto f = usable_frame(&reason);
        if (!f || !pointer_screen(f->meta.screen)) return KILN_UI_POINTER_PASS;
        const int i = hit_slot(*f, p->x, p->y, p->client_width, p->client_height);
        if (i < 0) return KILN_UI_POINTER_PASS;
        Pending pend;
        bool take = false;
        const uint32_t mods = p->mods & kModsAll;
        for (const auto& sp : *providers(kListClicks)) {
            if (!provider_live(*sp) || packed_button(sp->item) != p->button || packed_mods(sp->item) != mods) continue;
            pend.handles.push_back(sp->handle);
            take = take || (packed_flags(sp->item) & KUI_CLICK_TAKE);
        }
        if (pend.handles.empty()) return KILN_UI_POINTER_PASS;
        KilnUiSlotClick& c = pend.click;
        c.size = sizeof c;
        c.button = p->button;
        c.mods = mods;
        c.taken = take ? 1 : 0;
        c.x = p->x;
        c.y = p->y;
        c.session = f->meta.session;
        c.frame = f->meta.frame;
        c.press_ms = env().tick_ms ? env().tick_ms() : 0;
        c.screen = f->meta.screen;
        c.slot = f->slots[static_cast<size_t>(i)];
        ClickQueue& q = click_queue();
        {
            std::lock_guard lk(q.mu);
            if (q.items.size() >= kMaxPending) {
                ++q.dropped;  // nobody delivers (no frames): the press goes to the game as without Kiln
                return KILN_UI_POINTER_PASS;
            }
            q.items.push_back(std::move(pend));
        }
        return take ? KILN_UI_POINTER_TAKE : KILN_UI_POINTER_PASS;
    } catch (...) {
        return KILN_UI_POINTER_PASS;
    }
}

void clicks_frame_published() {
    std::vector<Pending> items;
    uint64_t dropped = 0;
    {
        ClickQueue& q = click_queue();
        std::lock_guard lk(q.mu);
        if (q.items.empty() && !q.dropped) return;
        items.swap(q.items);
        dropped = q.dropped;
        q.dropped = 0;
    }
    if (dropped) logf(KILN_LOG_WARN, "ui.inventory: %llu slot click(s) dropped (the queue was full)",
                      static_cast<unsigned long long>(dropped));
    if (items.empty()) return;
    const auto subs = providers(kListClicks);
    for (const Pending& pend : items)
        for (const auto& sp : *subs) {
            Provider& pr = *sp;
            if (!provider_live(pr) || std::find(pend.handles.begin(), pend.handles.end(), pr.handle) == pend.handles.end())
                continue;
            if (env().call_click && env().call_click(reinterpret_cast<KilnUiSlotClickFn>(pr.fn), &pend.click, pr.user))
                succeeded(pr);
            else
                fault(pr, "faulted");
        }
}

const KilnUiInventoryApi* inventory_table() { return &kInventoryTable; }

void clicks_reset_for_tests() {
    ClickQueue& q = click_queue();
    std::lock_guard lk(q.mu);
    q.items.clear();
    q.dropped = 0;
}

}  // namespace kiln::uivalues
