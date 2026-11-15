// SPDX-License-Identifier: MIT
// native_pack.cpp on the host (0.2.0-dev.29, package contentstop): when the WHOLE build of the mods' native quests and
// NPCs fails, nothing of it is served and the start is stopped (refuse_start, StartStop::Content) before any world;
// one mod's own errors leave that mod out and the start goes on; an input with no such content never stops anything.
// Each whole failure is shown twice where it can be: by its real cause (a manifest field that throws, a game resource
// that is not there, kiln.assets refusing, the hook not going in) and by the lab switch kiln/control/knowledge.fail-build.
// The build itself (questpack.cpp) is a fake the test steers; its real code is tested on the game's data by
// tools/kfc/tests/test_questbuild.py. refuse_start is a stub that records the call and returns, as it does once a world
// ran (start_stop.cpp ends the process otherwise). Also the texts of both stops (start_stop.h).
// Run by tests/run.sh: g++ -fsanitize=address,undefined.
#include <windows.h>  // tests/host/windows.h

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "../native_pack.cpp"

static int g_failures = 0, g_checks = 0;
#define CHECK(c)                                                          \
    do {                                                                  \
        ++g_checks;                                                       \
        if (!(c)) {                                                       \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);      \
            ++g_failures;                                                 \
        }                                                                 \
    } while (0)

namespace fs = std::filesystem;

// ---- what the service's other files define
namespace kiln::knowledge {
const KilnApi* g_api = nullptr;
KilnOwner g_owner = 7;
std::vector<std::string> t_log;
void logf(int, const char* fmt, ...) {
    char b[4096];
    va_list a;
    va_start(a, fmt);
    std::vsnprintf(b, sizeof b, fmt, a);
    va_end(a);
    t_log.emplace_back(b);
}
uint32_t roster_max(uint32_t, std::string*) { return 64; }
void roster_built(std::vector<uint32_t>, std::vector<uint32_t>) {}
struct StopCall {
    StartStop kind;
    std::string why;
    std::vector<std::string> mods;
};
std::vector<StopCall> t_stops;
std::vector<std::string> t_order;  // stops and activation faults, in the order they came
void refuse_start(StartStop kind, const std::string& why, const std::vector<std::string>& mods) {
    t_stops.push_back({kind, why, mods});
    t_order.push_back(kind == StartStop::Content ? "stop:content" : "stop:roster");
}
void start_stop_main() {}
}  // namespace kiln::knowledge

// ---- questpack.cpp's entry points: a build the test steers
namespace kiln::qb {
ojson parse_blocks_json(const std::string& text) { return ojson::parse(text, nullptr, false, true); }
Guid derive_guid(const std::string& mod, const std::string& kind, const std::string& name) {
    Guid ns;
    parse_guid("928632ef-3270-4bc2-8657-0b6bcdeb2976", &ns);
    return uuid5(ns, mod + ":" + kind + ":" + name);
}
enum class Fake { Ok, Throw, Failed, Roster };
Fake t_fake = Fake::Ok;
int t_compiles = 0;
// A document with "broken" is one mod's own error (ok: false); "knowledge", "quests" or "npcs" is native content;
// "template": a template the build adds for that mod.
PackOut compile_pack(const std::vector<ModInput>& mods, const GameData& game) {
    ++t_compiles;
    PackOut out;
    ojson m_mods = ojson::array();
    for (const ModInput& m : mods) {
        bool broken = false, native = false;
        for (const ojson& d : m.documents) {
            broken = broken || d.contains("broken");
            native = native || d.contains("knowledge") || d.contains("quests") || d.contains("npcs");
        }
        if (broken) {
            out.errors.push_back(m.id + ": knowledge x: type must be bool, counter or flags");
            m_mods.push_back({{"id", m.id}, {"ok", false}, {"errors", {m.id + ": knowledge x: type must be bool, counter or flags"}}});
            continue;
        }
        if (!native) continue;
        out.any = true;
        m_mods.push_back({{"id", m.id}, {"ok", true}, {"knowledge", {{"k1", "1a2b3c4d"}}}});
        for (const ojson& d : m.documents)
            if (d.contains("template")) {
                TemplateOut t;
                t.guid = derive_guid(m.id, "tpl", d["template"].get<std::string>());
                t.name = d["template"].get<std::string>();
                t.is_new = true;
                t.data = {1, 2, 3};
                out.templates.push_back(t);
            }
    }
    out.manifest = {{"mods", m_mods}, {"errors", out.errors}};
    if (!out.any) return out;
    if (t_fake == Fake::Throw) throw std::runtime_error("the knowledge map is damaged (test)");
    const auto fail = [&](const std::string& why) {
        out.errors.push_back(why);
        out.ok = false;
        out.any = false;
        out.manifest["errors"] = out.errors;
        out.manifest["failed"] = why;
        return out;
    };
    Guid gk;
    parse_guid(kGuidKnowledge, &gk);
    const uint8_t* p = nullptr;
    size_t n = 0;
    if (!game.read(gk, kTypeKnowledge, &p, &n)) return fail("GameKnowledgeResource is not in the game's data");
    if (t_fake == Fake::Failed) return fail("two mods define the clone 00000000-0000-0000-0000-000000000001");
    if (t_fake == Fake::Roster) {
        out.roster_refused = true;
        out.roster_refusal = "the roster holds at most 64 NPCs (a 64-bit unlock mask): 35 of the game's + 30 new";
        for (const auto& e : m_mods)
            if (e.value("ok", false)) out.roster_mods.push_back(e.value("id", ""));
        return fail(out.roster_refusal);
    }
    out.res_knowledge = {1, 2, 3, 4};
    out.res_npcs = {5, 6};
    out.ok = out.errors.empty();
    return out;
}
}  // namespace kiln::qb

