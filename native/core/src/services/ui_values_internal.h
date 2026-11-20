// SPDX-License-Identifier: MIT
// kiln.ui.values inside kiln-core: what its files share (abi/services/kiln_ui_values.h is the public table).
//
//   ui_values.cpp          the table, the owner registry (quotas, ordered provider lists, faults), the value chain
//   ui_values_text.cpp     the process-lifetime text store
//   ui_values_bars.cpp     the item detail bar and the slot bar (mids before the game draws a temporary Bar)
//   ui_values_update.cpp   the player-UI update: HUD pairs, the held item, the pre-provider stage (below)
//   ui_values_changes.cpp  change publication: identities, the 4096-entry cache, Added/Updated/Removed/Evicted/Reset
//   ui_values_slots.cpp    the drawn slots of each completed UI frame (copy_slots, subscribe_slots)
//   ui_values_gemforge.cpp the copied GEMS candidate list and selection context
//   ui_values_sockets.cpp  gems.sockets: the Gem Forge lists' category masks widened for sockets on other gear
//   ui_values_sources.cpp  ui.gemforge.sources: a server's offered chest gems in the GEMS grid
//   ui_values_lines.cpp    tooltip lines collected at the item detail view, the details window's snapshots
//   ui_values_tooltip.cpp  native lines drawn inside the game's item detail view (ui.tooltip.lines), stat numbers
//                          scaled in it (ui.tooltip.values), and the tooltip probe for the guided client session
//   ui_values_inventory.cpp kiln.ui.inventory: slot clicks (kiln.ui presses hit-tested on the slot frames), slot_at,
//                          the units-to-pixels mapping
//   ui_values_service.cpp  the production environment (owners, kiln.caps, sigdb, kiln.hooks, kiln.client, kiln.ui),
//                          the SEH guards and the service's registration with the core
//
// Everything but ui_values_service.cpp is plain C++ over Env below, so native/core/tests/ui_values_test.cpp builds it
// with the host compiler against fakes.
#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "services/kiln_hooks.h"
#include "services/kiln_ui_inventory.h"
#include "services/kiln_ui_values.h"

