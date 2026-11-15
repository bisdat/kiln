// SPDX-License-Identifier: MIT
// kiln.vfx: the plugin. core.cpp is the queue and game.cpp the game layer (native/core/abi/services/kiln_vfx.h,
// docs/kiln/services/vfx.md). Client only; kiln/config/vfx.json {"enabled": false} turns it off before anything is hooked. Labs can
// run the simulated game with {"sim": true} on a dedicated server, stepped by the server world, or {"testFailSoft": true}: the real
// game layer on an exe where no signature resolves.
#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <cstddef>
#include <cstring>
#include <string>

#include "boot.h"
#include "game.h"
#include "kiln_boot_collector.hpp"
#include "nlohmann/json.hpp"
#include "services/kiln_caps.h"

namespace kvx {

namespace {

const KilnApi* g_api = nullptr;
KilnOwner g_owner = 0;

struct Config {
    bool enabled = true;
    bool sim = false;
    bool test_fail_soft = false;  // lab only: run the real game layer on a dedicated server, where no signature resolves
    double trace = 0;             // seconds: log what the game creates after each of Kiln's plays (true = 3)
};

// kiln/config/vfx.json. Missing or broken: the defaults, and a broken file says so.
Config read_config() {
    Config c;
    char dir[1024];
    size_t n = 0;
    if (g_api->path(KILN_PATH_CONFIG, dir, sizeof dir, &n) != KILN_OK || n >= sizeof dir) return c;
    const std::string path = std::string(dir, n) + "/vfx.json";
    wchar_t wide[1200];
    const int wn = MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, wide, 1200);
    if (wn <= 0) return c;
    FILE* f = _wfopen(wide, L"rb");
    if (!f) return c;
    std::string text;
    char buf[4096];
    for (size_t got; (got = std::fread(buf, 1, sizeof buf, f)) > 0;) text.append(buf, got);
    std::fclose(f);
    const auto j = nlohmann::json::parse(text, nullptr, false, true);
    if (j.is_discarded() || !j.is_object()) {
        KV_W("vfx: %s is not valid JSON: using the defaults", path.c_str());
        return c;
    }
    if (auto it = j.find("enabled"); it != j.end() && it->is_boolean()) c.enabled = it->get<bool>();
    if (auto it = j.find("sim"); it != j.end() && it->is_boolean()) c.sim = it->get<bool>();
    if (auto it = j.find("testFailSoft"); it != j.end() && it->is_boolean()) c.test_fail_soft = it->get<bool>();
    if (auto it = j.find("trace"); it != j.end()) c.trace = it->is_boolean() ? (it->get<bool>() ? 3.0 : 0.0) : it->is_number() ? it->get<double>() : 0.0;
    return c;
}

void KILN_CALL on_world(const KilnWorldEvent* ev, void*) {
    if (ev && ev->event == KILN_WORLD_STOPPING && ev->kind == KILN_WORLD_CLIENT) core_world_ended();
}

void KILN_CALL on_sim_tick(const KilnTickInfo*, void*) { sim_update(); }

// ---- the boot generation's vfx collector (chapter 08) ----
// Client files stay in on_ready in this release, independently of the capability gate. The
// empty collector checks those failures and warns/refuses staged content. Dedicated-server
// and config-off collectors retain their existing behavior. Prepare builds remain for labs.
bool g_boot_build = false;
bool g_held_build = false;  // files built at start, outside the boot generation (not a gate hold)
// Written by on_ready, read by prepare after load_plugins returns (core/src/boot.cpp).
std::vector<BuildFailure> g_held_failures;
struct BootPlan {
    bool built;
};
void KILN_CALL destroy_boot(const void* p) {
    const auto* plan = static_cast<const BootPlan*>(p);
    if (plan->built) build_forget();
    delete plan;
}
// Servers render none. Config-off clients refuse required content; ordinary clients build
// files first, then this collector refuses failed required files or unsupported staged content.
struct EmptyWhy {
    const char* why;
    bool server, all_off;
};
EmptyWhy g_empty_off{"kiln.vfx is off by kiln/config/vfx.json", false, true};
EmptyWhy g_empty_server{"a dedicated server renders no VFX", true, false};
EmptyWhy g_empty_held{"in this release a client builds VFX and material files at start, outside the boot generation; VFX or materials staged into a boot generation are not built yet", false, false};

KilnResult KILN_CALL prepare_empty(void* user, const KilnBootPrepareInputV1* in, KilnBootPreparedV1* out, KilnBootIssueV1* issue) {
    const EmptyWhy& e = *static_cast<const EmptyWhy*>(user);
    const char* why = e.why;
    const kiln::boot::Input input = kiln::boot::parse(in);
    if (held_build_refusal(&e == &g_empty_held, g_held_failures, input, out, issue)) return KILN_OK;
    const auto docs = input.docs("vfx");
    for (const auto& d : docs)  // read or not, a document that is no blocks file refuses its mod, as everywhere
        if (!d.document.is_object())
            return kiln::boot::refuse(out, issue, d.owner, "vfx", d.key, "kiln.vfx: a boot document is not a blocks file (a JSON object)");
    if (!e.server) {
        for (const auto& d : docs)
            if (input.strict(d.owner))
                return kiln::boot::refuse(out, issue, d.owner, "vfx", d.key,
                                          std::string("kiln.vfx: its staged VFX or materials cannot be built in this start: ") + why);
        if (e.all_off) try {
            const auto hit = required_vfx_content(g_api, input, true, true);
            if (!hit.first.empty())
                return kiln::boot::refuse(out, issue, hit.first, "vfx", hit.second,
                                          "kiln.vfx: " + hit.second + ": its VFX or materials cannot be built in this start: " + why);
        } catch (const kiln::boot::Refusal& r) {
            return kiln::boot::refuse(out, issue, r.mod, r.kind, r.key, r.what());
        } catch (const std::exception&) {
        }
    } else {
        std::set<std::string> said;
        for (const auto& d : docs)
            if (input.strict(d.owner) && said.insert(d.owner).second)
                KV_I("vfx: %s's boot content is required: its staged VFX and materials are built by each player's game, not by this "
                     "dedicated server (it renders none), so nothing of it is missing here", d.owner.c_str());
    }
    if (!docs.empty()) {
        issue->code = KILN_BOOT_ISSUE_WARNING | 0x10001u;
        kiln::boot::put(issue->mod_id, docs.front().owner);
        kiln::boot::put(issue->kind, std::string("vfx"));
        kiln::boot::put(issue->key, docs.front().key);
        kiln::boot::put(issue->phase, std::string("prepare"));
        kiln::boot::put(issue->reason, std::to_string(docs.size()) + " staged VFX or material definition(s) are not built here: " +
                                           static_cast<const char*>(why));
    }
    return kiln::boot::prepared(out, new BootPlan{false}, &destroy_boot);
}
KilnResult KILN_CALL prepare_build(void*, const KilnBootPrepareInputV1* in, KilnBootPreparedV1* out, KilnBootIssueV1* issue) {
    const kiln::boot::Input input = kiln::boot::parse(in);
    if (!input.ok) return kiln::boot::refuse(out, issue, "", "", "", "kiln.vfx: the boot envelope is not JSON");
    try {
        build_on_ready(g_api, g_owner, &input);
    } catch (const kiln::boot::Refusal& r) {
        build_forget();
        return kiln::boot::refuse(out, issue, r.mod, r.kind, r.key, r.what());
    }
    return kiln::boot::prepared(out, new BootPlan{true}, &destroy_boot);
}
bool vfx_held(const KilnApi* api) {
    const auto* caps = static_cast<const KilnCapsApi*>(api->service(KilnStr{KILN_CAPS_SERVICE, sizeof(KILN_CAPS_SERVICE) - 1}, 1));
    return kiln_caps_held(caps, KilnStr{"mods.boot.vfx", 13}) != 0;
}

// Before the game's entry point: ordinary client files are served before resource enumeration,
// including with a boot profile. Retain failures for the required-participant check in prepare.
void KILN_CALL on_ready(void*) {
    if (!g_boot_build) {
        g_held_failures.clear();
        build_on_ready(g_api, g_owner, nullptr, g_held_build ? &g_held_failures : nullptr);
    }
}

static_assert(sizeof(KilnVfxPlay) == 128 && sizeof(KilnVfxInfo) == 88 && sizeof(KilnVfxStatus) == 64);
// v1's 13 members (112 bytes), then the resource budget's three, appended in 0.2.0-dev.29: the table grows at its end only
static_assert(offsetof(KilnVfxApi, bind_guid) == 104 && offsetof(KilnVfxApi, resource_budget) == 112 &&
                  offsetof(KilnVfxApi, define_vfx_result) == 120 && offsetof(KilnVfxApi, vfx_registration) == 128 && sizeof(KilnVfxApi) == 136,
              "kiln.vfx's table grows at its end only");

const KilnVfxApi kTable = {
    {sizeof(KilnVfxApi), KILN_VFX_VERSION},
    &core_status,
    &core_play,
    &core_stop,
    &core_set_param,
    &core_state,
    &core_info,
    &core_stop_owner,
    &core_is_registered,
    &core_preload,
    &core_status_json,
    &build_define_json,
    &build_status_json,
    &core_bind_guid,
    &build_resource_budget,
    &build_define_vfx_result,
    &build_vfx_registration,
};

}  // namespace