using namespace kiln::knowledge;

// ---- the fake Kiln
namespace {
fs::path t_root;
bool t_client = false, t_symbol = true, t_hook_ok = true, t_assets = true, t_game_knowledge = true, t_service_up = false;
KilnResult t_put = KILN_OK, t_put_templates = KILN_OK;
std::vector<std::string> t_puts;
std::vector<std::string> t_faults;  // activation_fault(mod)
const void* t_plan = nullptr;       // what kiln.boot published for "knowledge"
KilnTickFn t_post = nullptr;        // a server world Post subscription
void* t_post_user = nullptr;
uint8_t t_res[8] = {9, 9, 9, 9};

KilnResult KILN_CALL f_path(int32_t kind, char* b, size_t cap, size_t* n) {
    const char* leaf = kind == KILN_PATH_MODS ? "mods" : kind == KILN_PATH_DATA ? "data" : kind == KILN_PATH_CONTROL ? "control"
                     : kind == KILN_PATH_STATE ? "state" : kind == KILN_PATH_LOGS ? "logs" : "kiln";
    const std::string s = (t_root / leaf).string();
    *n = s.size();
    if (s.size() > cap) return KILN_ERR_BUFFER_TOO_SMALL;
    std::memcpy(b, s.data(), s.size());
    return KILN_OK;
}
KilnResult KILN_CALL f_build_info(KilnBuildInfo* out) {
    out->side = t_client ? KILN_SIDE_CLIENT : KILN_SIDE_SERVER;
    return KILN_OK;
}
uint32_t KILN_CALL f_side() { return t_client ? KILN_SIDE_CLIENT : KILN_SIDE_SERVER; }
KilnResult KILN_CALL f_symbol(KilnStr name, uintptr_t* out) {
    if (!t_symbol || std::string(name.ptr, name.len) != "assets/ResourceInit") return KILN_ERR_NOT_FOUND;
    *out = 0x140001000;
    return KILN_OK;
}
KilnResult KILN_CALL f_sub_world_tick(KilnOwner, uint32_t kind, int32_t phase, KilnTickFn fn, void* user, KilnHandle* h) {
    if (kind == KILN_WORLD_SERVER && phase == KILN_TICK_POST) t_post = fn, t_post_user = user;
    if (h) *h = 42;
    return KILN_OK;
}
KilnResult KILN_CALL f_sub_tick(KilnOwner o, int32_t phase, KilnTickFn fn, void* user, KilnHandle* h) {
    return f_sub_world_tick(o, KILN_WORLD_SERVER, phase, fn, user, h);
}
KilnResult KILN_CALL f_unsub(KilnHandle) { return KILN_OK; }

// kiln.assets
KilnResult KILN_CALL a_data(const KilnAssetKey* k, const void** p, size_t* n) {
    // the game's GameKnowledgeResource, once the game's resource service is up (ResourceInit ran)
    if (k->type != kiln::qb::kTypeKnowledge || !t_game_knowledge || !t_service_up) return KILN_ERR_NOT_FOUND;
    *p = t_res;
    *n = sizeof t_res;
    return KILN_OK;
}
KilnResult KILN_CALL a_put(KilnOwner, const KilnAssetKey* k, const void*, size_t, uint32_t) {
    const KilnResult r = k->type == kiln::qb::kTypeTemplate ? t_put_templates : t_put;
    if (r == KILN_OK) t_puts.push_back(std::to_string(k->type));
    return r;
}
KilnResult KILN_CALL a_list(uint32_t, KilnAssetKey*, size_t, size_t* n) {
    *n = 0;
    return KILN_OK;
}
void KILN_CALL a_revert(KilnOwner) {}
KilnAssetsApi t_assets_api = [] {
    KilnAssetsApi a{};
    a.header = {sizeof(KilnAssetsApi), KILN_ASSETS_VERSION};
    a.resource_data = &a_data;
    a.resource_put = &a_put;
    a.resource_list = &a_list;
    a.revert_owner = &a_revert;
    return a;
}();

// kiln.hooks: the raw detour goes in or not
KilnResult KILN_CALL h_raw(KilnOwner, uintptr_t, const KilnHookOrder*, void*, void** original, KilnHandle* h) {
    if (!t_hook_ok) return KILN_ERR_UNAVAILABLE;
    *original = nullptr;
    *h = 5;
    return KILN_OK;
}
KilnResult KILN_CALL h_revoke(KilnHandle) { return KILN_OK; }
KilnHooksApi t_hooks_api = [] {
    KilnHooksApi h{};
    h.header = {sizeof(KilnHooksApi), KILN_HOOKS_VERSION};
    h.raw_detour = &h_raw;
    h.revoke = &h_revoke;
    return h;
}();

// kiln.boot: what it published, and the activation faults it was told
bool t_boot = false;
KilnBootPrepareFnV1 t_prepare = nullptr;
const void* KILN_CALL b_active_plan(KilnStr, uint64_t* g) {
    if (g) *g = t_plan ? 1 : 0;
    return t_plan;
}
KilnResult KILN_CALL b_collector(KilnOwner, KilnStr, KilnBootPrepareFnV1 fn, void*) {
    if (!t_boot) return KILN_ERR_UNAVAILABLE;
    t_prepare = fn;
    return KILN_OK;
}
KilnResult KILN_CALL b_fault(KilnOwner, KilnStr mod, KilnStr, KilnStr, KilnStr) {
    t_faults.emplace_back(mod.ptr, mod.len);
    t_order.push_back("fault:" + std::string(mod.ptr, mod.len));
    return KILN_OK;
}
KilnBootApiV1 t_boot_api = [] {
    KilnBootApiV1 b{};
    b.header = {sizeof(KilnBootApiV1), KILN_BOOT_VERSION};
    b.active_plan = &b_active_plan;
    b.collector = &b_collector;
    b.activation_fault = &b_fault;
    return b;
}();

const void* KILN_CALL f_service(KilnStr name, uint32_t) {
    const std::string n(name.ptr, name.len);
    if (n == KILN_ASSETS_SERVICE) return t_assets ? &t_assets_api : nullptr;
    if (n == KILN_HOOKS_SERVICE) return &t_hooks_api;
    if (n == KILN_BOOT_SERVICE) return &t_boot_api;
    return nullptr;  // kiln.profile (drop-ins are scanned), kiln.content
}

KilnApi t_api = [] {
    KilnApi a{};
    a.size = sizeof(KilnApi);
    a.abi_version = KILN_ABI_VERSION;
    a.path = &f_path;
    a.build_info = &f_build_info;
    a.side = &f_side;
    a.symbol = &f_symbol;
    a.service = &f_service;
    a.subscribe_tick = &f_sub_tick;
    a.subscribe_world_tick = &f_sub_world_tick;
    a.unsubscribe = &f_unsub;
    return a;
}();

void write(const fs::path& p, const std::string& text) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << text;
}
// A mod folder in kiln/mods: its kiln.mod.json and quests/<id>.json
void mod(const std::string& id, const std::string& quests, const std::string& manifest_extra = "") {
    write(t_root / "mods" / id / "kiln.mod.json", "{\"id\":\"" + id + "\",\"side\":\"both\"" + manifest_extra + "}");
    if (!quests.empty()) write(t_root / "mods" / id / "quests" / (id + ".json"), quests);
}
const std::string kGood = R"({"format":1,"knowledge":{"met":{"scope":"world"}}})";
const std::string kBroken = R"({"format":1,"broken":true,"knowledge":{"x":{"type":"bogus"}}})";
const std::string kChatQuest = R"({"format":1,"quests":{"q":{"name":"Q","journal":"chat","giver":"n","steps":[{"id":"s","text":"Talk"}]}},"npcs":{"n":{"template":"NPC_Workshop_Alchemist01","place":{"near":"player"}}}})";
const std::string kChatDialogue = R"({"format":1,"npcs":{"n":{"template":"NPC_Workshop_Alchemist01","dialogue":"d","place":{"near":"player"}}},"dialogues":{"d":{"native":false,"start":"a","nodes":{"a":{"text":"Hello","options":[{"text":"Bye"}]}}}}})";
void lab_switch(const std::string& word) { write(t_root / "control" / "knowledge.fail-build", word + "\n"); }

