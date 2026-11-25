// SPDX-License-Identifier: MIT
// kiln.ui.input (kiln_ui_input.h; since 0.2.0-dev.31.13, mod API 0.2.22): input a subscriber owns from its press to its
// release, for XHL's Blueprint (a right-button gesture during construction) and overlays that close on a controller
// button. The rules are input_own.h's; this file holds the subscribers and their callbacks, and the table. Callers:
// input.cpp (on_key, the pointer's presses and releases, the raw records' flags, the MK_ bits, focus loss, the pads),
// keys.cpp (a new press is offered to the owners before the key subscribers), game.cpp (the world poll, UI thread).
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "input_own.h"
#include "services/kiln_client.h"
#include "services/kiln_ui_input.h"
#include "ui.h"

namespace kui {
namespace {

static_assert(own::kKeys == KILN_UI_OWN_KEYS && own::kMouse == KILN_UI_OWN_MOUSE && own::kPointer == KILN_UI_OWN_POINTER &&
                  own::kPad == KILN_UI_OWN_PAD && own::kRepeat == KILN_UI_OWNED_REPEAT && own::kEnd == KILN_UI_OWNED_END,
              "input_own.h's numbers are kiln_ui_input.h's");
static_assert(sizeof(KilnUiOwnedInput) == 64 && offsetof(KilnUiOwnedInput, x) == 40 && sizeof(KilnUiPadState) == 88 &&
                  offsetof(KilnUiPadState, polls) == 72 && sizeof(KilnUiInputApi) == 48,
              "kiln_ui_input.h layout (UiNative.OwnedInput, UiNative.PadState, UiNative.InputTable)");

struct OwnSub : CallbackLifetime {
    KilnHandle handle = 0;
    KilnOwner owner = 0;
    uint32_t sources = 0;
    KilnUiOwnedFn fn = nullptr;
    void* user = nullptr;
};
std::mutex g_mu;
std::vector<std::shared_ptr<OwnSub>> g_subs;
std::unordered_map<KilnHandle, std::shared_ptr<OwnSub>> g_retiring;  // unsubscribed, a call may still be in flight
std::atomic<uint32_t> g_count[4] = {};                               // subscribers per source bit (any thread reads)
own::Table g_table;
std::atomic<uint32_t> g_world{0};  // the generation the last world poll saw
bool g_fault_said = false;
// The press being offered on this thread (own_offer), so that a disown from inside its own call gives it up as soon as
// it is taken (an overlay that closes on the press).
struct Offering {
    KilnHandle sub = 0;
    uint32_t id = 0;
    bool disowned = false;
};
thread_local Offering t_offering;

int bit_index(uint32_t source) { return source == own::kKeys ? 0 : source == own::kMouse ? 1 : source == own::kPointer ? 2 : 3; }

void recount_locked() {
    for (int i = 0; i < 4; ++i) {
        const uint32_t bit = 1u << i;
        g_count[i].store(static_cast<uint32_t>(std::count_if(g_subs.begin(), g_subs.end(),
                                                             [&](const auto& s) { return (s->sources & bit) != 0; })),
                         std::memory_order_release);
    }
}

// The client world's generation (kiln.client state: it changes whenever a world starts or stops); 0 without kiln.client.
// kiln.client may register after kiln.ui first asks: until it is found it is asked again, at most once a second.
uint32_t generation() {
    static std::atomic<const KilnClientApi*> client{nullptr};
    static std::atomic<uint64_t> next_try{0};  // now_ms() of the next lookup allowed
    const KilnClientApi* c = client.load(std::memory_order_acquire);
    if (!c && g_api) {
        const uint64_t now = now_ms();
        uint64_t due = next_try.load(std::memory_order_relaxed);
        if (now >= due && next_try.compare_exchange_strong(due, now + 1000, std::memory_order_acq_rel)) {
            c = static_cast<const KilnClientApi*>(g_api->service(KilnStr{KILN_CLIENT_SERVICE, sizeof(KILN_CLIENT_SERVICE) - 1}, 1));
            if (c) client.store(c, std::memory_order_release);
        }
    }
    if (!c || !c->state) return 0;
    KilnClientState s{};
    s.size = sizeof s;
    return c->state(&s) == KILN_OK ? s.generation : 0;
}

bool guarded(KilnUiOwnedFn fn, const KilnUiOwnedInput* in, void* user, uint32_t* answer) {
    __try {
        *answer = fn(in, user);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

std::vector<std::shared_ptr<OwnSub>> snapshot() {
    std::lock_guard lk(g_mu);
    return g_subs;
}

std::shared_ptr<OwnSub> find(KilnHandle h) {
    std::lock_guard lk(g_mu);
    for (const auto& s : g_subs)
        if (s->handle == h) return s;
    return nullptr;
}

// One callback. False: the subscriber is off or faulted (removed; its presses nobody's).
bool call(const std::shared_ptr<OwnSub>& s, const KilnUiOwnedInput& in, uint32_t* answer) {
    std::lock_guard gate(s->gate);
    if (!s->active.load(std::memory_order_acquire)) return false;
    CallbackLifetime::Invocation invocation(s.get());
    auto* outer = t_callback;
    t_callback = s.get();
    uint32_t a = KILN_UI_OWN_PASS;
    const bool ok = guarded(s->fn, &in, s->user, &a);
    t_callback = outer;
    if (ok) {
        *answer = a;
        return true;
    }
    s->active.store(false, std::memory_order_release);
    set_error("an owned-input callback faulted; removed");
    if (!g_fault_said) {
        g_fault_said = true;
        logf(KILN_LOG_ERROR, "ui: a kiln.ui.input subscribe_owned callback faulted; it was removed and its presses are nobody's");
    }
    {
        std::lock_guard lk(g_mu);
        std::erase_if(g_subs, [&](const auto& x) { return x == s; });
        recount_locked();
    }
    g_table.disown(s->handle);
    return false;
}

KilnUiOwnedInput record(const OwnInput& e, uint32_t kind) {
    KilnUiOwnedInput in{};
    in.size = sizeof in;
    in.kind = kind;
    in.source = own::source_of(e.vk);
    in.vk = e.vk;
    in.scan = e.scan;
    in.mods = e.mods;
    in.controller = e.controller;
    in.time = e.time;
    in.x = e.x;
    in.y = e.y;
    in.client_width = e.client_width;
    in.client_height = e.client_height;
    in.text_input = e.text_input ? 1 : 0;
    in.captured = e.captured ? 1 : 0;
    return in;
}

// The notes of one table operation, in order. `e`: the transition they come from (null: an End of its own, which
// carries only the code); `leaked`: the game saw this press go down (KILN_UI_OWNED_LEAKED on a repeat or a release).
void deliver(const std::vector<own::Note>& notes, const OwnInput* e, bool leaked) {
    for (const auto& n : notes) {
        const auto s = find(n.sub);
        if (!s) continue;
        OwnInput base{};
        if (e && n.kind != own::kEnd) base = *e;
        base.vk = own::vk_of(n.id);
        base.controller = own::controller_of(n.id);
        KilnUiOwnedInput in = record(base, n.kind);
        in.end = n.end;
        if (n.kind == own::kEnd) in.time = 0;
        if (leaked && n.kind != own::kEnd) in.flags |= KILN_UI_OWNED_LEAKED;
        uint32_t ignored = 0;
        call(s, in, &ignored);
    }
}

}  // namespace

bool own_wanted(uint32_t source) {
    if (!g_table.empty()) return true;  // a claim (an orphan too) still routes its release
    for (int i = 0; i < 4; ++i)
        if ((source & (1u << i)) && g_count[i].load(std::memory_order_acquire)) return true;
    return false;
}

bool own_claimed() { return !g_table.empty(); }

bool own_route(const OwnInput& e, bool down, bool repeat, bool* keep) {
    if (g_table.empty()) return false;
    std::vector<own::Note> notes;
    const own::Decision d = g_table.transition(own::id_of(e.vk, e.controller), down, repeat, generation(), &notes);
    deliver(notes, &e, d.handled && !d.keep);
    if (!d.handled) return false;
    if (keep) *keep = d.keep;
    return true;
}

bool own_offer(const OwnInput& e) {
    const uint32_t source = own::source_of(e.vk);
    if (!source || !g_count[bit_index(source)].load(std::memory_order_acquire)) return false;
    struct Entry {
        KilnHandle handle;
        uint32_t sources;
        std::shared_ptr<OwnSub> sub;
    };
    std::vector<Entry> subs;
    for (auto& s : snapshot()) subs.push_back({s->handle, s->sources, s});
    const KilnUiOwnedInput in = record(e, KILN_UI_OWNED_PRESS);
    const uint32_t id = own::id_of(e.vk, e.controller);
    const Offering outer = t_offering;
    const KilnHandle owner = own::offer(g_table, id, generation(), subs, [&](Entry& s) {
        t_offering = {s.handle, id, false};
        uint32_t answer = KILN_UI_OWN_PASS;
        return call(s.sub, in, &answer) && answer == KILN_UI_OWN_TAKE;
    });
    const bool disowned = owner && t_offering.sub == owner && t_offering.disowned;
    t_offering = outer;
    if (disowned) g_table.disown(owner, id);  // it gave the press up in its own call: taken, and nobody's
    return owner != 0;
}

void own_leak(uint32_t vk, uint32_t controller) {
    if (!g_table.empty()) g_table.leak(own::id_of(vk, controller));
}

bool own_hidden(uint32_t vk, uint32_t controller) { return g_table.hidden(own::id_of(vk, controller)); }

uint32_t own_hidden_mk() { return own::hidden_mk(g_table); }

uint32_t own_pad_owned(uint32_t controller) { return g_table.pad_owned(controller); }

void own_focus_lost() {
    if (g_table.empty()) return;
    std::vector<own::Note> notes;
    g_table.end_all(own::kEndFocus, &notes);
    deliver(notes, nullptr, false);
}

void own_world_poll() {
    if (g_table.empty()) return;
    const uint32_t gen = generation();
    if (gen == g_world.exchange(gen, std::memory_order_acq_rel)) return;
    std::vector<own::Note> notes;
    g_table.end_world(gen, &notes);
    deliver(notes, nullptr, false);
}

KilnResult own_subscribe(KilnOwner owner, uint32_t sources, KilnUiOwnedFn fn, void* user, KilnHandle* out) {
    if (!fn || !sources || (sources & ~own::kSources)) return KILN_ERR_INVALID_ARGUMENT;
    auto sub = std::make_shared<OwnSub>();
    sub->owner = owner;
    sub->sources = sources;
    sub->fn = fn;
    sub->user = user;
    std::lock_guard lk(g_mu);
    sub->handle = next_handle();
    if (out) *out = sub->handle;
    g_subs.push_back(std::move(sub));
    recount_locked();
    return KILN_OK;
}

KilnResult own_unsubscribe(KilnHandle h) {
    std::shared_ptr<OwnSub> removed;
    {
        std::lock_guard lk(g_mu);
        const auto it = std::find_if(g_subs.begin(), g_subs.end(), [&](const auto& s) { return s->handle == h; });
        if (it != g_subs.end()) {
            removed = *it;
            removed->active.store(false, std::memory_order_release);
            g_retiring[h] = removed;
            g_subs.erase(it);
            recount_locked();
        } else if (const auto r = g_retiring.find(h); r != g_retiring.end()) {
            removed = r->second;
        }
    }
    if (!removed) return KILN_ERR_NOT_FOUND;
    g_table.disown(h);  // its presses are nobody's: their releases stay kept from the game
    // A call in flight ends first (never waited for under g_mu: the callback may itself unsubscribe). Inside another
    // kiln.ui callback this only tries, so two callbacks cancelling each other cannot deadlock their threads.
    auto drain = removed->drain(t_callback != nullptr);
    if (!drain.owns_lock()) return KILN_ERR_WRONG_THREAD;
    std::lock_guard lk(g_mu);
    g_retiring.erase(h);
    return KILN_OK;
}

uint32_t own_disown(KilnHandle h, uint32_t vk, uint32_t controller) {
    if (!h) return 0;
    const uint32_t id = vk ? own::id_of(vk, controller) : 0;
    uint32_t n = static_cast<uint32_t>(g_table.disown(h, id));
    // inside the call that offers a press to `h`: that press too, once it is taken
    if (t_offering.sub == h && !t_offering.disowned && (id == 0 || id == t_offering.id)) {
        t_offering.disowned = true;
        ++n;
    }
    return n;
}

bool own_owns(KilnHandle h, uint32_t vk, uint32_t controller) { return g_table.owns(h, own::id_of(vk, controller)); }

namespace {
KilnResult KILN_CALL t_subscribe_owned(KilnOwner o, uint32_t sources, KilnUiOwnedFn fn, void* u, KilnHandle* out) {
    return own_subscribe(o, sources, fn, u, out);
}
KilnResult KILN_CALL t_unsubscribe(KilnHandle h) { return own_unsubscribe(h); }
uint32_t KILN_CALL t_disown(KilnHandle h, uint32_t vk, uint32_t controller) { return own_disown(h, vk, controller); }
bool KILN_CALL t_owns(KilnHandle h, uint32_t vk, uint32_t controller) { return own_owns(h, vk, controller); }
KilnResult KILN_CALL t_pad_state(uint32_t controller, KilnUiPadState* out) { return input_pad_state(controller, out); }

const KilnUiInputApi kInputTable = {
    {sizeof(KilnUiInputApi), KILN_UI_INPUT_VERSION}, &t_subscribe_owned, &t_unsubscribe, &t_disown, &t_owns, &t_pad_state,
};
}  // namespace

const KilnUiInputApi* input_table() { return &kInputTable; }

}  // namespace kui
