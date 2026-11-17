// SPDX-License-Identifier: MIT
// Issue #44: validated HUD/Social call-site probe and opt-in native Up/Down history.
// KILN_CHAT_INPUT_PROBE=1: call sites only (read-only).
// KILN_CHAT_HISTORY_DIAG=1: read-only key / submit diagnostics (no text logged).
// KILN_CHAT_HISTORY=1: interactive preview command recall.
// All game edit operations are scoped to the two confirmed chat call sites.
#include <windows.h>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

#include "nlohmann/json.hpp"
#include "chat_history.h"
#include "chat_focus.h"
#include "chat_keys.h"
#include "kiln_hook.hpp"
#include "ui.h"

namespace kui {
namespace {

kiln::RawHook g_input;
uintptr_t g_hud_ret = 0, g_social_ret = 0;
uintptr_t g_state_fn = 0, g_set_text_fn = 0;
std::atomic<bool> g_hud_seen{false}, g_social_seen{false};
std::atomic<bool> g_history_on{false};
std::atomic<bool> g_diagnose_on{false};
std::atomic<bool> g_trace_on{false};
std::atomic<uint32_t> g_key_log_count{0};
std::atomic<uint32_t> g_event_log_count{0};
std::atomic<uint32_t> g_recall_log_count{0};
// A literal user-edit signal, sent by WndProc. Do not infer manual edits from
// transient differences in the game's widget buffer between UI frames.
std::atomic<bool> g_manual_text_edit{false};
std::atomic<bool> g_window_blurred{false};
chat_keys::Router g_keys;
chat_history::History g_history;
chat_focus::Grace g_focus_grace;
std::filesystem::path g_history_file;
// UI-thread-only.
bool g_editing_in_frame = false;
bool g_hud_focus_logged = false, g_social_focus_logged = false;
std::string g_last_applied;
// SetText changes the editor. The caller-owned widget buffer is updated
// by the normal TextInputProcess, so verify only after that call returns.
bool g_set_text_pending = false;

constexpr uint64_t kEditHash = 0x72615e88968a8b89ull;
constexpr uintptr_t kEditOffset = 0x10;
constexpr uintptr_t kEditActiveOffset = 0x1988;
constexpr uint64_t kMaxEditBytes = 1024;

struct GameText { const char* data; uint64_t length; uint64_t hash; };
struct TextSlice { const char* data; uint64_t length; };
struct EditHead { const char* data; uint64_t length; uint64_t capacity; };
static_assert(sizeof(GameText) == 24 && sizeof(EditHead) == 24);

template <class T> bool game_read(const void* p, T* out) {
    if (!p || reinterpret_cast<uintptr_t>(p) < 0x10000) return false;
    __try {
        std::memcpy(out, p, sizeof(T));
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
bool game_copy(const char* p, char* out, size_t len) {
    if (!p || reinterpret_cast<uintptr_t>(p) < 0x10000 || len > 1024) return false;
    __try {
        std::memcpy(out, p, len);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Game accessor is called only from the scoped input hook.
using StateFn = void* (__fastcall*)(void*, uint64_t, uint32_t);
void* game_state(void* widget) {
    if (!g_state_fn || !widget) return nullptr;
    __try {
        return reinterpret_cast<StateFn>(g_state_fn)(widget, kEditHash, 1);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

// EditSetText(out, edit, &slice): use the engine to maintain cursor, selection
// and undo bookkeeping instead of writing to an arbitrary game buffer.
using SetTextFn = void (__fastcall*)(void*, void*, const TextSlice*);
bool game_set_text(void* edit, const char* data, size_t length) {
    if (!g_set_text_fn || !edit || length > 1024) return false;
    const TextSlice slice{data, length};
    alignas(16) uint8_t result[256]{};
    __try {
        reinterpret_cast<SetTextFn>(g_set_text_fn)(result, edit, &slice);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
bool edit_text(void* state, std::string* out) {
    EditHead b{};
    if (!state || !game_read(reinterpret_cast<const uint8_t*>(state) + kEditOffset, &b) ||
        b.length > kMaxEditBytes || b.length > b.capacity || b.capacity > (1ull << 20))
        return false;
    if (!b.length) { out->clear(); return true; }
    char bytes[1024];
    if (!game_copy(b.data, bytes, static_cast<size_t>(b.length))) return false;
    out->assign(bytes, static_cast<size_t>(b.length));
    return true;
}
bool editing(void* state) {
    uint8_t yes = 0;
    return state && game_read(reinterpret_cast<const uint8_t*>(state) + kEditActiveOffset, &yes) && yes != 0;
}

// The constructor receives a caller-owned chat string at widget+0x150.
// The editor's private buffer may differ from the value committed for send.
// This is a read-only cross-check: never trust the widget pointer without SEH
// and size checks; the game controls both pointers and their lifetimes.
bool widget_text(void* widget, std::string* out) {
    uintptr_t caller_buffer = 0;
    if (!widget || !game_read(reinterpret_cast<const uint8_t*>(widget) + 0x150, &caller_buffer)) return false;
    EditHead b{};
    if (!game_read(reinterpret_cast<const void*>(caller_buffer), &b) ||
        b.length > kMaxEditBytes || b.length > b.capacity || b.capacity > (1ull << 20))
        return false;
    if (b.length == 0) { out->clear(); return true; }
    char bytes[1024];
    if (!game_copy(b.data, bytes, static_cast<size_t>(b.length))) return false;
    out->assign(bytes, static_cast<size_t>(b.length));
    return true;
}

size_t controls(std::string_view line) {
    size_t n = 0;
    for (const unsigned char c : line)
        if (c < 0x20 || c == 0x7f) ++n;
    return n;
}

bool symbol(const char* name, uintptr_t* out) {
    *out = 0;
    return g_api->symbol(KilnStr{name, std::strlen(name)}, out) == KILN_OK && *out;
}
bool env_enabled(const char* name) {
    char env[8]{};
    const DWORD n = GetEnvironmentVariableA(name, env, sizeof env);
    return n > 0 && n < sizeof env && env[0] == '1' && env[1] == '\0';
}
std::filesystem::path history_path() {
    char buffer[4096]{};
    size_t length = 0;
    if (g_api->path(KILN_PATH_KILN, buffer, sizeof buffer, &length) != KILN_OK || !length || length >= sizeof buffer)
        return {};
    const std::u8string utf8(reinterpret_cast<const char8_t*>(buffer), length);
    return std::filesystem::path(utf8) / "user" / "chat-history.json";
}
void load_history() {
    g_history_file = history_path();
    if (g_history_file.empty()) return;
    try {
        std::ifstream in(g_history_file, std::ios::binary);
        if (!in.good()) return;
        const auto doc = nlohmann::json::parse(in);
        if (!doc.is_object() || !doc.contains("version") || doc["version"] != 1 ||
            !doc.contains("entries") || !doc["entries"].is_array()) return;
        std::vector<std::string> lines;
        for (const auto& e : doc["entries"]) {
            if (lines.size() >= 2000) break;
            if (e.is_object() && e.contains("text") && e["text"].is_string())
                lines.push_back(e["text"].get<std::string>());
        }
        g_history.load(lines);
    } catch (...) {
        // History is optional; file problems must never interfere with chat.
    }
}
void save_history() {
    if (g_history_file.empty()) return;
    try {
        std::filesystem::create_directories(g_history_file.parent_path());
        nlohmann::json doc = {{"version", 1}, {"entries", nlohmann::json::array()}};
        for (const auto& line : g_history.persisted())
            doc["entries"].push_back({{"text", line}, {"server", nullptr}});
        const std::filesystem::path temp = g_history_file.wstring() + L".tmp";
        {
            std::ofstream file(temp, std::ios::binary | std::ios::trunc);
            if (!file) return;
            file << doc.dump();
            file.flush();
            if (!file) return;
        }
        if (!MoveFileExW(temp.c_str(), g_history_file.c_str(),
                         MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            DeleteFileW(temp.c_str());
    } catch (...) {
        // Optional local history; never crash or block chat over a file error.
    }
}

// UI-thread-only, before the underlying TextInputProcess.
void history_before(void* widget) {
    void* state = game_state(widget);
    if (!editing(state)) return;
    std::string current;
    // The private edit bytes were proven NOT to be the visible chat string
    // (3 control bytes in the exact game build). Only use the validated
    // caller-owned widget buffer for the live draft and submitted text.
    if (!widget_text(widget, &current)) return;
    // A widget readback can legitimately differ for a frame as Enshrouded
    // reconciles the native edit state. A mismatch alone does NOT mean the
    // player edited a recalled line: clearing the cursor here lost the draft
    // before Down could restore it. Only explicit user text-edit input resets.
    if (g_manual_text_edit.exchange(false, std::memory_order_acq_rel)) {
        const bool was_browsing = g_history.browsing();
        g_history.edited();
        g_last_applied.clear();
        if (was_browsing && g_trace_on.load(std::memory_order_relaxed))
            logf(KILN_LOG_INFO, "ui: chat history trace: navigation reset by user text edit");
    }
    chat_keys::Action action{};
    while (g_keys.take(action)) {
        const bool browsing_before = g_history.browsing();
        const std::string next = action == chat_keys::Action::Older
                                     ? g_history.older(current) : g_history.newer(current);
        if (g_trace_on.load(std::memory_order_relaxed) && g_recall_log_count.fetch_add(1) < 80)
            logf(KILN_LOG_INFO,
                 "ui: chat history trace: recall %s entries=%zu changed=%u browsing_before=%u browsing_after=%u",
                 action == chat_keys::Action::Older ? "older" : "newer",
                 g_history.count(), next != current ? 1u : 0u,
                 browsing_before ? 1u : 0u, g_history.browsing() ? 1u : 0u);
        if (next == current) continue;
        if (!game_set_text(reinterpret_cast<uint8_t*>(state) + kEditOffset, next.data(), next.size())) {
            g_keys.focus(false);
            g_history_on.store(false, std::memory_order_release);
            logf(KILN_LOG_WARN, "ui: chat history: edit replace failed; preview disabled for this process");
            return;
        }
        current = next;
        g_last_applied = next;
        g_set_text_pending = true;
        // Post-call check in history_after(): the game's TextInputProcess
        // first synchronizes its private edit with the outgoing widget string.
    }
}

// The submit result is set by TextInputProcess, before the caller sends and clears.
void history_after(void* widget, void* result, bool hud) {
    void* state = game_state(widget);
    if (!state) return;
    if (g_set_text_pending) {
        g_set_text_pending = false;
        std::string applied;
        if (!widget_text(widget, &applied) || applied != g_last_applied) {
            g_keys.focus(false);
            g_history_on.store(false, std::memory_order_release);
            logf(KILN_LOG_WARN,
                 "ui: chat history: SetText did not reach the chat widget after input processing; "
                 "preview disabled for this process");
            return;
        }
        if (g_trace_on.load(std::memory_order_relaxed) && g_recall_log_count.fetch_add(1) < 24)
            logf(KILN_LOG_INFO, "ui: chat history trace: engine edit replacement verified (widget readback)");
    }
    // The input result is an event discriminant. Only the first byte is
    // needed (1 = Enter, 2 = Escape); reading a 32-bit word could include
    // unrelated/uninitialised trailing bytes and miss a valid submission.
    uint8_t event = 0;
    if (game_read(result, &event) && event != 0) {
        // The actual chat string is the caller-owned buffer, verified in
        // both HUD and Social. Do not submit the private edit's invalid
        // internal/control bytes to the history model.
        std::string sent;
        const bool readable = widget_text(widget, &sent);
        // Compare the game's caller-owned chat string with the private edit
        // state. Log *only* validity flags, lengths, controls and equivalence:
        // no user message contents, addresses, hashes or command names.
        std::string committed;
        const bool committed_readable = edit_text(state, &committed);
        if (g_trace_on.load(std::memory_order_relaxed) && g_event_log_count.fetch_add(1) < 24)
            logf(KILN_LOG_INFO,
                 "ui: chat history trace: %s event=%u widget_ok=%u widget_bytes=%zu widget_command=%u widget_valid=%u "
                 "widget_sensitive=%u widget_controls=%zu edit_ok=%u edit_bytes=%zu edit_command=%u "
                 "edit_valid=%u edit_sensitive=%u edit_controls=%zu same=%u",
                 hud ? "HUD" : "Social", static_cast<unsigned>(event),
                 readable ? 1u : 0u, readable ? sent.size() : 0,
                 readable && !sent.empty() && sent.front() == '/' ? 1u : 0u,
                 readable && chat_history::History::valid(sent) ? 1u : 0u,
                 readable && chat_history::History::sensitive(sent) ? 1u : 0u,
                 readable ? controls(sent) : 0,
                 committed_readable ? 1u : 0u, committed_readable ? committed.size() : 0,
                 committed_readable && !committed.empty() && committed.front() == '/' ? 1u : 0u,
                 committed_readable && chat_history::History::valid(committed) ? 1u : 0u,
                 committed_readable && chat_history::History::sensitive(committed) ? 1u : 0u,
                 committed_readable ? controls(committed) : 0,
                 readable && committed_readable && sent == committed ? 1u : 0u);
        if (event == 1 && readable && !g_diagnose_on.load(std::memory_order_relaxed)) {
            const size_t before = g_history.count();
            const bool persisted_changed = g_history.submit(sent);
            if (persisted_changed) save_history();
            if (g_trace_on.load(std::memory_order_relaxed) && g_event_log_count.fetch_add(1) < 24)
                logf(KILN_LOG_INFO, "ui: chat history trace: submit processed entries=%zu added=%u persisted=%u",
                     g_history.count(), g_history.count() != before ? 1u : 0u, persisted_changed ? 1u : 0u);
        }
        // Enter/escape end the active browse session even if focus briefly
        // lingers while the native widget finishes closing.
        if (event == 1 || event == 2) {
            if (event == 2) g_history.edited(); // Enter already calls Submit.
            g_manual_text_edit.store(false, std::memory_order_release);
            g_keys.focus(false);
        }
        g_last_applied.clear();
        return;
    }
    if (!editing(state)) return;
    g_editing_in_frame = true;
    bool& logged = hud ? g_hud_focus_logged : g_social_focus_logged;
    if (!logged) {
        logged = true;
        logf(KILN_LOG_INFO, "ui: chat history: %s edit focus confirmed (no text logged)", hud ? "HUD" : "Social");
    }
    // No text-difference reset here. The next history_before observes the
    // WndProc's explicit text-edit signal, not a transient UI buffer snapshot.
}

void __fastcall hk_input(void* widget, void* result, const GameText* label, uint32_t mode) {
    const uintptr_t caller = g_input.caller();
    const bool hud = caller == g_hud_ret;
    const bool social = caller == g_social_ret;
    const bool scoped = hud || social;
    if (g_history_on.load(std::memory_order_relaxed) && scoped)
        history_before(widget);
    g_input.call<void>(widget, result, label, mode);
    if (hud && !g_hud_seen.exchange(true))
        logf(KILN_LOG_INFO, "ui: chat input probe: native HUD chat call observed (call-site confirmation)");
    else if (social && !g_social_seen.exchange(true))
        logf(KILN_LOG_INFO, "ui: chat input probe: native Social chat call observed (call-site confirmation)");
    if (scoped && (g_history_on.load(std::memory_order_relaxed) ||
                   g_diagnose_on.load(std::memory_order_relaxed)))
        history_after(widget, result, hud);
}
} // namespace

// Window thread: the core WndProc already deduplicates raw and legacy key messages.
void chat_input_text_edited() {
    if (g_history_on.load(std::memory_order_acquire) && g_keys.focused())
        g_manual_text_edit.store(true, std::memory_order_release);
}

bool chat_input_on_key(uint32_t vk, uint32_t mods, bool down, bool captured) {
    if (!g_history_on.load(std::memory_order_acquire) && !g_diagnose_on.load(std::memory_order_acquire))
        return false;
    // These keys modify text without necessarily producing WM_CHAR. Modifier
    // bits follow KilnUiKey; Ctrl+V/X is paste/cut, Ctrl+A is only selection.
    if (down && !captured && g_history_on.load(std::memory_order_relaxed) && g_keys.focused() &&
        (vk == VK_BACK || vk == VK_DELETE ||
         ((mods & KILN_UI_MOD_CTRL) && (vk == 'V' || vk == 'X'))))
        g_manual_text_edit.store(true, std::memory_order_release);
    const bool kept = !captured && g_history_on.load(std::memory_order_acquire) && g_keys.key(vk, mods, down);
    if (down && (vk == VK_UP || vk == VK_DOWN) && g_trace_on.load(std::memory_order_relaxed) &&
        g_key_log_count.fetch_add(1) < 24)
        logf(KILN_LOG_INFO, "ui: chat history trace: key %s mods=%u focus=%u captured=%u consumed=%u",
             vk == VK_UP ? "Up" : "Down", mods, g_keys.focused() ? 1u : 0u, captured ? 1u : 0u, kept ? 1u : 0u);
    return kept;
}

void chat_input_window_blur() {
    g_keys.focus(false);
    g_manual_text_edit.store(false, std::memory_order_release);
    g_window_blurred.store(true, std::memory_order_release);
}

// UI thread, after the game finishes each GameUiFrame.
void chat_input_frame_end() {
    if (!g_history_on.load(std::memory_order_acquire) && !g_diagnose_on.load(std::memory_order_acquire)) return;
    const bool active = g_editing_in_frame && !g_window_blurred.exchange(false);
    // Never swallow gameplay arrows when the native chat edit lacks focus.
    g_keys.focus(active);
    // A single game UI frame may temporarily fail to render either chat
    // control during a transition. That must not erase the saved draft.
    // Exit events explicitly reset the session above; for a real close or
    // a missing editor, forget the cursor only after sustained absence.
    const bool focus_expired = g_focus_grace.frame(active);
    if (focus_expired) {
        const bool was_browsing = g_history.browsing();
        g_history.edited();
        g_last_applied.clear();
        g_manual_text_edit.store(false, std::memory_order_release);
        if (was_browsing && g_trace_on.load(std::memory_order_relaxed))
            logf(KILN_LOG_INFO, "ui: chat history trace: navigation reset after sustained loss of chat focus");
    }
    g_editing_in_frame = false;
}

bool chat_input_probe_install() {
    const bool diag = env_enabled("KILN_CHAT_HISTORY_DIAG");
    const bool history = env_enabled("KILN_CHAT_HISTORY") && !diag; // diagnostics take precedence: never swallow keys
    const bool probe = env_enabled("KILN_CHAT_INPUT_PROBE");
    if (!history && !diag && !probe) return false;
    if (!g_api->feature(KilnStr{"chat.ui.input", sizeof("chat.ui.input") - 1})) {
        logf(KILN_LOG_WARN, "ui: chat input probe requested, but chat.ui.input symbols are unavailable");
        return false;
    }
    uintptr_t input = 0;
    if (!symbol("chatui/TextInputProcess", &input) ||
        !symbol("chatui/HudInputReturn", &g_hud_ret) ||
        !symbol("chatui/SocialInputReturn", &g_social_ret) || g_hud_ret == g_social_ret) {
        logf(KILN_LOG_WARN, "ui: chat input probe requested, but scoped call sites were not resolved");
        return false;
    }
    if ((history || diag) && (!symbol("chatui/TextInputState", &g_state_fn) ||
                              (history && !symbol("chatui/EditSetText", &g_set_text_fn)))) {
        logf(KILN_LOG_WARN, "ui: chat history unavailable: edit state/replace symbols unresolved");
        return false;
    }
    if (!g_input.install(g_api, g_owner, input, reinterpret_cast<void*>(&hk_input), "chatui/TextInputProcess")) {
        logf(KILN_LOG_WARN, "ui: chat input hook unavailable: %s", g_input.error());
        return false;
    }
    if (history) {
        load_history();
        g_trace_on.store(env_enabled("KILN_CHAT_HISTORY_TRACE"), std::memory_order_release);
        g_history_on.store(true, std::memory_order_release);
        logf(KILN_LOG_INFO, "ui: chat history preview installed (Up/Down, commands persisted; opt-in; no Tab/Ctrl+R)");
    } else if (diag) {
        g_trace_on.store(true, std::memory_order_release);
        g_diagnose_on.store(true, std::memory_order_release);
        logf(KILN_LOG_INFO, "ui: chat history diagnostic installed (read-only; no keys intercepted or text logged/persisted)");
    } else {
        logf(KILN_LOG_INFO, "ui: chat input probe installed for HUD and Social (read-only; no history keys intercepted)");
    }
    return true;
}
} // namespace kui