// A fresh start: no mods, no switch, every fake as it is in a working game; the service's state as at a process start.
void start(const char* name) {
    std::printf("-- %s\n", name);
    std::error_code ec;
    fs::remove_all(t_root, ec);
    fs::create_directories(t_root / "mods");
    t_client = false, t_symbol = true, t_hook_ok = true, t_assets = true, t_game_knowledge = true, t_service_up = false;
    t_put = KILN_OK, t_put_templates = KILN_OK;
    t_puts.clear(), t_faults.clear(), t_stops.clear(), t_order.clear(), t_log.clear();
    t_plan = nullptr, t_post = nullptr, t_post_user = nullptr, t_boot = false, t_prepare = nullptr;
    kiln::qb::t_fake = kiln::qb::Fake::Ok;
    kiln::qb::t_compiles = 0;
    g_built = false;
    g_boot = false;
    A = nullptr;
    g_names.clear();
    g_template_names.clear();
    g_manifest = "{\"mods\":[],\"errors\":[]}";
}
// kiln_plugin_main and on_ready of a server, then the game's resource start (the ResourceInit hook's call)
void boot_and_resource_init() {
    native_pack_install();
    native_pack_boot();
    native_pack_ready();
    t_service_up = true;
    if (!g_boot) build_once();
}
bool logged(const std::string& part) {
    for (const auto& l : t_log)
        if (l.find(part) != std::string::npos) return true;
    return false;
}
bool content_stop(const std::vector<std::string>& mods, const std::string& why_part) {
    return t_stops.size() == 1 && t_stops[0].kind == StartStop::Content && t_stops[0].mods == mods &&
           t_stops[0].why.find(why_part) != std::string::npos;
}
void dump() {
    for (const auto& s : t_stops)
        std::printf("   stop %s: %s [%s]\n", s.kind == StartStop::Content ? "content" : "roster", s.why.c_str(), stop::csv(s.mods).c_str());
}
}  // namespace

