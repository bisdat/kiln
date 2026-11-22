// SPDX-License-Identifier: MIT
// #120. The only game pointers are borrowed inside the existing ItemDetail TLS scope.
#include <algorithm>
#include <cmath>
#include <cstring>
#include <deque>
#include <mutex>
#include "ui_values_internal.h"

namespace kiln::uivalues {
struct ItemDetailCapture {
    KilnUiItemDetailSnapshot meta{};
    std::vector<KilnUiItemDetailLine> lines;
    size_t bytes = 0;
    bool rejected = false;
};
namespace itemdetail {
constexpr const char* row = "ui.item_detail";
std::mutex mu, install_mu;
std::deque<std::shared_ptr<const ItemDetailCapture>> cache;
std::atomic<bool> on{false};
int installed = 0;
using TextFn = void* (*)(void*, const text::GameText*, const void*, uint8_t);
TextFn next = nullptr;
thread_local bool has_value = false;
thread_local float stat_value = 0;

void* text_hook(void* out, const text::GameText* text, const void* style, uint8_t mode) {
    if (auto* c = current_detail(); c && c->capture && !c->suppress_capture) {
        text::GameText t{};
        if (!env().read(&t, reinterpret_cast<uintptr_t>(text), sizeof t) || t.n > KILN_UI_ITEM_DETAIL_TEXT_BYTES) {
            c->capture->rejected = true;
        } else if (t.n) {
            char bytes[KILN_UI_ITEM_DETAIL_TEXT_BYTES];
            if (env().read(bytes, reinterpret_cast<uintptr_t>(t.p), static_cast<size_t>(t.n)))
                item_detail_text(*c, bytes, static_cast<size_t>(t.n), has_value, stat_value);
            else c->capture->rejected = true;
            has_value = false; // metadata on the first text row of a stat draw; remaining rows retain display order
        }
    }
    return next(out, text, style, mode);
}

bool install() {
    std::lock_guard lk(install_mu);
    if (installed) return installed > 0;
    uintptr_t at = 0;
    const bool ready = env().layout_supported && env().layout_supported() &&
        sig_symbol("ui/NativeTextLine", &at) && detail_hook_install() && item_detail_stats_install();
    if (!ready || env().hook_raw(at, "ui.item_detail/NativeTextLine", 0, reinterpret_cast<void*>(&text_hook),
                                reinterpret_cast<void**>(&next)) != KILN_OK || !next) {
        installed = -1;
        if (env().row_off) env().row_off(row, "item detail capture hooks could not be installed");
        return false;
    }
    installed = 1;
    on.store(true, std::memory_order_release);
    return true;
}
}

bool item_detail_on() { return itemdetail::on.load(std::memory_order_acquire); }

void item_detail_begin(DetailCopy& c) {
    if (!item_detail_on() || !c.ok) return;
    ClientNow now{};
    if (!env().client || !env().client(&now) || !now.session) return;
    ViewLayout layout;
    if (!load_view_layout(&layout)) return;
    KilnUiItemView v{}; v.size = sizeof v;
    fill_view(c.raw, layout, KUI_DETAIL_COPY, &v);
    if (!v.item) return;
    c.capture = std::make_shared<ItemDetailCapture>();
    auto& m = c.capture->meta;
    m.size = sizeof m; m.item = v.item; m.entity = v.client_entity; m.level = v.level; m.rarity = v.rarity;
    m.session = now.session; m.capture_ms = env().tick_ms ? env().tick_ms() : 0;
    KilnUiScreen screen{};
    if (env().screen && env().screen(&screen)) m.frame = screen.frame + 1;
}

void item_detail_text(DetailCopy& c, const char* text, size_t bytes, bool value, float number) {
    if (!c.capture || c.suppress_capture || c.capture->rejected) return;
    auto& s = *c.capture;
    if (!text::valid_utf8(std::string_view(text, bytes)) || bytes > KILN_UI_ITEM_DETAIL_TEXT_BYTES || s.bytes + bytes > 32768 ||
        s.lines.size() >= KILN_UI_ITEM_DETAIL_MAX_LINES || (value && !std::isfinite(number))) {
        s.rejected = true; return;
    }
    KilnUiItemDetailLine l{}; l.size = sizeof l; l.text_bytes = static_cast<uint32_t>(bytes);
    if (bytes) std::memcpy(l.text, text, bytes);
    if (value) { l.flags = KILN_UI_ITEM_DETAIL_VALUE; l.value = number; }
    s.bytes += bytes; s.lines.push_back(l);
}

// The stat label is opaque. Capture its *constructed text*, never reinterpret it as a GameText.
void item_detail_stat(DetailCopy& c, float value, void (*draw)(const void*, float), const void* label) {
    const bool old_has = itemdetail::has_value;
    const float old_value = itemdetail::stat_value;
    itemdetail::has_value = true; itemdetail::stat_value = value;
    const auto before = c.capture ? c.capture->lines.size() : 0;
    draw(label, value);
    if (c.capture && c.capture->lines.size() == before) item_detail_text(c, "", 0, true, value);
    itemdetail::has_value = old_has; itemdetail::stat_value = old_value;
}

void item_detail_end(DetailCopy& c) {
    if (!c.capture) return;
    ClientNow now{};
    if (!env().client || !env().client(&now) || now.session != c.capture->meta.session) return;
    std::lock_guard lk(itemdetail::mu);
    auto& cache = itemdetail::cache;
    const auto& m = c.capture->meta;
    std::erase_if(cache, [&](const auto& s) {
        return s->meta.session != m.session || (m.entity ? s->meta.entity == m.entity :
            (!s->meta.entity && s->meta.item == m.item && s->meta.level == m.level && s->meta.rarity == m.rarity));
    });
    if (c.capture->rejected || c.capture->lines.empty()) return;
    c.capture->meta.ok = 1; c.capture->meta.count = static_cast<uint32_t>(c.capture->lines.size());
    cache.push_front(std::move(c.capture));
    if (cache.size() > 64) cache.pop_back();
}

KilnResult KILN_CALL item_detail_copy(KilnOwner owner, const KilnUiItemDetailQuery* q,
    KilnUiItemDetailSnapshot* out, KilnUiItemDetailLine* lines, uint32_t capacity) {
    if (!owner || !q || q->size < sizeof *q || (!q->entity && !q->item) || !out || out->size < sizeof *out ||
        (capacity && !lines)) return KILN_ERR_INVALID_ARGUMENT;
    *out = {}; out->size = sizeof *out;
    if (!env().client_side || !env().client_side() || !env().row_available || !env().row_available(itemdetail::row))
        return KILN_ERR_UNAVAILABLE;
    if (!env().owner_alive(owner)) return KILN_ERR_REVOKED;
    try {
        if (!itemdetail::install()) return KILN_ERR_UNAVAILABLE;
        ClientNow now{};
        if (!env().client || !env().client(&now)) return KILN_OK;
        std::lock_guard lk(itemdetail::mu);
        std::erase_if(itemdetail::cache, [&](const auto& s) { return s->meta.session != now.session; });
        for (const auto& s : itemdetail::cache) {
            const auto& m = s->meta;
            if ((q->entity && q->entity != m.entity) || (q->item && q->item != m.item) ||
                (q->level != UINT32_MAX && q->level != m.level) || (q->rarity != UINT32_MAX && q->rarity != m.rarity)) continue;
            *out = m;
            if (capacity < m.count) { out->ok = 0; return KILN_ERR_BUFFER_TOO_SMALL; }
            std::copy(s->lines.begin(), s->lines.end(), lines);
            return KILN_OK;
        }
        return KILN_OK;
    } catch (...) { return KILN_ERR_INTERNAL; }
}

const KilnUiItemDetailApi* item_detail_table() {
    static const KilnUiItemDetailApi t{{sizeof(KilnUiItemDetailApi), KILN_UI_ITEM_DETAIL_VERSION}, &item_detail_copy};
    return &t;
}
void item_detail_reset_for_tests() {
    std::lock_guard lk(itemdetail::mu);
    itemdetail::cache.clear(); itemdetail::installed = 0; itemdetail::on = false; itemdetail::next = nullptr;
}
} // namespace kiln::uivalues
