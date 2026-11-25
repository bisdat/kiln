// SPDX-License-Identifier: MIT
// kiln-emf: loads unmodified EMF (Enshrouded Modding Framework 0.6.22, API v49, MIT) mods on Kiln. Shared state.
//
// Layout of the plugin:
//   plugin.cpp    kiln_plugin_main / on_ready: step aside when the real EMF runs, discover, order and load the mods,
//                 world ticks (where EMF's "after the step" callbacks run), status file
//   mods.cpp      mod.json, the legacy folder and kiln.profile, version ranges, dependency order, the load itself
//   settings.cpp  EMF mod settings: <configDir>/emf/<id>.ini (+ mirror to <game>/emf/config/), schema snapshots
//   api_core.cpp  the EmfApi table: log, worlds, components, entities, ticks, build, mod info, stubs
//   api_game.cpp  items, destroy, chat, saves, combat, teleport, world settings, HUD/notify
//   api_spawn.cpp spawn, spawn_ex, spawn_facing through kiln.spawn (creation-time yaw and components)
//   api_terrain.cpp v44/v45/v47 block queries and queued sphere/column breaks through kiln.build
//   api_events.cpp on_event and mod programs through kiln.world
//   api_hooks.cpp resolve / hook / patterns / write_memory, the EMF signature -> sigdb alias table
//   commands.cpp  EMF commands from players' chat (kiln.chat) and <game>/emf/commands.txt
//   api_attributes.cpp v18 attribute_get / attribute_add on kiln.combat v2 (health, stamina, mana, body heat, oxygen)
//   api_level.cpp v22: a player's XP and level through kiln.progression
//   api_components.cpp v23 change_component (a kiln.world program) and v25 E prompts (offers)
//   api_windows.cpp v24 mod windows and v26 pictures through kiln.ui (client); refused on a dedicated server
//   api_wheel.cpp v10 radial wheel through kiln.ui's wheel (client); refused on a dedicated server
//   api_v40.cpp   v28-v49: world_to_screen through kiln.render; the functions Kiln does not carry yet answer their
//                 documented failure (chat_tell_ex / chat_broadcast_ex are in api_game.cpp)
//   api_orders.cpp v39 enemy orders and v40 move orders on kiln.creature_control leases, kiln.creatures and
//                 kiln.spawn (the logic without the game: orders_model.*)
//   api_markers.cpp v33 markers and v47 marker bars through kiln.markers (since 0.2.0-dev.31.13)
//   api_hits.cpp  v31 on_hit: the server world's HitEvents through kiln-emf's event relay (hits_model.* without the game)
//   api_buffs.cpp v32 buffs: kiln.playerstate reads, the BuffRegistry's types, kiln.abilities programs that run the
//                 game's applyBuff / removeBuff, kiln.content texts (buffs_model.* without the game)
//   api_view.cpp  v35 player_view through kiln.client and kiln.render; fog_at, map_markers, canvases answer "not yet"
//   v26_model.*   the v22-v26 logic without the game (host tests: tests/run.sh)
//   seh.cpp       structured-exception guards around every call into a mod
// Docs: docs/kiln/emf.md (users), docs/re/emf-compat.md (how each function maps onto Kiln).
#pragma once
#include <windows.h>

#include <atomic>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "emf/emf_api.h"
#include "kiln_api.h"
#include "services/kiln_build.h"
#include "services/kiln_caps.h"
#include "services/kiln_chat.h"
#include "services/kiln_client.h"
#include "services/kiln_combat.h"
#include "services/kiln_content.h"
#include "services/kiln_hooks.h"
#include "services/kiln_items.h"
#include "services/kiln_players.h"
#include "services/kiln_profile.h"
#include "services/kiln_saves.h"
#include "services/kiln_settings.h"
#include "services/kiln_spawn.h"
#include "services/kiln_ui.h"
#include "services/kiln_world.h"
#include "kiln_progression.h"