// ---- the cases
void served_and_single_mod_errors() {
    start("a normal start serves the content and does not stop");
    mod("alpha", kGood);
    boot_and_resource_init();
    CHECK(t_stops.empty());
    CHECK(t_puts.size() == 2);  // GameKnowledgeResource and NpcCollection
    CHECK(logged("native: 1 mod(s):"));

    start("one mod's own errors: it is left out, the good one is served, the start goes on");
    mod("alpha", kGood);
    mod("broken", kBroken);
    boot_and_resource_init();
    CHECK(t_stops.empty());
    CHECK(t_puts.size() == 2);
    CHECK(logged("native: broken: knowledge x: type must be bool, counter or flags"));
    CHECK(g_names.count("alpha:k1") == 1 && g_names.count("broken:k1") == 0);
    dump();

    start("the roster that cannot be held stops as the roster, not as content");
    mod("alpha", kGood);
    kiln::qb::t_fake = kiln::qb::Fake::Roster;
    boot_and_resource_init();
    CHECK(t_stops.size() == 1 && t_stops[0].kind == StartStop::Roster && t_stops[0].mods == std::vector<std::string>{"alpha"});
    dump();
}

void empty_input_never_stops() {
    for (const char* word : {"read", "compile", "shared", "assets", "hook", "late"}) {
        start((std::string("no mod ships such content, lab switch ") + word + ": no stop").c_str());
        mod("plain", "");  // a mod without quests, blocks or code
        mod("chatonly", R"({"format":1})");  // a quests file that holds nothing native
        lab_switch(word);
        boot_and_resource_init();
        for (int i = 0; t_post && i < 40; ++i) t_post(nullptr, t_post_user);
        CHECK(t_stops.empty());
        dump();
    }
    start("no mod ships such content, the game resource missing and kiln.assets refusing: no stop");
    t_game_knowledge = false;
    t_put = KILN_ERR_UNAVAILABLE;
    boot_and_resource_init();
    CHECK(t_stops.empty());
    start("no mod ships such content, the hook not in: no stop");
    t_hook_ok = false;
    boot_and_resource_init();
    CHECK(t_stops.empty());
}

// These paths do not invoke the fake compiler: the real compiler's chat/placement classification must also govern
// the decision to stop when ResourceInit cannot be hooked. A required boot participant is still prepared normally.
void non_native_hook_failures() {
    for (const std::string& input : {
             kChatQuest, kChatDialogue,
             std::string(R"({"format":1,"quests":{"q":{"name":"Q","journal":"chat","steps":[{"id":"s","text":"Talk"}]}}})"),
             std::string(R"({"format":1,"dialogues":{"d":{"native":false,"start":"a","nodes":{"a":{"text":"Hello"}}}}})"),
             std::string(R"({"format":1,"dialogues":{"d":{"start":"a","nodes":{"a":{"text":"Unbound"}}}}})"),
             std::string(R"({"format":1,"npcs":{"n":{"template":"NPC_Workshop_Alchemist01","place":{"near":"player"}}}})")})
        for (bool unresolved : {false, true})
            for (bool boot : {false, true}) {
                start((std::string("no native resources, ") + (unresolved ? "unresolved ResourceInit" : "hook refused") +
                       (boot ? ", required boot mod: no stop" : ": no stop")).c_str());
                mod("chat", input);
                t_hook_ok = false;
                t_symbol = !unresolved;
                t_boot = boot;
                native_pack_install();
                native_pack_boot();
                native_pack_ready();
                CHECK(!g_no_hook.empty());
                if (boot) {
                    const std::string env = R"({"owners":[{"id":"chat","owner":11,"required":true}]})";
                    KilnBootPrepareInputV1 in{sizeof in, KILN_BOOT_VERSION, 1, KilnStr{env.data(), env.size()}};
                    KilnBootPreparedV1 out{sizeof out};
                    KilnBootIssueV1 issue{sizeof issue};
                    CHECK(t_prepare && t_prepare(nullptr, &in, &out, &issue) == KILN_OK);
                    CHECK(out.ok == 1 && out.plan);
                    if (out.ok && out.destroy) out.destroy(out.plan);
                }
                CHECK(t_stops.empty() && t_faults.empty());
                CHECK(t_puts.empty() && kiln::qb::t_compiles == 0);
                dump();
            }

    start("chat-only mods and kiln.assets unavailable at ResourceInit: no stop");
    mod("chat", kChatQuest);
    mod("talk", kChatDialogue);
    t_assets = false;
    boot_and_resource_init();
    CHECK(t_stops.empty() && kiln::qb::t_compiles == 0);

    // First NPC object definition wins; a shadowed clone must not turn placement-only input into native content.
    for (bool native : {false, true}) {
        start(native ? "the first native NPC stays native" : "a shadowed native NPC needs no rebuilt resources");
        const std::string placement = R"({"format":1,"npcs":{"n":{"template":"NPC_Workshop_Alchemist01","place":{"near":"player"}}}})";
        const std::string clone = R"({"format":1,"npcs":{"n":{"template":"NPC_Workshop_Alchemist01","clone":{}}}})";
        mod("chat", native ? clone : placement);
        write(t_root / "mods/chat/quests/z.json", native ? placement : clone);
        t_hook_ok = false;
        native_pack_install();
        native_pack_boot();
        native_pack_ready();
        CHECK(native ? content_stop({"chat"}, "assets/ResourceInit") : t_stops.empty());
        CHECK(kiln::qb::t_compiles == 0);
        dump();
    }

    // Last dialogue definition wins across documents, as in compile_pack; a later native definition must still stop.
    for (bool native : {false, true}) {
        start(native ? "a later native dialogue definition still stops" : "a later chat dialogue definition needs no resources");
        mod("chat", kChatDialogue);
        write(t_root / "mods/chat/quests/z.json", R"({"format":1,"dialogues":{"d":{"native":true}}})");
        if (!native) write(t_root / "mods/chat/quests/zz.json", R"({"format":1,"dialogues":{"d":{"native":false}}})");
        t_hook_ok = false;
        native_pack_install();
        native_pack_boot();
        native_pack_ready();
        CHECK(native ? content_stop({"chat"}, "assets/ResourceInit") : t_stops.empty());
        CHECK(kiln::qb::t_compiles == 0);
        dump();
    }
}