namespace kiln::uivalues {

// ---- limits (chapter 05a §5.1, §5.2) ----
inline constexpr uint32_t kValueKinds = 6;             // KUI_DETAIL .. KUI_MANA
inline constexpr uint32_t kRegistrationsPerMod = 128;  // value, line, slot and change subscriptions together
inline constexpr uint32_t kFaultLimit = 5;             // consecutive faults that remove a registration
inline constexpr size_t kTextMaxBytes = 4096;          // one string, without the NUL
inline constexpr size_t kCallbackTextBytes = 64 * 1024;
inline constexpr uint64_t kModTextBudget = 8ull << 20;
inline constexpr uint64_t kGlobalTextBudget = 64ull << 20;
inline constexpr uint32_t kGlobalStrings = 65536;
inline constexpr uint64_t kEntryOverhead = 1 + 64;  // charged per string on top of its bytes
inline constexpr uint64_t kMembershipCharge = 16;   // charged per mod holding a string
inline constexpr double kNewStringsPerSecond = 64;  // per mod
inline constexpr double kNewStringsBurst = 128;

// Registration lists: the value kinds (KilnUiValueKind), then these.
enum : uint32_t { kListLines = kValueKinds, kListSlots, kListChanges, kListGemForge, kListValues, kListClicks, kLists };

// The capability row of a list ("ui.values.hud" for the three HUD kinds).
const char* row_of(uint32_t list);

// ---- what the service takes from its host (production: ui_values_service.cpp; host tests: fakes) ----
struct ClientNow {
    uint32_t local_player = 0;  // kiln.client's local player, 0 = none
    uint64_t session = 0;       // client worlds since boot
    uint64_t step = 0;          // client-world steps since boot
};
struct Env {
    std::string (*owner_name)(KilnOwner) = nullptr;  // the stable mod id ("mod:<id>", "native:<id>")
    bool (*owner_alive)(KilnOwner) = nullptr;
    uint64_t (*registry_version)() = nullptr;  // moves when an owner is revoked
    double (*now_ms)() = nullptr;              // monotonic milliseconds (text rate)
    uint64_t (*tick_ms)() = nullptr;           // GetTickCount64 (capture times)
    bool (*client_side)() = nullptr;
    bool (*layout_supported)() = nullptr;  // private UI layout measured for this client image
    bool (*row_available)(const char* name) = nullptr;              // kiln.caps shows the row available
    void (*row_off)(const char* name, const char* text) = nullptr;  // reports the row unavailable (faulted)
    bool (*client)(ClientNow* out) = nullptr;
    bool (*screen)(KilnUiScreen* out) = nullptr;                 // kiln.ui's last completed screen
    bool (*read)(void* dst, uintptr_t src, size_t n) = nullptr;  // game memory, guarded
    bool (*write)(uintptr_t dst, const void* src, size_t n) = nullptr;
    bool (*symbol)(const char* name, uintptr_t* out) = nullptr;
    bool (*constant)(const char* name, int64_t* out) = nullptr;
    // priority: higher runs first (outer); kiln.ui's own links are at 0
    KilnResult (*hook_raw)(uintptr_t target, const char* label, int32_t priority, void* fn, void** original) = nullptr;
    KilnResult (*hook_mid)(uintptr_t target, const char* label, KilnMidFn fn) = nullptr;
    // A provider called under a structured-exception guard: false = it faulted (*rc untouched).
    bool (*call_value)(KilnUiValueFn fn, const KilnUiProviderContext* c, const KilnUiItemView* v,
                       const KilnUiBarValue* in, KilnUiBarEdit* e, void* user, KilnResult* rc) = nullptr;
    bool (*call_changed)(KilnUiValueChangedFn fn, const KilnUiValueChange* c, void* user) = nullptr;
    bool (*call_lines)(KilnUiLinesFn fn, const KilnUiProviderContext* c, const KilnUiItemView* v, KilnUiLineWriter* w,
                       void* user, KilnResult* rc) = nullptr;
    bool (*call_slots)(KilnUiSlotsFn fn, const KilnUiSlotsFrame* f, const KilnUiVisibleSlot* s, uint32_t n,
                       void* user) = nullptr;
    // fn(frame) on the UI thread after every completed game UI frame (kiln.ui's subscribe_ui_tick); false = none
    bool (*subscribe_frames)(void (*fn)(uint64_t frame, void* user), void* user) = nullptr;
    void (*log)(int level, const char* text) = nullptr;
    bool (*gemforge_abi)() = nullptr;  // sigdb's measured Win64 fingerprint
    // Kiln's immutable item catalog: 1 known (category set), 0 not in it (a mod's item while its mod is not loaded,
    // a placeholder), -1 no catalog.
    int (*item_category)(uint32_t item, uint8_t* category) = nullptr;
    bool (*call_gemforge)(KilnUiGemForgeFn, const KilnUiGemForgeFrame*, const KilnUiGemForgeCandidate*, uint32_t,
                         void* user) = nullptr;
    // ---- native tooltip lines (ui_values_tooltip.cpp) ----
    // Builds and draws one game text line as TextDraw says, under a structured-exception guard: false = it faulted.
    // *node = the line's UI node (for its layout rectangle; 0 when unknown).
    bool (*draw_text)(const struct TextDraw* d, uintptr_t* node) = nullptr;
    // The last layout rectangle of a UI node through the game's UiNodeRect (fn), guarded; false = faulted or none.
    bool (*node_rect)(uintptr_t fn, uintptr_t node, float out[4]) = nullptr;
    bool (*key_down)(uint32_t vk) = nullptr;  // the key is held now (the probe's phase key)
    uint32_t (*thread_id)() = nullptr;
    // A sigdb feature resolved on this build and not held by the client gate (usable before kiln.caps has rows).
    bool (*feature_open)(const char* name) = nullptr;
    bool (*call_scale)(KilnUiScaleFn fn, const KilnUiProviderContext* c, const KilnUiItemView* v, float* scale,
                       void* user, KilnResult* rc) = nullptr;
    // kiln.inventory (0.2.0-dev.30): kiln.ui's subscribe_pointer_press with the service's owner (false: not there, too
    // old, refused); a click subscriber called under a structured-exception guard; the cursor in the game window's
    // client pixels with the client area's size (false: no game window in front).
    bool (*subscribe_pointer)(KilnUiPointerFn fn, void* user) = nullptr;
    bool (*call_click)(KilnUiSlotClickFn fn, const KilnUiSlotClick* c, void* user) = nullptr;
    bool (*cursor_client)(float* x, float* y, float* width, float* height) = nullptr;
};
Env& env();
void logf(int level, const char* fmt, ...);

// ---- the registry ----
struct Provider {
    KilnHandle handle = 0;
    KilnOwner owner = 0;
    std::string mod;  // stable mod id at registration (quotas and text are charged to it)
    uint32_t list = 0;
    uint32_t item = 0;  // 0 = any nonempty item
    uint32_t flags = 0;  // lines: KilnUiNativeLinesFlags (tooltip_lines_native)
    void* fn = nullptr;
    void* user = nullptr;
    uint64_t seq = 0;  // accepted registration order
    std::atomic<bool> live{true};
    std::atomic<uint32_t> faults{0};   // consecutive
    std::atomic<uint32_t> refusal{0};  // the last refusal reason logged (once per transition)
};
using ProviderList = std::vector<std::shared_ptr<Provider>>;

// Adds a registration after the caller checked the arguments. KILN_OK with out->ok = 0 and KUI_REGISTRATION_BUDGET
// when the mod holds 128; KILN_ERR_REVOKED for a retired owner.
KilnResult add(KilnOwner owner, uint32_t list, uint32_t item, void* fn, void* user, KilnUiRegistration* out,
               uint32_t flags = 0);
KilnResult remove(KilnHandle h);
// The live providers of a list in registration order, as one immutable snapshot (changes made while a dispatch holds
// it apply to the next one). Never null.
std::shared_ptr<const ProviderList> providers(uint32_t list);
bool any(uint32_t list);              // lock-free: false = nothing registered (the hot path's early out)
uint64_t providers_revision(uint32_t list);  // changes even when a remove/add leaves the count equal
size_t active(std::string_view mod);  // registrations the mod holds (tests)
void owner_retired(KilnOwner owner);  // core owner-retiring notification: flags old dispatch snapshots dead
bool provider_live(const Provider& p);

// A fault of `p` (SEH, invalid edit, KILN_ERR_INTERNAL): the fifth in a row removes it.
void fault(Provider& p, const char* what);
// An accepted edit or a pass-through.
void succeeded(Provider& p);
// A refusal: logged once per reason transition, never a fault.
void refused(Provider& p, uint32_t reason);

// A kind's hooks: installed once, when the first provider of the kind registers and its row is available. Each kind's
// file registers its installer; false = the kind stays unavailable (its row is reported off).
using Installer = bool (*)(uint32_t list);
void set_installer(uint32_t list, Installer fn);
bool ensure_installed(uint32_t list);
void disable_kind(uint32_t list, const char* why);

// ---- the value chain ----
// Runs the providers of `kind` matching view.item over `game` in order and returns the accumulated value. Writes no
// game memory. A nested chain on the same thread (a provider reaching game UI code) passes through.
struct ChainStamp {
    uint64_t session = 0, frame = 0, step = 0;
};
KilnUiBarValue run_chain(uint32_t kind, const KilnUiItemView& view, const KilnUiBarValue& game,
                         const ChainStamp& stamp);
// Validates `e` against the kind's mask and the input; on success applies it to *acc. False = KUI_INVALID_EDIT.
bool apply_edit(uint32_t kind, const KilnUiBarEdit& e, const std::string& mod, KilnUiBarValue* acc);

// ---- tooltip lines and the details window's snapshots (ui_values_lines.cpp) ----
KilnResult tooltip_open(KilnOwner owner, KilnUiTooltipOpen* out);
KilnResult tooltip_close(KilnOwner owner);
KilnResult tooltip_refresh(KilnOwner owner, KilnUiTooltipOpen* out);
KilnResult tooltip_details(KilnOwner owner, uint32_t which, KilnUiTooltipDetails* out);
void lines_reset_for_tests();


// ---- visible slots (ui_values_slots.cpp) ----
KilnResult slots_copy(KilnOwner owner, uint64_t expected_session, uint64_t expected_frame, KilnUiSlotsFrame* frame,
                      KilnUiVisibleSlot* slots, uint32_t capacity, uint32_t* required);
void slots_reset_for_tests();
// The latest completed slot frame (immutable; null before the first). Any thread.
struct SlotsSnapshot {
    KilnUiSlotsFrame meta{};
    std::vector<KilnUiVisibleSlot> slots;
};
std::shared_ptr<const SlotsSnapshot> slots_latest();

// ---- kiln.ui.inventory (ui_values_inventory.cpp) ----
// Registration of any list after the caller checked its arguments (ui_values.cpp's register_in).
KilnResult register_list(KilnOwner owner, uint32_t list, uint32_t item, void* fn, void* user, KilnUiRegistration* out);
// the slots' publish, UI thread: delivers the clicks pressed since the last frame; a click of another client session
// than this frame's (the world was left in between) is dropped
void clicks_frame_published(uint64_t session);
bool pointer_screen(const KilnUiScreen& s);  // the screen has a pointer: slot clicks may hit there
// units-to-pixels for a target size; false = the frame has no usable viewport
bool map_frame(const KilnUiSlotsFrame& m, float target_w, float target_h, float* sx, float* sy, float* ox, float* oy);
// the slot under (x, y) target pixels: its index, -1 = none (the last drawn wins)
int hit_slot(const SlotsSnapshot& f, float x, float y, float target_w, float target_h);
uint32_t on_pointer(const KilnUiPointerPress* p);  // what kiln.ui's press callback does (tests call it directly)
const KilnUiInventoryApi* inventory_table();
void clicks_reset_for_tests();
KilnResult gemforge_copy(KilnOwner, uint64_t, uint64_t, KilnUiGemForgeFrame*, KilnUiGemForgeCandidate*, uint32_t,
                        uint32_t*);
void gemforge_reset_for_tests();

// ---- gems.sockets: the Gem Forge's widened category masks (ui_values_sockets.cpp) ----
KilnResult sockets_set(KilnOwner owner, uint32_t category_mask);
void sockets_owner_retired(KilnOwner owner);
uint32_t sockets_effective();  // the OR over owners without Weapons (the extra bits the hooks add); 0 = vanilla
uint32_t sockets_seen();       // every extra bit applied in this process: a list built a frame ago may still hold one
void sockets_reset_for_tests();

// ---- ui.gemforge.sources: a server's offered chest gems in the GEMS grid (ui_values_sources.cpp) ----
KilnResult sources_set(KilnOwner owner, const KilnUiGemForgeSource* rows, uint32_t count, const KilnUiGemForgePayer* payers,
                       uint32_t payer_count);
KilnResult sources_state(KilnUiGemForgeSourcesState* out);
void sources_owner_retired(KilnOwner owner);
void sources_reset_for_tests();
bool gemforge_grid_hooked();  // ui.gemforge.inspect's link is on the GEMS grid (its entry is Kiln's detour now)

// ---- change publication (ui_values_changes.cpp) ----
bool changes_wanted();  // a change subscriber exists: the chains stage their results
// The final value of one chain run under its identity (the bars and the update call it after writing).
void publish_value(uint32_t kind, const KilnUiItemView& view, const KilnUiBarValue& value);
void update_done();                   // a player-UI update captured the local player (held/HUD absence counts these)
void changes_reset(uint32_t reason);  // KilnUiResetReason: one Reset at the next frame, the cache cleared
KilnResult changes_subscribe(KilnOwner owner, KilnUiValueChangedFn fn, void* user, KilnUiRegistration* out);
void changes_reset_for_tests();

// ---- game memory and sigdb through Env ----
inline bool sig_constant(const char* name, int64_t* out) { return env().constant && env().constant(name, out); }
inline bool sig_symbol(const char* name, uintptr_t* out) { return env().symbol && env().symbol(name, out) && *out; }
template <class T>
bool game_read(uintptr_t at, T* out) {
    return at && env().read && env().read(out, at, sizeof(T));
}

// ---- the item view (ui_values_bars.cpp) ----
inline constexpr size_t kViewBytes = 0x48;  // the ItemView prefix (ui/view.stride); a detail record's first 0x48 bytes
struct ViewLayout {                         // offsets inside the prefix (sigdb ui/view.*)
    int64_t page = 0, index = 0, item = 0, count = 0, pide = 0, durability = 0, durability_max = 0, fuel = 0,
            fuel_max = 0, level = 0, rarity = 0;
};
bool load_view_layout(ViewLayout* out);  // false when a constant is missing or does not fit the prefix
// Fills identity, flags and values of `v` from a copied prefix. base_flags: KUI_DETAIL_COPY for a detail record (an
// all-zero identity then makes it KUI_SYNTHETIC), 0 for a slot's or a model's view. server_entity stays 0.
void fill_view(const uint8_t* raw, const ViewLayout& layout, uint32_t base_flags, KilnUiItemView* v);
void load_charge_layout();                  // once; the balancing offsets for units_per_charge
uint32_t units_per_charge(uintptr_t play);  // max(1, the water charge amount) through play's balancing registry

// ---- the pre-provider stage of the player-UI update (ui_values_update.cpp) ----
// For 09-ui-blocks' snapshot service (chapter 09 §4.4: local vitals and inventory), so it never edits the update hook.
// What the game's player-UI update copied for the local player, BEFORE any held or HUD provider patched it: the
// original u32 pairs and the local player's own inventory (its own models only: no open container, no detail copy),
// nonempty slots sorted by (page, index). Called on the thread of the update (the client's main thread) right after
// the update's original returned and play+localEntity was checked against kiln.client's local player, before the
// providers run. Everything is borrowed for the call and holds no game pointer: copy what you keep. Tagged with the
// client world's session and step (kiln.client) and GetTickCount64 milliseconds. Not called without a local player or
// when the update's capture was ambiguous. The observer must not fault (it is core code; no SEH guard).
struct StageRow {  // the layout of chapter 09's KilnUiInventoryRow
    uint32_t holder, inventory_page, slot_index, item, count, pide;
};
static_assert(sizeof(StageRow) == 24);
inline constexpr uint32_t kStageRowsMax = 512;  // rows carried; rows_total says when there were more
enum : uint32_t {
    kStageVitals = 1,     // health / stamina / mana and their maxima are the game's
    kStageInventory = 2,  // rows / rows_total are the local player's inventory (0 rows = nothing in it)
};
struct Stage {
    uint32_t size;
    uint32_t available;  // kStage*: the parts that are valid (inventory needs the model layout to resolve)
    uint32_t local_player;
    uint32_t health, health_max, stamina, stamina_max, mana, mana_max;
    uint32_t rows_total;  // nonempty own slots found; above kStageRowsMax only the first 512 by (page, index) are here
    uint32_t row_count;
    const StageRow* rows;
    uint64_t session, step, capture_ms;
};
using StageFn = void (*)(const Stage* stage, void* user);
// Adds an observer (any thread). It needs the update hook, which comes with ui.values.hud: KILN_ERR_UNAVAILABLE while
// that row is held by the client gate or off. Owner 0 is the core's own; another owner's observers stop when it is
// revoked. Remove with stage_unsubscribe.
KilnResult stage_subscribe(KilnOwner owner, StageFn fn, void* user, KilnHandle* out);
KilnResult stage_unsubscribe(KilnHandle h);
void stage_owner_retired(KilnOwner owner);

// ---- text (ui_values_text.cpp) ----
namespace text {
struct GameText {  // the game's string view (24 bytes): UTF-8, byte length, signed-byte FNV-1a 64
    const char* p;
    uint64_t n;
    uint64_t fnv;
};
static_assert(sizeof(GameText) == 24);
uint64_t fnv64(const char* p, size_t n);
bool valid_utf8(std::string_view s);  // also refuses NUL
// Interns `s` for `mod`: KILN_OK + *out (0 for ""); KILN_ERR_INVALID_ARGUMENT (not UTF-8, a NUL);
// KILN_ERR_BUFFER_TOO_SMALL (a length, budget or rate limit: KUI_TEXT_BUDGET).
KilnResult intern(const std::string& mod, std::string_view s, KilnUiTextId* out);
bool held_by(KilnUiTextId id, std::string_view mod);  // id 0 counts as held
bool bytes(KilnUiTextId id, std::string_view* out);   // any id of the store; process-stable bytes
bool game_text(KilnUiTextId id, GameText* out);       // for a descriptor's label: permanent bytes
struct Usage {
    uint64_t charged = 0;  // bytes charged
    uint32_t strings = 0;  // strings (store) or memberships (a mod)
};
Usage usage(std::string_view mod);
Usage usage_all();
void reset_for_tests();
}  // namespace text

// The text ops a provider's context carries, and the call it may use them in.
inline constexpr uint32_t kCallMagic = 0x55564331u;  // "UVC1"
struct CallState {
    uint32_t magic = 0;
    const Provider* provider = nullptr;
    KilnUiTextId input_label = 0;  // the accumulated input's label: resolvable and usable by this callback
    size_t bytes = 0;              // interned during this callback
    uint32_t latched = 0;          // KUI_TEXT_BUDGET after a refused intern: the callback is refused
};
const KilnUiTextOps* text_ops();
// Binds `cs` to the calling thread for the callback's duration (restores the previous one: nested calls).
struct CallScope {
    explicit CallScope(CallState* cs);
    ~CallScope();
    CallState* prev;
};

// One detail occurrence: ItemDetail's own record, copied while ItemDetail runs on this thread (ui_values_lines.cpp
// keeps the innermost on a thread-local stack; a nested detail gets its own).
inline constexpr uint32_t kNativeLines = 16;
// KilnUiLine flags of a native line (KUI_LINE_* in the public header since 0.2.0-dev.29.19)
inline constexpr uint32_t kLineColorMask = 0x0f, kLineSmall = 0x10, kLineAfterStats = 0x100, kLineFlagsKnown = 0x11f;
// KilnUiLineColor, then two probe-only sources (the stats block's buff icon tints, Q6 of the client session)
inline constexpr uint32_t kColorDefault = 0, kColorPositive = 1, kColorNegative = 2, kColorCommon = 3,
                          kColorUncommon = 4, kColorRare = 5, kColorEpic = 6, kColorLegendary = 7, kColorUnique = 8,
                          kColorMuted = 9, kColorsPublic = 10, kColorBuffPositive = 10, kColorBuffNegative = 11,
                          kColors = 12;
struct NativeLine {
    std::string_view text;  // process-lifetime bytes (the text store, or the probe's static strings)
    uint32_t flags = 0;
};
struct DetailCopy {
    uint8_t raw[kViewBytes] = {};
    bool ok = false, collected = false;
    uintptr_t record = 0;      // ItemDetail's record (valid while ItemDetail runs)
    bool native_ready = false;  // the native lines below were gathered (once per occurrence)
    uint32_t native_count = 0;
    NativeLine native[kNativeLines];
    bool drawn[kNativeLines] = {};
    uintptr_t nodes[kNativeLines] = {};
    bool scaled = false;  // the values scale below was computed (once per occurrence)
    float scale = 1.0f;
};
DetailCopy* current_detail();          // this thread's innermost detail occurrence, or null
void collect_once(DetailCopy& c);      // runs the lines providers over c once (the details window's snapshot)
// The ItemDetail link (shared by the lines, the native lines and the values): installed once; false = not possible.
bool detail_hook_install();
bool detail_wanted();                  // a copy of each detail occurrence is needed (providers or the probe)

// ---- native tooltip lines and values (ui_values_tooltip.cpp) ----
// One text line built as the game builds the detail's description: NativeTextLine(object, text, style, 1); the shown
// byte; UiNodeAlign(node, align) and the same u16 in the object; the wrap byte; UiNodeSize(node, {1, 0}); optionally
// the colour (bits |= 1, flags |= 0x40, the 8-byte palette entry, flags2 = 0) and the small size (flags |= 8, the
// size field); NativeTextDraw(object). Offsets are inside the game's 0x150-byte text object.
struct TextDraw {
    uintptr_t line_fn = 0, draw_fn = 0, align_fn = 0, size_fn = 0;
    uintptr_t style = 0;  // the style record inside the UI bundle
    const char* text = nullptr;  // the game's string view: UTF-8, byte length, signed-byte FNV-1a 64
    uint64_t text_len = 0, text_fnv = 0;
    uint16_t align = 0;
    int32_t node = 0, shown = 0, align_field = 0, wrap = 0, flags = 0, flags2 = 0, color = 0, bits = 0, size_field = 0;
    bool colored = false;
    uint64_t color_value = 0;
    bool small = false;
    float small_size = 0;
};
inline constexpr int32_t kTextObjectBytes = 0x150;
// A provider registered with KUI_LINES_IN_GAME_TOOLTIP: installs the native sites once when ui.tooltip.lines is
// available (false = the lines stay in the details window).
bool tooltip_lines_request();
KilnResult tooltip_native_state(KilnUiTooltipNativeState* out);
bool tooltip_lines_on();     // native lines are drawn now
bool tooltip_values_on();    // stat numbers are scaled now
bool tooltip_probe_on();
// Starts the tooltip probe (the guided client session's build): spec = the key name ("" = the default). Installs the
// ItemDetail link and the native sites the open features allow. Client only.
bool probe_start(KilnOwner owner, const char* spec);
uint32_t probe_key_of(const char* spec);  // the virtual-key code of a key name ("F1".."F24", "A".."Z"), 0 = invalid
void tooltip_detail_enter(DetailCopy& c);  // ItemDetail entry (the probe's DETAIL line and its key)
void tooltip_reset_for_tests();

const KilnUiValuesApi* table();
void reset_for_tests();  // registry, installers' state and text (host tests only)

}  // namespace kiln::uivalues