namespace kemf {

constexpr const char* kPluginId = "kiln-emf";
constexpr const char* kPluginVersion = "0.1.0";
constexpr uint32_t kEmfApiVersion = EMF_API_VERSION;  // 49

// ---------------------------------------------------------------------------------------------- core handles ----
extern const KilnApi* g_api;
extern KilnOwner g_owner;
extern uint32_t g_side;  // KILN_SIDE_*

struct Services {
    const KilnBuildApi* build = nullptr;
    const KilnCapsApi* caps = nullptr;
    const KilnWorldApi* world = nullptr;
    const KilnClientApi* client = nullptr;
    const KilnHooksApi* hooks = nullptr;
    const KilnItemsApi* items = nullptr;
    const KilnContentApi* content = nullptr;
    const KilnSpawnApi* spawn = nullptr;
    const KilnChatApi* chat = nullptr;
    const KilnPlayersApi* players = nullptr;
    const KilnSavesApi* saves = nullptr;
    const KilnSettingsApi* settings = nullptr;
    const KilnCombatApi* combat = nullptr;
    const KilnUiApi* ui = nullptr;
    const KilnProfileApi* profile = nullptr;
    const KilnProgressionApi* progression = nullptr;
};
extern Services S;

struct Paths {
    std::filesystem::path game, kiln, config, state, logs, emf;  // emf = <game>/emf
    bool state_is_logs = false;                                   // core without KILN_PATH_STATE
};
extern Paths P;

// ---------------------------------------------------------------------------------------------- mods ------------
struct Dependency {
    std::string id;
    std::string range;  // "" = any
    bool optional = false;
};

struct Mod {
    std::string id, name, version, description, side;  // side: "server" | "client" | "both" | "" (unknown)
    std::vector<std::string> authors;
    std::filesystem::path folder;  // mod folder (folder mods) or emf/mods (loose DLLs)
    std::filesystem::path dll;     // empty for script mods
    std::string main;              // script mods: entry file ("main.lua")
    std::string source;            // EmfModInfo.source: "clock\" or "old_mod.dll"
    std::string origin;            // "legacy" | "profile"
    std::string profile_id;        // origin "profile": the entry's id (Kiln's Library id; the manifest id may differ)
    std::string state;             // state-file word when kiln-emf stands aside ("own-loader", "not-loaded"), else ""
    uint32_t emf_min = 1;
    bool loose = false;
    bool server_ticks_only = false;  // compat_ticks.h: on_tick for the server world only (a known mod bug)
    bool enabled_default = true;
    bool enabled = true;
    std::vector<Dependency> deps;
    std::string settings_json;  // the manifest's "settings" array (JSON text), "" when none
    uint32_t status = EMF_MOD_PENDING;
    std::string reason;  // why it failed / was rejected, in words
    HMODULE module = nullptr;
    KilnOwner owner = 0;  // "emf:<id>": log lines, content layers
    double load_ms = 0;
    std::atomic<uint32_t> faults{0};
    std::atomic<uint64_t> tick_ns{0};      // time in this mod's after-step callbacks (on_tick, run_on_world, events)
    std::atomic<uint64_t> tick_calls{0};
    std::atomic<uint64_t> tick_max_ns{0};  // the slowest single callback
    // What the mod asked kiln-emf for inside its callbacks (Context.api, added when the callback's scope ends; 30.7):
    std::atomic<uint64_t> api_reads{0};          // get_component / get_template_component
    std::atomic<uint64_t> api_scans{0};          // for_each_entity
    std::atomic<uint64_t> api_scan_entities{0};  // the ids those gave
    std::atomic<uint64_t> api_scan_ns{0};        // kiln-emf's time reading the entity map for them
};
// Adds one callback's time to its mod (plugin.cpp writes the totals into state/emf.json).
void account(Mod* m, uint64_t ns);
uint64_t qpc_ns();
extern std::vector<std::unique_ptr<Mod>> g_mods;  // stable pointers, never freed
extern std::mutex g_mods_mutex;

Mod* find_mod(std::string_view id);
Mod* mod_of_address(const void* address);  // the mod whose DLL contains `address`
Mod* caller_mod(const void* return_address);  // mod_of_address, else the mod running emf_mod_load / a callback
const char* mod_name(const Mod* m);         // "?" for none

// discover + order + load (mods.cpp). Returns the number of mods that loaded.
void discover_mods();
// While kiln-emf stands aside for the real EMF: lists the profile's EMF entries with what happens to each.
void aside_mods();
void load_mods(const EmfApi* table);
bool valid_mod_id(std::string_view id);

// ---------------------------------------------------------------------------------------------- calling context -
// What the current thread is doing on a mod's behalf. Set while kiln-emf runs a mod callback (or emf_mod_load);
// read by API functions that EMF allows only inside world callbacks (spawn, destroy, give_item) and by the command
// functions (command_reply, command_sender).
struct CommandRun;
// Plain per-thread counters: get_component runs tens of thousands of times a step for some mods, so it counts here and
// the mod's atomics get the sum once, when the callback's ContextScope ends.
struct ApiCounts {
    uint64_t reads = 0, scans = 0, scan_entities = 0, scan_ns = 0;
};
struct Context {
    Mod* mod = nullptr;
    EmfWorld* world = nullptr;  // the world the callback was given (null: emf_mod_load, a program, another thread)
    bool world_cb = false;      // inside on_tick / run_on_world / on_event / a command / a result callback
    std::shared_ptr<CommandRun> cmd;
    ApiCounts api;  // this scope's calls (an inner scope's calls go to its own mod)
};
extern thread_local Context t_ctx;

struct ContextScope {
    Context saved;
    ContextScope(Mod* m, EmfWorld* w, bool world_cb, std::shared_ptr<CommandRun> cmd = nullptr) : saved(t_ctx) {
        t_ctx.mod = m;
        t_ctx.world = w;
        t_ctx.world_cb = world_cb;
        t_ctx.cmd = std::move(cmd);
        t_ctx.api = {};
    }
    ~ContextScope() {
        if (Mod* m = t_ctx.mod) {
            const ApiCounts& a = t_ctx.api;
            if (a.reads) m->api_reads.fetch_add(a.reads, std::memory_order_relaxed);
            if (a.scans) {
                m->api_scans.fetch_add(a.scans, std::memory_order_relaxed);
                m->api_scan_entities.fetch_add(a.scan_entities, std::memory_order_relaxed);
                m->api_scan_ns.fetch_add(a.scan_ns, std::memory_order_relaxed);
            }
        }
        t_ctx = saved;
    }
};

// ---------------------------------------------------------------------------------------------- logging ---------
void logf(int level, const char* fmt, ...);                       // kiln-emf's own lines
void mod_log(const Mod* m, int level, std::string_view text);     // a line under the mod's owner
void mod_logf(const Mod* m, int level, const char* fmt, ...);
// "emf: <mod> called <fn>, not supported" once per mod and function; records it for the status file.
void unsupported(const Mod* m, const char* fn, const char* why = nullptr);
// A refusal line (EMF logs every refusal); `once` = at most once per mod+key.
void refuse(const Mod* m, const char* fn, const char* why, bool once = false);
void note_call(const char* fn);  // counts calls per function for the status file

// ---------------------------------------------------------------------------------------------- SEH -------------
// Runs fn(ctx) under a structured-exception guard. False (and the exception code) when it faulted; C++ exceptions
// that escape a mod (0xE06D7363) count as faults.
using ThunkFn = void (*)(void*);
bool seh_call(ThunkFn fn, void* ctx, unsigned long* code, uintptr_t* address);
bool seh_copy(void* dst, const void* src, size_t n);
bool seh_read_u32(uintptr_t a, uint32_t* out);
bool seh_read_u64(uintptr_t a, uint64_t* out);
bool seh_read_u16(uintptr_t a, uint16_t* out);
bool seh_read_u8(uintptr_t a, uint8_t* out);
// Calls a mod callback with the context set; on a fault logs it under the mod and returns false (the caller then
// disables the callback).
bool guarded(Mod* m, const char* what, ThunkFn fn, void* ctx);

// ---------------------------------------------------------------------------------------------- worlds ----------
uint8_t world_kind_of(EmfWorld* w);  // EMF_WORLD_* (0xFF unknown)
EmfWorld* world_of_kind(uint32_t kind);
bool on_thread_of(EmfWorld* w);
uint32_t server_session();
const void* component_of(EmfWorld* w, uint32_t hash, uint32_t entity, uint64_t* size);
void* template_component_of(EmfWorld* w, uint32_t hash, uint32_t entity, uint64_t* size);
uint32_t type_size(uint32_t qualified_hash);  // the game's reflected size of a keen type, 0 unknown
uint32_t fnv1a(std::string_view s);

// ---------------------------------------------------------------------------------------------- ticks -----------
// Runs after each step of a world of `kind`, on its thread (plugin.cpp calls these from Kiln's Post / Pre).
void dispatch_after_step(EmfWorld* w, uint8_t kind);  // run_on_world, events, commands, on_tick, setting changes
void dispatch_server_pre();                            // saves events, world-setting writes, timers, commands.txt
void install_ticks();

// ---------------------------------------------------------------------------------------------- the table -------
const EmfApi* api_table();
void fill_core(EmfApi* t);    // api_core.cpp
void fill_game(EmfApi* t);    // api_game.cpp
void fill_spawn(EmfApi* t);   // api_spawn.cpp (spawn, spawn_ex, v47 spawn_facing)
void fill_events(EmfApi* t);  // api_events.cpp
void fill_hooks(EmfApi* t);   // api_hooks.cpp
void fill_commands(EmfApi* t);  // commands.cpp
void fill_settings(EmfApi* t);  // settings.cpp
void fill_level(EmfApi* t);       // api_level.cpp (v22)
void fill_components(EmfApi* t);  // api_components.cpp (v23, v25)
void fill_windows(EmfApi* t);     // api_windows.cpp (v24, v26; it also fills the wheel)
void fill_wheel(EmfApi* t);       // api_wheel.cpp (v10)
void fill_attributes(EmfApi* t);  // api_attributes.cpp (v18)
void fill_net(EmfApi* t);  // api_net.cpp (v27, EMF's wire protocol over the game's chat)
void fill_terrain(EmfApi* t); // v44/v45/v47 translation onto kiln.build
void terrain_server_pre();
void terrain_after_step(EmfWorld* w, uint8_t kind);
void terrain_world_stopping(); // cancellations delivered at the next server Post, not on the destroying thread
void terrain_world_started();
std::string terrain_status_json();
void fill_v40(EmfApi* t);  // api_v40.cpp (v28-v48, without the two chat functions of api_game.cpp)
// v39 enemy orders and v40 move orders (api_orders.cpp, orders_model.*; since 0.2.0-dev.31.13): after fill_v40
void fill_orders(EmfApi* t);
void orders_after_step(EmfWorld* w, uint8_t kind);  // the server world's: orders and moves walked on, callbacks
void orders_world_stopping();                       // every move ends cancelled (world NULL), every order ends
void orders_world_started();
void orders_shutdown();                             // on_unload: kiln.creatures' holds of kiln-emf go
void fill_markers(EmfApi* t);  // api_markers.cpp (v33 markers and v47 bars on kiln.markers), after fill_v40
void markers_after_step(EmfWorld* w, uint8_t kind);  // queued server calls, follow and map retries
void markers_world_stopping(uint8_t kind);           // that world's markers of every mod go
void markers_shutdown();                             // releases every mod's kiln.markers owner
void net_on_ready();
void net_server_pre();
void net_after_step(EmfWorld* w, uint8_t kind);
void net_world_stopping(uint8_t kind);
void net_world_started(uint8_t kind);
void net_shutdown();
void core_note(std::string_view text);  // api_game.cpp: a note of kiln-emf itself on the player's screen (any thread)
void portraits_on_ready();  // offline generation through the separate GPL kiln-eml component (drawing on a worker)
// image_load("portrait:<key>"): true when the picture is not being drawn (any more). While it is, the thread that started the
// drawing waits (for 120 s all calls together); any other thread gets false at once (image_load then returns a pending handle).
bool portraits_wait(const std::string& key);
// true while the portrait worker is drawing `key`. Never waits.
bool portraits_drawing(const std::string& key);
// `fn` runs on the portrait worker once its drawing has finished (EMF's image handles of pictures that were still drawing).
void portraits_when_drawn(void (*fn)());
bool commands_is_admin(uint32_t entity, uint64_t account);  // same policy as command_sender.admin

// tick hooks of the parts
void core_after_step(EmfWorld* w, uint8_t kind);
void events_after_step(EmfWorld* w, uint8_t kind);
void commands_after_step(EmfWorld* w, uint8_t kind);
void settings_after_step(EmfWorld* w, uint8_t kind);
class SlowParts;                     // slow_parts.h
void game_server_pre(SlowParts* parts);  // marks saves, world settings, teleport timers in `parts`
void commands_server_pre();          // no disk: tells the disk thread that the server world steps (commands_poll_files)
void events_on_ready();    // registers the kiln.world programs that on_event / register_program asked for
void commands_on_ready();  // subscribes to kiln.chat
void game_on_ready();
void hud_on_ready();
// v22-v26 parts
void level_after_step(EmfWorld* w, uint8_t kind);
void components_on_ready();  // before events_on_ready: the emf_components program and the E-prompt listener
void components_after_step(EmfWorld* w, uint8_t kind);
void windows_after_step(EmfWorld* w, uint8_t kind);
void windows_world_stopping(uint8_t kind);
void windows_world_started(uint8_t kind);
void events_world_stopping();               // the server world's
void events_world_stopping(uint8_t kind);   // that world kind's (EMF_WORLD_SERVER / EMF_WORLD_CLIENT)
void events_shutdown();
void program_hooks_shutdown();
void keep_adapter_mapped();
void windows_shutdown();
// the radial wheel (api_wheel.cpp): the windows' hooks call these
void wheel_after_step(EmfWorld* w, uint8_t kind);  // delivers the picks and cancels of wheels of that world kind
void wheel_world_stopping(uint8_t kind);           // closes a wheel of that kind, drops its undelivered results
void wheel_world_started(uint8_t kind);
void wheel_shutdown();
bool wheel_is_open();                              // a wheel is open (any mod's): window_open is refused meanwhile
void components_world_stopping();
void components_world_started();
bool server_requests_allowed();
bool windows_want_frame();  // a mod window is open (kiln.ui demand)
// attribute_get / attribute_add of EMF_ATTR_XP* / EMF_ATTR_LEVEL (api_attributes.cpp hands them over)
bool level_attribute_get(Mod* m, EmfWorld* w, uint32_t entity, uint32_t attr, int32_t* out);
bool level_attribute_add(Mod* m, EmfWorld* w, uint32_t entity, uint32_t attr, int32_t delta);
// spawn as EMF's spawn does (api_spawn.cpp), for spawn_offer
uint32_t spawn_for(Mod* m, EmfWorld* w, const uint8_t guid[16], double x, double y, double z, const char* fn);
// a listener of kiln-emf itself for an engine event (no mod), before events_on_ready
bool events_add_internal(const char* name, uint32_t expected_size, EmfWorldEventCallback cb, void* user);
// the same, with a gate: while wanted() is false (and no other registration of that event wants it) the relay leaves
// the game's queue alone, so a listener nobody uses costs one call per step (on_hit, api_hits.cpp)
bool events_add_internal_gated(const char* name, uint32_t expected_size, EmfWorldEventCallback cb, void* user,
                               bool (*wanted)());
// after events_on_ready: the server world's relay of `name` is a registered kiln.world program with its queue slot
bool events_relay_registered(const char* name);
// v31 on_hit (api_hits.cpp): after load_mods and before events_on_ready, then right after events_on_ready
void hits_on_ready(bool any_mod_loaded);
void hits_after_ready();
void fill_hits(EmfApi* t);  // after fill_v40: on_hit (on_hit_before and the hit rules stay in api_v40.cpp)
// v32 buffs (api_buffs.cpp, since 0.2.0-dev.31.13): kiln.playerstate reads, kiln.abilities programs for giving and
// removing, the players' buff events; v35 player_view (api_view.cpp). Both after fill_v40.
void fill_buffs(EmfApi* t);
void fill_view(EmfApi* t);  // player_view; fog_at, map_markers and the canvases answer "not yet" there
void buffs_server_pre();                           // starts queued gives and removals (the server world's Pre)
void buffs_after_step(EmfWorld* w, uint8_t kind);  // server: answers, on_buff; client: texts, the view's copy
void buffs_world_stopping();                       // the server world's: waiting requests are refused at the next Post
// a posted task that runs `f` on the server world's tick thread (Pre of the next step); false when it was not queued
bool post_tick(std::function<void()> f);
// kiln.ui v2 slots (a core with an older kiln.ui ends its table above them)
inline bool ui_has_wheel() {
    return S.ui && S.ui->header.size >= offsetof(KilnUiApi, wheel_active) + sizeof(void*) && S.ui->wheel_open && S.ui->wheel_close &&
           S.ui->wheel_active;
}
inline bool ui_has_item_clicked() {
    return S.ui && S.ui->header.size >= offsetof(KilnUiApi, is_item_clicked) + sizeof(void*) && S.ui->is_item_clicked;
}
// the HUD and windows share kiln.ui's demand
void ui_refresh_demand();

// ---------------------------------------------------------------------------------------------- settings --------
void settings_load_mod(Mod* m);   // declares the manifest settings, seeds and mirrors the ini, reads `enabled`
bool settings_enabled(const Mod* m, bool fallback);
void settings_write_schema(const Mod* m);
std::string settings_summary(const Mod* m);  // for `settings <mod>`
bool settings_set_text(const char* mod, const char* key, const char* value, std::string* why);

// ---------------------------------------------------------------------------------------------- commands --------
struct CommandRun {
    std::string line;
    EmfCommandSender sender{};
    uint32_t replies = 0;   // chat lines sent back so far (at most 10 reach the chat)
    bool to_chat = false;   // reply into the sender's chat
};
void command_reply_text(const std::shared_ptr<CommandRun>& run, std::string_view text);
bool run_builtin_or_mod_command(EmfWorld* w, const std::shared_ptr<CommandRun>& run);

// ---------------------------------------------------------------------------------------------- disk thread -----
// io.cpp (io_worker.h): the tick never waits for the disk (0.2.0-dev.29.17). io_start in on_ready; io_stop in on_unload.
void io_start();
void io_write(const std::filesystem::path& p, std::string text);  // atomic; a newer text of a waiting file replaces it
void io_post(const char* what, std::function<void()> job);       // in order with the writes, on the disk thread
void io_stop(uint64_t timeout_ms);
void commands_poll_files();  // commands.cpp, on the disk thread: emf_core.ini and emf/commands.txt while the server steps

// ---------------------------------------------------------------------------------------------- status ----------
void write_status();
void mark_status_dirty();
extern std::string g_stepped_aside;  // why kiln-emf steps aside ("" = it runs)

// ---------------------------------------------------------------------------------------------- strings ---------
std::string vformat(const char* fmt, va_list ap);
std::string format(const char* fmt, ...);
std::string to_utf8(const std::wstring& w);
std::wstring to_wide(std::string_view s);
std::string lower(std::string_view s);
bool iequals(std::string_view a, std::string_view b);
inline KilnStr kstr(std::string_view s) { return KilnStr{s.data(), s.size()}; }
inline KilnStr kstr(const char* s) { return KilnStr{s, s ? std::strlen(s) : 0}; }
inline std::string_view sv(KilnStr s) { return std::string_view(s.ptr ? s.ptr : "", s.ptr ? s.len : 0); }
void copy_cstr(char* dst, size_t cap, std::string_view s);
bool read_file(const std::filesystem::path& p, std::string* out);
bool write_file_atomic(const std::filesystem::path& p, std::string_view data);
std::string path_utf8(const std::filesystem::path& p);
uint64_t now_ms();

}  // namespace kemf