// Genuine native input still stops without a hook, independently of a knowledge entry. This includes the compiler's
// conservative clone/roster and qualified-reference cases, and native data alongside chat-only content.
void native_hook_controls() {
    for (const std::string& input : {
             std::string(R"({"format":1,"quests":{"q":{"name":"Q","steps":[{"id":"s","text":"Talk"}]}}})"),
             std::string(R"({"format":1,"npcs":{"n":{"template":"NPC_Workshop_Alchemist01","dialogue":"d"}},"dialogues":{"d":{"start":"a","nodes":{"a":{"text":"Hello"}}}}})"),
             std::string(R"({"format":1,"npcs":{"n":{"template":"NPC_Workshop_Alchemist01","clone":{}}}})"),
             std::string(R"({"format":1,"npcs":{"n":{"template":"NPC_Workshop_Alchemist01","roster":{}}}})"),
             std::string(R"({"format":1,"npcs":{"n":{"template":"NPC_Workshop_Alchemist01","dialogue":"alpha:d"}},"dialogues":{"d":{"native":false}}})"),
             std::string(R"({"format":1,"knowledge":{"met":{}},"quests":{"q":{"journal":"chat"}},"dialogues":{"d":{"native":false}}})")})
        for (bool unresolved : {false, true}) {
            start(unresolved ? "native input, unresolved ResourceInit: still stops" : "native input, hook refused: still stops");
            mod("alpha", input);
            mod("chat", kChatQuest);
            t_hook_ok = false;
            t_symbol = !unresolved;
            native_pack_install();
            native_pack_boot();
            native_pack_ready();
            CHECK(content_stop({"alpha"}, "assets/ResourceInit"));
            CHECK(t_puts.empty() && kiln::qb::t_compiles == 0);
            dump();
        }
}

// One mod's files that throw when read are that mod's error: it is left out and named, every other mod is read, built
// and served; a required boot participant is refused through kiln.boot by name (coordinator, 2026-11-15).
void one_mods_read() {
    start("one mod's files throw when read (a manifest field of the wrong type): only that mod is left out, named");
    mod("alpha", kGood);
    mod("beta", kGood, ",\"side\":5");  // nlohmann's value() throws on it
    mod("gamma", kGood);
    boot_and_resource_init();
    CHECK(t_stops.empty());
    CHECK(t_puts.size() == 2);
    CHECK(logged("native: beta: reading its files failed ([json.exception.type_error.302]"));
    CHECK(g_names.count("alpha:k1") == 1 && g_names.count("gamma:k1") == 1 && g_names.count("beta:k1") == 0);
    CHECK(kiln::qb::t_compiles == 1);
    dump();

    const auto prepare = [](const std::string& env, KilnBootPreparedV1* out, KilnBootIssueV1* issue) {
        KilnBootPrepareInputV1 in{sizeof in, KILN_BOOT_VERSION, 1, KilnStr{env.data(), env.size()}};
        return t_prepare && t_prepare(nullptr, &in, out, issue) == KILN_OK;
    };
    start("boot generation: an optional mod whose files throw is left out, the others are prepared");
    mod("alpha", kGood);
    mod("beta", kGood, ",\"side\":5");
    t_boot = true;
    native_pack_install();
    native_pack_boot();
    {
        KilnBootPreparedV1 out{sizeof out};
        KilnBootIssueV1 issue{sizeof issue};
        CHECK(prepare(R"({"owners":[{"id":"alpha","owner":11},{"id":"beta","owner":12}]})", &out, &issue));
        CHECK(out.ok == 1 && out.plan);
        const auto* plan = static_cast<const BootPlan*>(out.plan);
        CHECK(plan && plan->mods.size() == 1 && plan->mods[0].id == "alpha");
        CHECK(logged("native: beta: reading its files failed"));
        CHECK(t_stops.empty());
        if (out.ok && out.destroy) out.destroy(out.plan);
    }
    start("boot generation: a required mod whose files throw is refused through kiln.boot, by name");
    mod("alpha", kGood);
    mod("beta", kGood, ",\"side\":5");
    t_boot = true;
    native_pack_install();
    native_pack_boot();
    {
        KilnBootPreparedV1 out{sizeof out};
        KilnBootIssueV1 issue{sizeof issue};
        CHECK(prepare(R"({"owners":[{"id":"alpha","owner":11},{"id":"beta","owner":12,"required":true}]})", &out, &issue));
        CHECK(out.ok == 0 && std::string(issue.mod_id) == "beta" && std::string(issue.kind) == "knowledge");
        CHECK(std::string(issue.reason).find("reading its files failed") != std::string::npos);
        CHECK(t_stops.empty());
    }
    dump();
}