void vlog(int level, const char* fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    const int n = std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n <= 0 || !g_api) return;
    g_api->log(g_owner, level, KilnStr{buf, static_cast<size_t>(n < static_cast<int>(sizeof(buf)) ? n : sizeof(buf) - 1)});
}

uint32_t thread_id() { return GetCurrentThreadId(); }
uint64_t now_ms() { return GetTickCount64(); }

}  // namespace kvx

extern "C" KILN_EXPORT KilnResult KILN_CALL kiln_plugin_main(const KilnApi* api, KilnOwner owner, KilnPluginInfo* info) {
    using namespace kvx;
    if (api->abi_version != KILN_ABI_VERSION || api->size < sizeof(KilnApi)) return KILN_ERR_UNAVAILABLE;
    g_api = api;
    g_owner = owner;
    info->id = KilnStr{KILN_VFX_SERVICE, sizeof(KILN_VFX_SERVICE) - 1};
    info->version = KilnStr{"1", 1};
    const Config cfg = read_config();
    if (!cfg.enabled) {
        KV_I("vfx: off by kiln/config/vfx.json: nothing hooked, mods play nothing");
        // the empty plan as on a dedicated server (the module stays mapped; see there)
        kiln::boot::collect(api, owner, "vfx", &prepare_empty, &g_empty_off);
        return KILN_ERR_UNAVAILABLE;
    }
    KilnHandle h = 0;
    if (cfg.sim) {
        KV_W("vfx: SIMULATED game (kiln/config/vfx.json \"sim\"): for labs and tests only, nothing reaches the real VFX system");
        core_set_game(sim_game());
        api->subscribe_tick(owner, KILN_TICK_PRE, &on_sim_tick, nullptr, &h);
    } else {
        if (api->side() != KILN_SIDE_CLIENT && !cfg.test_fail_soft) {
            KV_I("vfx: not a game client (the dedicated server has no VFX): kiln.vfx unavailable here; Kiln.Vfx relays server calls to Kiln clients");
            // A boot generation's vfx part here: the empty validated plan (chapter 08 §4.3 step 4), so a both-sides mod's staged VFX is not a
            // missing collector on a server. Registered although this function answers UNAVAILABLE: kiln-core keeps a plugin that answers so
            // mapped for the process (api.cpp load_one: no FreeLibrary) and only revokes its owner, which kiln.boot uses for log names alone;
            // prepare_empty reads nothing but its input.
            kiln::boot::collect(api, owner, "vfx", &prepare_empty, &g_empty_server);
            return KILN_ERR_UNAVAILABLE;
        }
        real_game_trace(cfg.trace);
        Game* game = real_game(api, owner);
        if (!game) return KILN_ERR_UNAVAILABLE;
        core_set_game(game);
        api->subscribe_world(owner, &on_world, nullptr, &h);
    }
    const KilnResult rc = api->register_service(owner, KilnStr{KILN_VFX_SERVICE, sizeof(KILN_VFX_SERVICE) - 1}, &kTable);
    if (rc == KILN_OK) {
        core_set_build_hooks(&build_on_update, &build_refuses, &build_one_shot_ns);
        budget_start(api, owner);  // the rows of the appended members, with the table (before the mods load)
        info->on_ready = &on_ready;
        // Keep client file builds at start until boot staging is implemented. Server/config-off
        // cases returned above; non-client lab routes preserve their previous choice.
        if (!boot_builds_in_prepare(api->side(), vfx_held(api))) {
            // No boot profile: leave even failure collection out of the ordinary on_ready build.
            g_held_build = kiln::boot::collect(api, owner, "vfx", &prepare_empty, &g_empty_held);
        } else
            g_boot_build = kiln::boot::collect(api, owner, "vfx", &prepare_build, nullptr);
        if (g_boot_build) KV_I("vfx: boot profile: mods' VFX and materials are built with the boot generation");
        Game* g = core_game();
        KV_I("vfx: kiln.vfx v1 ready (guard %s, play %s); waiting for the first FxController update", g->has_guard() ? "on" : "OFF",
             g->can_play() ? "on" : "OFF");
    }
    return rc;
}