// A read that fails but is no one mod's (the switch throws after every mod was read): the whole read failed.
void whole_read() {
    start("lab switch read: the start stops");
    mod("alpha", kGood);
    lab_switch("read");
    boot_and_resource_init();
    CHECK(content_stop({"alpha"}, "lab switch kiln/control/knowledge.fail-build: read"));
    CHECK(logged("LAB SWITCH kiln/control/knowledge.fail-build"));
    dump();
}

void whole_compile() {
    start("the build throws: the start stops");
    mod("alpha", kGood);
    mod("broken", kBroken);
    kiln::qb::t_fake = kiln::qb::Fake::Throw;
    boot_and_resource_init();
    CHECK(content_stop({"alpha", "broken"}, "the build of the mods' native quests failed: the knowledge map is damaged"));
    CHECK(t_puts.empty());
    dump();
    start("lab switch compile: the start stops");
    mod("alpha", kGood);
    lab_switch("compile");
    boot_and_resource_init();
    CHECK(content_stop({"alpha"}, "lab switch kiln/control/knowledge.fail-build: compile"));
    dump();
}

void whole_shared() {
    start("the shared resources do not build (the manifest's failed): the start stops, naming the mods it was to serve");
    mod("alpha", kGood);
    mod("beta", kGood);
    mod("broken", kBroken);
    kiln::qb::t_fake = kiln::qb::Fake::Failed;
    boot_and_resource_init();
    CHECK(content_stop({"alpha", "beta"}, "two mods define the clone"));
    CHECK(t_puts.empty());
    dump();
    start("the game's GameKnowledgeResource is not there: the start stops");
    mod("alpha", kGood);
    t_game_knowledge = false;
    boot_and_resource_init();
    CHECK(content_stop({"alpha"}, "GameKnowledgeResource is not in the game's data"));
    dump();
    start("lab switch shared: the build cannot read the game's knowledge resource, the start stops");
    mod("alpha", kGood);
    lab_switch("shared");
    boot_and_resource_init();
    CHECK(content_stop({"alpha"}, "GameKnowledgeResource is not in the game's data"));
    dump();
}

void whole_assets() {
    start("kiln.assets refuses the built pack: the start stops");
    mod("alpha", kGood);
    t_put = KILN_ERR_UNAVAILABLE;
    boot_and_resource_init();
    CHECK(content_stop({"alpha"}, "kiln.assets refused the built GameKnowledgeResource"));
    dump();
    start("kiln.assets refuses a built template: the start stops");
    mod("alpha", R"({"format":1,"knowledge":{"met":{}},"template":"Kiln_alpha_warden"})");
    t_put_templates = KILN_ERR_UNAVAILABLE;
    boot_and_resource_init();
    CHECK(content_stop({"alpha"}, "kiln.assets refused the built template Kiln_alpha_warden"));
    dump();
    start("lab switch assets: the start stops");
    mod("alpha", kGood);
    lab_switch("assets");
    boot_and_resource_init();
    CHECK(content_stop({"alpha"}, "kiln.assets refused the built GameKnowledgeResource"));
    CHECK(t_puts.empty());
    dump();
    start("kiln.assets is not there at the resource start: the start stops");
    mod("alpha", kGood);
    t_assets = false;
    boot_and_resource_init();
    CHECK(content_stop({"alpha"}, "kiln.assets is not available"));
    dump();
}

void whole_hook() {
    start("the hook on ResourceInit does not go in: the start stops at on_ready");
    mod("alpha", kGood);
    t_hook_ok = false;
    native_pack_install();
    native_pack_boot();
    native_pack_ready();
    CHECK(content_stop({"alpha"}, "assets/ResourceInit"));
    CHECK(logged("hook on ResourceInit failed"));
    dump();
    start("assets/ResourceInit is not resolved: the start stops");
    mod("alpha", kGood);
    t_symbol = false;
    native_pack_install();
    native_pack_boot();
    native_pack_ready();
    CHECK(content_stop({"alpha"}, "assets/ResourceInit is not resolved"));
    dump();
    start("lab switch hook: the start stops");
    mod("alpha", kGood);
    lab_switch("hook");
    native_pack_install();
    native_pack_boot();
    native_pack_ready();
    CHECK(content_stop({"alpha"}, "lab switch kiln/control/knowledge.fail-build: hook"));
    dump();
    start("the hook not in, but a game client whose resource service is up: built at on_ready, no stop");
    mod("alpha", kGood);
    t_client = true;
    t_service_up = true;
    t_hook_ok = false;
    native_pack_install();
    native_pack_boot();
    native_pack_ready();
    CHECK(t_stops.empty() && t_puts.size() == 2);
    dump();
}

void late() {
    start("lab switch late: the content is served, then a stop is asked for after the world's first step");
    mod("alpha", kGood);
    lab_switch("late");
    boot_and_resource_init();
    CHECK(t_stops.empty() && t_puts.size() == 2);
    CHECK(t_post != nullptr);
    for (int i = 0; t_post && i < 30; ++i) t_post(nullptr, t_post_user);
    CHECK(t_stops.empty());  // not in the world's first second
    for (int i = 0; t_post && i < 5; ++i) t_post(nullptr, t_post_user);  // once only
    CHECK(content_stop({"alpha"}, "lab switch kiln/control/knowledge.fail-build: late"));
    dump();
}

// A boot generation: built at ResourceInit from the published plan, or in the prepare (the resource service up), and a
// prepare that knows the hook is not there.
void boot_generation() {
    static kiln::qb::ModInput good{"alpha", {kiln::qb::ojson::parse(kGood)}, {"alpha.json"}, {}, ""};
    static kiln::qb::ModInput req{"req", {kiln::qb::ojson::parse(kGood)}, {"req.json"}, {}, ""};
    start("boot generation, the build throws at ResourceInit: the start stops, then a required mod's fault");
    t_boot = true;
    native_pack_install();
    native_pack_boot();
    CHECK(g_boot && t_prepare);
    auto* plan = new BootPlan;
    plan->mods = {good, req};
    plan->required = {"req"};
    t_plan = plan;
    native_pack_ready();
    t_service_up = true;
    kiln::qb::t_fake = kiln::qb::Fake::Throw;
    build_once();
    CHECK(content_stop({"alpha", "req"}, "the build of the mods' native quests failed"));
    CHECK(t_order.size() == 2 && t_order[0] == "stop:content" && t_order[1] == "fault:req");
    dump();
    delete plan;

    start("boot generation, kiln.assets refuses at ResourceInit: the start stops");
    t_boot = true;
    native_pack_install();
    native_pack_boot();
    plan = new BootPlan;
    plan->mods = {good};
    t_plan = plan;
    native_pack_ready();
    t_service_up = true;
    t_put = KILN_ERR_UNAVAILABLE;
    build_once();
    CHECK(content_stop({"alpha"}, "kiln.assets refused the built GameKnowledgeResource"));
    dump();
    delete plan;

    const std::string env = R"({"owners":[{"id":"alpha","owner":11},{"id":"req","owner":12,"required":true}]})";
    KilnBootPrepareInputV1 in{sizeof in, KILN_BOOT_VERSION, 1, KilnStr{env.data(), env.size()}};
    start("boot generation, the hook not in: the prepare stops the start");
    mod("alpha", kGood);
    t_boot = true;
    t_hook_ok = false;
    native_pack_install();
    native_pack_boot();
    {
        KilnBootPreparedV1 out{sizeof out};
        KilnBootIssueV1 issue{sizeof issue};
        CHECK(t_prepare && t_prepare(nullptr, &in, &out, &issue) == KILN_OK);
        CHECK(content_stop({"alpha"}, "assets/ResourceInit"));
        if (out.ok && out.destroy) out.destroy(out.plan);
    }
    dump();

    start("boot generation, the resource service up at the prepare, the build throws: the start stops in the prepare");
    mod("alpha", kGood);
    mod("req", kGood);
    t_boot = true;
    t_client = true;
    t_service_up = true;
    native_pack_install();
    native_pack_boot();
    kiln::qb::t_fake = kiln::qb::Fake::Throw;
    {
        KilnBootPreparedV1 out{sizeof out};
        KilnBootIssueV1 issue{sizeof issue};
        CHECK(t_prepare && t_prepare(nullptr, &in, &out, &issue) == KILN_OK);
        CHECK(content_stop({"alpha", "req"}, "the build of the mods' native quests failed"));
        CHECK(out.ok == 0 && std::string(issue.mod_id) == "req");  // went on: the required mod is refused as before
        if (out.ok && out.destroy) out.destroy(out.plan);
    }
    dump();

    start("boot generation, one mod's own errors: no stop, a required broken mod is refused as before");
    mod("alpha", kGood);
    mod("req", kBroken);
    t_boot = true;
    t_client = true;
    t_service_up = true;
    native_pack_install();
    native_pack_boot();
    {
        KilnBootPreparedV1 out{sizeof out};
        KilnBootIssueV1 issue{sizeof issue};
        CHECK(t_prepare && t_prepare(nullptr, &in, &out, &issue) == KILN_OK);
        CHECK(t_stops.empty());
        CHECK(out.ok == 0 && std::string(issue.mod_id) == "req");
        if (out.ok && out.destroy) out.destroy(out.plan);
    }
    dump();
}

// The texts (start_stop.h): the roster's word for word as 17c shipped them; the content stop's.
void texts() {
    std::printf("-- the texts of both stops\n");
    const std::vector<std::string> mods{"a", "b"};
    const std::string why = "the roster holds at most 128 NPCs";
    const std::string roster_advice =
        "Do not restart the game or the server: the next start stops the same way (a dedicated server under Wine exits with "
        "status 2; a supervisor must not restart on it). Fix the mod list: remove these mods, or run a Kiln and game build "
        "where the roster's second unlock page installs";
    CHECK(stop::advice(StartStop::Roster) == roster_advice);
    CHECK(stop::head(StartStop::Roster, 412) == "roster: the start is stopped (pid 412, exit code 0x4B02): ");
    CHECK(stop::line(StartStop::Roster, why, mods) ==
          why + "; mods: a, b. No world was loaded and no save was opened: a world without these mods' NPCs and knowledge "
                "would drop their progress at its next save. " + roster_advice + " (kiln/state/roster-refused.json)");
    CHECK(stop::reason(StartStop::Roster, why, mods) == "kiln.knowledge: " + why + "; mods: a, b. " + roster_advice);
    CHECK(stop::box(StartStop::Roster, why, {}).find("Mods: (none named)") != std::string::npos);
    CHECK(std::string(stop::title(StartStop::Roster)) == "Kiln: the NPC roster is too long");
    CHECK(stop::kind(StartStop::Roster).exit_code == 0x4B02 && stop::unix_status(StartStop::Roster) == 2);
    CHECK(std::string(stop::kind(StartStop::Roster).state) == "roster-refused.json");

    const std::string cwhy = "the build of the mods' native quests failed: boom";
    CHECK(stop::kind(StartStop::Content).exit_code == 0x4B03 && stop::unix_status(StartStop::Content) == 3);
    CHECK(KILN_KNOW_CONTENT_EXIT_CODE == 0x4B03u && (KILN_KNOW_CONTENT_EXIT_CODE & 0xff) != (KILN_KNOW_ROSTER_EXIT_CODE & 0xff));
    CHECK(std::string(stop::kind(StartStop::Content).state) == "content-refused.json");
    CHECK(stop::head(StartStop::Content, 9) == "native: the start is stopped (pid 9, exit code 0x4B03): ");
    const std::string line = stop::line(StartStop::Content, cwhy, mods);
    for (const char* part : {"could not be built at all (the build of the mods' native quests failed: boom); mods: a, b.",
                             "No world was loaded and no save was opened", "Remove or update the mods named here",
                             "send kiln/logs/kiln.log", "Do not restart the game or the server in a loop",
                             "exits with status 3", "(kiln/state/content-refused.json)"})
        CHECK(line.find(part) != std::string::npos);
    CHECK(line.find("second unlock page") == std::string::npos);  // not the roster's advice
    CHECK(stop::reason(StartStop::Content, cwhy, mods).rfind("kiln.knowledge: ", 0) == 0);
    CHECK(stop::reason(StartStop::Content, cwhy, mods).find("mods: a, b.") != std::string::npos);
    const std::string box = stop::box(StartStop::Content, cwhy, mods);
    CHECK(box.find(cwhy) != std::string::npos && box.find("Mods: a, b") != std::string::npos &&
          box.find("remove or update these mods") != std::string::npos && box.find("60 seconds") != std::string::npos);
    CHECK(stop::cannot_stop(StartStop::Content, cwhy, mods).find("could NOT be stopped") != std::string::npos);
    CHECK(stop::cannot_stop(StartStop::Content, cwhy, mods).find("mods: a, b") != std::string::npos);
    CHECK(stop::cannot_stop(StartStop::Roster, why, mods).rfind("roster: the start could NOT be stopped", 0) == 0);
    const nlohmann::json j = stop::state(StartStop::Content, cwhy, mods, false, true, 9, "2026-11-15T12:00:00Z");
    CHECK(j["format"] == 1 && j["refused"] == true && j["why"] == cwhy && j["mods"] == mods && j["side"] == "server" &&
          j["exitCode"] == 19203 && j["unixExitStatus"] == 3 && j["restart"] == false && j["bootStop"] == true && j["pid"] == 9 &&
          j["advice"] == stop::advice(StartStop::Content) && j.size() == 12);
    const nlohmann::json r = stop::state(StartStop::Roster, why, mods, true, false, 9, "u");
    CHECK(r["exitCode"] == 19202 && r["unixExitStatus"] == 2 && r["side"] == "client" && r["bootStop"] == false);
}

int main(int argc, char** argv) {
    t_root = fs::path(argc > 1 ? argv[1] : "/tmp") / "native-pack-test";
    g_api = &t_api;
    served_and_single_mod_errors();
    empty_input_never_stops();
    non_native_hook_failures();
    native_hook_controls();
    one_mods_read();
    whole_read();
    whole_compile();
    whole_shared();
    whole_assets();
    whole_hook();
    late();
    boot_generation();
    texts();
    std::error_code ec;
    fs::remove_all(t_root, ec);
    std::printf("NATIVE PACK TESTS %d checks, %d FAIL\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
