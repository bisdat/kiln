// SPDX-License-Identifier: MIT
// kiln.routines on the host (no game, no Wine).
//  - plan: the wire document of format 1 (the golden text and hash are the constants Kiln.Blocks.Tests asserts
//    Kiln.Blocks writes and Kiln.TestKit.Tests feeds the kit's model), what is refused, which window a minute takes;
//  - hooks: the engine against a stubbed villager worker. The stub game keeps the facts the hooks rely on: SetState
//    runs the old state's exit and the new one's enter at once (through the state's handler, DecideTask included),
//    leaves previousState alone when it leaves FinishTask, and SetDecision clears the failure count;
//  - state: the change queue, one completion per change, the events.
//   native/services/routines/tests/run.sh
#include <atomic>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <thread>
#include <vector>

#include "../engine.h"
#include "../plan.h"
#include "../state.h"

using namespace kroutines;

namespace {

int g_fail = 0, g_pass = 0;
#define CHECK(cond)                                                     \
    do {                                                                \
        if (cond) ++g_pass;                                             \
        else {                                                          \
            ++g_fail;                                                   \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        }                                                               \
    } while (0)
#define CHECK_EQ(a, b)                                                                                      \
    do {                                                                                                    \
        if ((a) == (b)) ++g_pass;                                                                           \
        else {                                                                                              \
            ++g_fail;                                                                                       \
            std::printf("FAIL %s:%d: %s == %s (%lld vs %lld)\n", __FILE__, __LINE__, #a, #b, static_cast<long long>(a), \
                        static_cast<long long>(b));                                                         \
        }                                                                                                   \
    } while (0)

const char kGoldenHash[] = "02dcb72ca219f3a72202cede3638112dda4aa5a71948b89543c43f667c4d3ebb";
const char kGoldenWire[] =
    R"({"format":1,"key":"routine-test:day","planVersion":2,"contentHash":"02dcb72ca219f3a72202cede3638112dda4aa5a71948b89543c43f667c4d3ebb","windows":[{"startMinute":360,"endMinute":720,"allDay":false,"priority":0,"task":{"kind":"inspect"}},{"startMinute":1320,"endMinute":360,"allDay":false,"priority":5,"task":{"kind":"sleep"}},{"startMinute":0,"endMinute":0,"allDay":true,"priority":-1,"task":{"kind":"sit"}}],"fallback":{"kind":"roam"}})";

std::string replaced(std::string s, const std::string& what, const std::string& with) {
    const size_t at = s.find(what);
    if (at == std::string::npos) {
        std::printf("FAIL replaced(): \"%s\" is not in the text\n", what.c_str());
        ++g_fail;
        return s;
    }
    return s.replace(at, what.size(), with);
}

// A wire document for a plan the test writes itself.
std::string wire(const std::string& key, Kind fallback, std::vector<Window> windows = {}, uint32_t version = 1) {
    std::string s = "{\"format\":1,\"key\":\"" + key + "\",\"planVersion\":" + std::to_string(version) + ",\"contentHash\":\"" +
                    sha256_hex(canonical(version, windows, fallback)) + "\",\"windows\":[";
    for (size_t i = 0; i < windows.size(); ++i) {
        const Window& w = windows[i];
        if (i) s += ',';
        s += "{\"startMinute\":" + std::to_string(w.start) + ",\"endMinute\":" + std::to_string(w.end) + ",\"allDay\":" +
             (w.all_day ? "true" : "false") + ",\"priority\":" + std::to_string(w.priority) + ",\"task\":{\"kind\":\"" + kind_name(w.task) + "\"}}";
    }
    return s + "],\"fallback\":{\"kind\":\"" + kind_name(fallback) + "\"}}";
}

Window all_day(Kind k) { return Window{0, 0, true, 0, k}; }

std::shared_ptr<const Plan> plan_of(const std::string& text) {
    auto p = std::make_shared<Plan>();
    std::string problem;
    if (!parse_plan(text, *p, problem)) {
        std::printf("FAIL plan_of: %s\n", problem.c_str());
        ++g_fail;
    }
    return p;
}

// ---------------------------------------------------------------------------------------------------- plan ----

void test_sha256() {
    CHECK(sha256_hex("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    CHECK(sha256_hex("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK(sha256_hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq") == "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    CHECK(sha256_hex(std::string(1000000, 'a')) == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
    // every tail length around the block and padding edges gives 64 hex digits and differs from its neighbour
    std::string last;
    for (size_t n = 54; n <= 66; ++n) {
        const std::string h = sha256_hex(std::string(n, 'x'));
        CHECK(h.size() == 64 && h != last);
        last = h;
    }
}

void test_plan_golden() {
    Plan p;
    std::string problem;
    CHECK(parse_plan(kGoldenWire, p, problem));
    CHECK(problem.empty());
    CHECK(p.key == "routine-test:day" && p.mod == "routine-test" && p.version == 2 && p.hash == kGoldenHash);
    CHECK_EQ(p.windows.size(), 3u);
    CHECK(p.windows[1].start == 1320 && p.windows[1].end == 360 && !p.windows[1].all_day && p.windows[1].priority == 5 &&
          p.windows[1].task == Kind::Sleep);
    CHECK(p.windows[2].all_day && p.windows[2].priority == -1 && p.windows[2].task == Kind::Sit);
    CHECK(p.fallback == Kind::Roam);
    // any property order, and whitespace between tokens
    const char shuffled[] =
        R"( { "fallback" : {"kind":"roam"}, "windows":[ {"task":{"kind":"inspect"},"priority":0,"allDay":false,"endMinute":720,"startMinute":360},{"startMinute":1320,"endMinute":360,"allDay":false,"priority":5,"task":{"kind":"sleep"}},{"startMinute":0,"endMinute":0,"allDay":true,"priority":-1,"task":{"kind":"sit"}}],"contentHash":"02dcb72ca219f3a72202cede3638112dda4aa5a71948b89543c43f667c4d3ebb","planVersion":2,"key":"routine-test:day","format":1 } )";
    Plan q;
    CHECK(parse_plan(shuffled, q, problem));
    CHECK(q.hash == kGoldenHash && q.windows.size() == 3);
    // a plan with no window at all
    Plan e;
    CHECK(parse_plan(wire("m:empty", Kind::Vanilla), e, problem));
    CHECK(e.windows.empty() && e.select(100) == -1 && e.task_at(-1) == Kind::Vanilla);
}

void refused(const std::string& text, const char* says) {
    Plan p;
    p.key = "untouched";
    std::string problem;
    const bool ok = parse_plan(text, p, problem);
    if (ok || problem.find(says) == std::string::npos) {
        ++g_fail;
        std::printf("FAIL refused(\"%s\"): %s\n", says, ok ? "accepted" : problem.c_str());
    } else ++g_pass;
    CHECK(p.key == "untouched");  // a refused document leaves the output alone
}

void test_plan_refusals() {
    const std::string g = kGoldenWire;
    refused(replaced(g, "\"priority\":5", "\"priority\":6"), "contentHash");
    refused(replaced(g, "\"format\":1", "\"format\":2"), "format 2");
    refused(replaced(g, "\"format\":1,", "\"format\":1,\"owner\":\"x\","), "owner");
    refused(replaced(g, "\"format\":1,", "\"format\":1,\"format\":1,"), "twice");
    refused(replaced(g, "\"format\":1,", ""), "all needed");
    refused(replaced(g, "{\"kind\":\"inspect\"}", "{\"kind\":\"inspect\",\"duration\":5}"), "nothing else");
    refused(replaced(g, "{\"kind\":\"inspect\"}", "{\"duration\":5}"), "not part of a task");
    refused(replaced(g, "{\"kind\":\"inspect\"}", "{\"kind\":\"goTo\"}"), "not a task of this version");
    refused(replaced(g, "{\"kind\":\"inspect\"}", "{\"kind\":\"job\"}"), "not a task of this version");
    refused(replaced(g, "\"priority\":0,", ""), "all needed");
    refused(replaced(g, "\"priority\":0,", "\"priority\":0,\"priority\":0,"), "twice");
    refused(replaced(g, "\"priority\":0,", "\"priority\":0,\"radius\":3,"), "not part of a window");
    refused(replaced(g, "\"startMinute\":360", "\"startMinute\":360.5"), "whole number");
    refused(replaced(g, "\"startMinute\":360", "\"startMinute\":3e2"), "whole number");
    refused(replaced(g, "\"startMinute\":360", "\"startMinute\":0360"), "leading zero");
    refused(replaced(g, "\"startMinute\":360", "\"startMinute\":\"360\""), "whole number");
    refused(replaced(g, "\"allDay\":false", "\"allDay\":0"), "true or false");
    refused(replaced(g, "routine-test:day", "day"), "<mod>:<name>");
    refused(replaced(g, "routine-test:day", ":day"), "<mod>:<name>");
    refused(replaced(g, "routine-test:day", "routine-test:"), "<mod>:<name>");
    refused(replaced(g, "routine-test:day", "routine test:day"), "<mod>:<name>");
    refused(replaced(g, "routine-test:day", "a:b:c"), "<mod>:<name>");
    refused(replaced(g, "\"planVersion\":2", "\"planVersion\":-1"), "32 bits");
    refused(replaced(g, "\"planVersion\":2", "\"planVersion\":4294967296"), "32 bits");
    refused(g + "x", "text after");
    refused("[" + g + "]", "expected");
    refused("{", "string expected");
    refused("", "expected");
    refused(std::string(KILN_ROUTINES_MAX_PLAN_BYTES + 1, ' '), "16384 bytes");
    // the numbers' rules, with a hash that fits the content: the rule refuses, not the hash
    refused(wire("m:p", Kind::Vanilla, {}, 0), "planVersion is 0");
    refused(wire("m:p", Kind::Sleep), "vanilla, idle or roam");
    refused(wire("m:p", Kind::Sit), "vanilla, idle or roam");
    refused(wire("m:p", Kind::Inspect), "vanilla, idle or roam");
    refused(wire("m:p", Kind::Idle, {Window{-1, 10, false, 0, Kind::Idle}}), "startMinute is outside");
    refused(wire("m:p", Kind::Idle, {Window{0, 1440, false, 0, Kind::Idle}}), "endMinute is outside");
    refused(wire("m:p", Kind::Idle, {Window{600, 600, false, 0, Kind::Idle}}), "same minute");
    refused(wire("m:p", Kind::Idle, {Window{0, 60, true, 0, Kind::Idle}}), "whole day");
    std::vector<Window> many;
    for (int i = 0; i < 65; ++i) many.push_back(Window{i, i + 1, false, 0, Kind::Idle});
    refused(wire("m:p", Kind::Idle, many), "more than 64 windows");
    many.pop_back();
    Plan p;
    std::string problem;
    CHECK(parse_plan(wire("m:p", Kind::Idle, many), p, problem));
    CHECK_EQ(p.windows.size(), 64u);
    CHECK(parse_plan(wire("m:p", Kind::Roam, {Window{1439, 0, false, INT32_MIN, Kind::Idle}}, UINT32_MAX), p, problem));
}

void test_select() {
    const auto p = plan_of(kGoldenWire);
    CHECK_EQ(p->select(400), 0);   // inspect (0) over the all-day sit (-1)
    CHECK_EQ(p->select(800), 2);   // only the all-day window
    CHECK_EQ(p->select(100), 1);   // sleep (5), over midnight
    CHECK_EQ(p->select(1320), 1);
    CHECK_EQ(p->select(1319), 2);
    CHECK_EQ(p->select(359), 1);
    CHECK_EQ(p->select(360), 0);   // end-exclusive: the sleep window ends where the inspect window starts
    CHECK_EQ(p->select(720), 2);
    const auto tie = plan_of(wire("m:tie", Kind::Vanilla, {Window{300, 900, false, 0, Kind::Roam}, Window{0, 600, false, 0, Kind::Idle},
                                                            Window{850, 900, false, 1, Kind::Sit}}));
    CHECK_EQ(tie->select(400), 0);  // equal priority: the first written
    CHECK_EQ(tie->select(100), 1);
    CHECK_EQ(tie->select(860), 2);  // the higher priority, though written last
    CHECK_EQ(tie->select(1000), -1);
    CHECK(tie->task_at(-1) == Kind::Vanilla && tie->task_at(2) == Kind::Sit);
    const int64_t minute = kNsPerMinute, day = 1440 * minute;
    CHECK_EQ(minute_of(0), 0);
    CHECK_EQ(minute_of(minute - 1), 0);
    CHECK_EQ(minute_of(minute), 1);
    CHECK_EQ(minute_of(day - 1), 1439);
    CHECK_EQ(minute_of(day), 0);
    CHECK_EQ(minute_of(7 * day + 22 * 60 * minute + 59'999'999'999), 1320);
    CHECK_EQ(minute_of(-1), 1439);
    CHECK_EQ(minute_of(-day), 0);
}

// ------------------------------------------------------------------------------------------------- the game ----

struct Call {
    char what;  // 'D' SetDecision, 'S' SetState, 'F' FinishState, 'V' the game's chooser, 'Z' the game's sleep check
    uint8_t arg;
    bool operator==(const Call&) const = default;
};

struct Villager;
Engine* g_engine = nullptr;

// One villager's worker context, with the records the hooks read at the offsets of the real one.
struct Villager {
    alignas(16) uint8_t ctx[0x500] = {};
    uint8_t dyn[136] = {};
    uint8_t common[1688] = {};
    uint8_t tmpl[456] = {};
    uint8_t time[0x80] = {};
    std::vector<Call> calls;
    std::vector<std::thread::id> threads;
    int vanilla = 0, vanilla_phase2 = 0;
    bool sleep_allowed = false;
    uint8_t vanilla_previous_seen = 0xff, previous_at_first_decision = 0xff;
    // what the game's own code does inside the worker this step
    void (*script)(Villager&) = nullptr;
    uint8_t script_phase = 1;
    // set: the enter handler of this state calls the chooser again (a nested dispatch)
    uint8_t reenter_on = 0xff;
    bool finish_from_decide = true;

    explicit Villager(uint32_t npc_id) {
        put(0x1b0, dyn);
        put(0x300, common);
        put(0x450, tmpl);
        put(0x148, time);
        std::memcpy(tmpl, &npc_id, 4);
        state() = native::DecideTask;
        previous() = native::DecideTask;
    }
    void put(uint32_t offset, void* p) { std::memcpy(ctx + offset, &p, sizeof p); }
    uint8_t& state() { return dyn[0]; }
    uint8_t& previous() { return dyn[1]; }
    uint8_t& decision() { return dyn[2]; }
    uint8_t& failures() { return common[0x68e]; }
    void now(int hour, int minute = 0) {
        const int64_t ns = (hour * 60LL + minute) * kNsPerMinute;
        std::memcpy(time + 0x48, &ns, 8);
    }
    uintptr_t dynamic() const { return reinterpret_cast<uintptr_t>(dyn); }
    static Villager& of(void* ctx) { return *reinterpret_cast<Villager*>(ctx); }
};
static_assert(offsetof(Villager, ctx) == 0);

// The state's handler, as the game's dispatch calls it.
void dispatch(Villager& v, uint8_t phase) {
    if (v.state() == native::DecideTask) g_engine->on_decide(v.ctx, phase);
}

// The game's transition table, as far as the tests walk it.
uint8_t target_of(uint8_t state, uint8_t condition, uint8_t decision) {
    if (condition == 1) return native::FinishTask;
    switch (state) {
        case native::DecideTask: return native::FindTarget;
        case native::FindTarget: return native::GoToTarget;
        case native::FindBed: return native::GoToTarget;
        case native::GoToTarget: return decision == native::WantsSit ? native::Sitting : decision == native::WantsSleep ? native::Sleep : native::Interacting;
        case native::FinishTask: return native::DecideTask;
        default: return native::FinishTask;
    }
}

void game_worker(void* ctx) {
    Villager& v = Villager::of(ctx);
    v.threads.push_back(std::this_thread::get_id());
    if (v.script) v.script(v);
}
void game_decide(void* ctx, uint8_t phase) {
    Villager& v = Villager::of(ctx);
    if (phase == native::PhaseExit) {
        ++v.vanilla_phase2;
        return;
    }
    ++v.vanilla;
    v.calls.push_back({'V', phase});
    v.vanilla_previous_seen = v.previous();
    // the game's own choice, here always Idle
    g_engine->game.set_decision(ctx, native::WantsIdle);
    g_engine->game.set_state(ctx, native::Idle);
}
void game_set_decision(void* ctx, uint8_t decision) {
    Villager& v = Villager::of(ctx);
    v.calls.push_back({'D', decision});
    if (v.previous_at_first_decision == 0xff) v.previous_at_first_decision = v.previous();
    v.failures() = 0;
    v.decision() = decision;
}
void game_set_state(void* ctx, uint8_t state) {
    Villager& v = Villager::of(ctx);
    v.calls.push_back({'S', state});
    if (v.state() == state) return;
    dispatch(v, native::PhaseExit);
    const uint8_t old = v.state();
    v.state() = state;
    if (old != native::FinishTask) v.previous() = old;
    dispatch(v, native::PhaseEnter);
    if (v.reenter_on == state) g_engine->on_decide(ctx, native::PhaseEnter);
}
bool game_finish_state(void* ctx, uint8_t condition) {
    Villager& v = Villager::of(ctx);
    v.calls.push_back({'F', condition});
    if (condition == 1) ++v.failures();
    if (v.state() == native::DecideTask && !v.finish_from_decide) return false;
    g_engine->game.set_state(ctx, target_of(v.state(), condition, v.decision()));
    return true;
}
bool game_sleep_check(void* ctx) {
    Villager& v = Villager::of(ctx);
    v.calls.push_back({'Z', 0});
    if (!v.sleep_allowed) return false;
    g_engine->game.set_decision(ctx, native::WantsSleep);
    g_engine->game.set_state(ctx, native::FindBed);
    return true;
}

// The entries: a call of the game's function goes through the service's own link first.
void entry_set_state(void* ctx, uint8_t state) { g_engine->on_set_state(ctx, state); }
bool entry_finish_state(void* ctx, uint8_t condition) { return g_engine->on_finish_state(ctx, condition); }

struct World {
    Engine engine;
    std::vector<std::unique_ptr<Runtime>> runtimes;
    std::vector<Bind> binds;
    uint64_t step = 0;

    World() {
        engine.layout = Layout{0x1b0, 0x300, 0x450, 0x148, 0x48, 0, 1, 2};
        engine.game = Game{&game_worker, &game_decide, &game_set_state, &game_finish_state, &game_set_decision, &entry_set_state,
                           &entry_finish_state, &game_sleep_check};
        g_engine = &engine;
    }
    Runtime& bind(Villager& v, const std::string& plan, bool active = true, uintptr_t dynamic = 1) {
        uint32_t npc;
        std::memcpy(&npc, v.tmpl, 4);
        runtimes.push_back(std::make_unique<Runtime>());
        binds.push_back(Bind{npc, dynamic == 1 ? v.dynamic() : dynamic, plan_of(plan), active, runtimes.back().get()});
        return *runtimes.back();
    }
    void pre() { engine.publish(binds, ++step); }
    // one worker update: the dispatch of the villager's state with `phase`
    void update(Villager& v, uint8_t phase = native::PhaseUpdate) {
        v.script_phase = phase;
        v.script = [](Villager& x) { dispatch(x, x.script_phase); };
        engine.on_worker(v.ctx);
    }
    void run(Villager& v, void (*script)(Villager&)) {
        v.script = script;
        engine.on_worker(v.ctx);
    }
};

std::vector<Call> calls(std::initializer_list<Call> c) { return c; }

struct Seen {
    TaskState from, to;
    Reason reason;
    Kind task;
    bool operator==(const Seen&) const = default;
};
std::vector<Seen> events(Runtime& rt) {
    std::vector<Seen> out;
    for (uint32_t i = 0; i < rt.n_events; ++i) out.push_back({rt.events[i].from, rt.events[i].to, rt.events[i].reason, rt.events[i].task});
    rt.n_events = 0;
    return out;
}

// -------------------------------------------------------------------------------------------------- hooks ----

void test_unowned_passes_through() {
    World w;
    Villager a(0x1111), b(0x2222);
    w.bind(a, wire("m:day", Kind::Idle, {all_day(Kind::Roam)}));
    // nothing published yet: the game's code
    w.update(a);
    CHECK_EQ(a.vanilla, 1);
    w.pre();
    // a villager without an assignment: every phase runs the game's code once, and nothing else is called
    for (uint8_t phase : {uint8_t{0}, uint8_t{1}, uint8_t{3}, uint8_t{4}}) {
        b.state() = native::DecideTask;
        b.calls.clear();
        w.update(b, phase);
        CHECK(b.calls == calls({{'V', phase}, {'D', native::WantsIdle}, {'S', native::Idle}}));
    }
    CHECK_EQ(b.vanilla, 4);
    b.state() = native::DecideTask;
    w.update(b, native::PhaseExit);
    CHECK_EQ(b.vanilla_phase2, 1 + 4);  // the four SetState calls above left DecideTask through its exit phase too
    CHECK_EQ(w.engine.stats.owned.load(), 0u);
    CHECK_EQ(w.engine.invocations(), 0u);
    // the hooks outside any worker call (another program calling the setters): the game's code
    Villager c(0x1111);
    w.engine.on_decide(c.ctx, 1);
    CHECK_EQ(c.vanilla, 1);
    w.engine.on_set_state(c.ctx, native::Roam);
    CHECK(w.engine.on_finish_state(c.ctx, 0));
    CHECK_EQ(w.runtimes[0]->n_events, 0u);
}

void test_stale_and_foreign() {
    World w;
    Villager a(0x1111);
    Runtime& rt = w.bind(a, wire("m:day", Kind::Idle, {all_day(Kind::Roam)}), true, a.dynamic() + 136);  // Pre saw another address
    w.pre();
    w.update(a);
    CHECK_EQ(a.vanilla, 1);
    CHECK_EQ(w.engine.stats.stale.load(), 1u);
    CHECK_EQ(rt.n_events, 0u);
    // the same villager in another world (a hosted game's client copy): the same NpcId, its own records
    w.binds[0].dynamic = a.dynamic();
    w.pre();
    Villager replica(0x1111);
    w.update(replica);
    CHECK_EQ(replica.vanilla, 1);
    CHECK_EQ(w.engine.stats.stale.load(), 2u);
    // and the real one is owned in the same step
    a.state() = native::DecideTask;
    w.update(a);
    CHECK_EQ(a.vanilla, 1);
    CHECK(a.state() == native::Roam);
    // a villager Pre did not find (address 0) is not in the list at all
    w.binds[0].dynamic = 0;
    w.pre();
    a.state() = native::DecideTask;
    w.update(a);
    CHECK_EQ(a.vanilla, 2);
    // the world stopped: nothing is owned
    w.binds[0].dynamic = a.dynamic();
    w.pre();
    w.engine.clear();
    CHECK(w.engine.empty());
    a.state() = native::DecideTask;
    w.update(a);
    CHECK_EQ(a.vanilla, 3);
    // a context without its records
    w.pre();
    Villager broken(0x1111);
    broken.put(0x450, nullptr);
    w.update(broken);
    CHECK_EQ(broken.vanilla, 1);
}

void test_tasks_use_the_games_setters() {
    struct Case {
        Kind kind;
        std::vector<Call> want;
        uint8_t state;
    };
    const Case cases[] = {
        {Kind::Idle, calls({{'D', native::WantsIdle}, {'S', native::Idle}}), native::Idle},
        {Kind::Roam, calls({{'D', native::WantsRoam}, {'S', native::Roam}}), native::Roam},
        {Kind::Sit, calls({{'D', native::WantsSit}, {'F', 0}, {'S', native::FindTarget}}), native::FindTarget},
        {Kind::Inspect, calls({{'D', native::WantsInteract}, {'F', 0}, {'S', native::FindTarget}}), native::FindTarget},
        {Kind::Vanilla, calls({{'V', 1}, {'D', native::WantsIdle}, {'S', native::Idle}}), native::Idle},
    };
    for (const Case& c : cases) {
        World w;
        Villager v(7);
        Runtime& rt = w.bind(v, wire("m:day", Kind::Idle, {all_day(c.kind)}));
        w.pre();
        v.failures() = 9;
        w.update(v);
        CHECK(v.calls == c.want);
        CHECK(v.state() == c.state && v.previous() == native::DecideTask);
        CHECK_EQ(v.failures(), 0);  // the game's SetDecision cleared them: the hooks wrote nothing themselves
        CHECK_EQ(v.vanilla, c.kind == Kind::Vanilla ? 1 : 0);
        CHECK(rt.task == c.kind && rt.window == 0 && rt.invocation == 1 && rt.native_state == c.state);
        CHECK(events(rt) == (std::vector<Seen>{{TaskState::None, TaskState::Chosen, Reason::None, c.kind},
                                                {TaskState::Chosen, TaskState::Started, Reason::None, c.kind}}));
    }
}

void test_phases() {
    World w;
    Villager v(7);
    Runtime& rt = w.bind(v, wire("m:day", Kind::Idle));
    w.pre();
    // 2: the game's (a no-op there), nothing chosen
    w.update(v, native::PhaseExit);
    CHECK_EQ(v.vanilla_phase2, 1);
    CHECK(v.calls.empty() && rt.invocation == 0);
    // 3: the pause is recorded; neither the game's chooser nor a choice
    w.update(v, native::PhasePause);
    CHECK(rt.paused && v.calls.empty() && rt.invocation == 0 && v.vanilla == 0);
    CHECK_EQ(w.engine.stats.paused.load(), 1u);
    // 4: the resume chooses
    w.update(v, native::PhaseResume);
    CHECK(!rt.paused && rt.invocation == 1 && v.state() == native::Idle);
    // 0 and 1 choose
    for (uint8_t phase : {uint8_t{0}, uint8_t{1}}) {
        World x;
        Villager y(7);
        Runtime& r = x.bind(y, wire("m:day", Kind::Roam));
        x.pre();
        x.update(y, phase);
        CHECK(r.invocation == 1 && y.state() == native::Roam && y.vanilla == 0);
    }
}

void test_suspended_runs_the_game() {
    World w;
    Villager v(7);
    Runtime& rt = w.bind(v, wire("m:day", Kind::Roam), false);
    w.pre();
    w.update(v);
    CHECK_EQ(v.vanilla, 1);
    CHECK(rt.invocation == 0 && rt.n_events == 0);
    CHECK_EQ(rt.native_state, native::Idle);  // still watched: the state the game took is known
}

void test_previous_state_survives_finish_task() {
    // Idle -> FinishTask -> DecideTask: the game leaves previousState at Idle when it leaves FinishTask, and the
    // chooser (the game's own weights look at it) must find it so. The plan's window is vanilla here, so the game's
    // chooser itself reports what it saw.
    World w;
    Villager v(7);
    Runtime& rt = w.bind(v, wire("m:day", Kind::Idle, {Window{0, 600, false, 0, Kind::Idle}, Window{600, 1200, false, 0, Kind::Vanilla}}));
    w.pre();
    v.now(1);
    w.update(v);
    CHECK(v.state() == native::Idle && v.previous() == native::DecideTask);
    w.run(v, [](Villager& x) { g_engine->game.finish_state(x.ctx, 0); });  // the idle is over
    CHECK(v.state() == native::FinishTask && v.previous() == native::Idle);
    CHECK(events(rt).back() == (Seen{TaskState::Started, TaskState::Completed, Reason::None, Kind::Idle}));
    v.now(11);
    v.calls.clear();
    v.previous_at_first_decision = 0xff;
    // FinishTask finishes: SetState(DecideTask) enters the chooser in phase 0, inside the game's own FinishState
    w.run(v, [](Villager& x) { g_engine->game.finish_state(x.ctx, 0); });
    CHECK_EQ(v.vanilla, 1);
    CHECK_EQ(v.vanilla_previous_seen, native::Idle);
    CHECK(rt.task == Kind::Vanilla && rt.window == 1);
    // and with a task of the plan: the first setter runs while previousState is still Idle
    World x;
    Villager y(7);
    x.bind(y, wire("m:day", Kind::Idle, {all_day(Kind::Roam)}));
    x.pre();
    y.state() = native::FinishTask;
    y.previous() = native::Idle;
    x.run(y, [](Villager& z) { g_engine->game.finish_state(z.ctx, 0); });
    CHECK_EQ(y.previous_at_first_decision, native::Idle);
    CHECK(y.state() == native::Roam && y.previous() == native::DecideTask);
    CHECK(y.calls == calls({{'F', 0}, {'S', native::DecideTask}, {'D', native::WantsRoam}, {'S', native::Roam}}));
}

void test_nested_set_state_does_not_choose_again() {
    World w;
    Villager v(7);
    Runtime& rt = w.bind(v, wire("m:day", Kind::Idle, {all_day(Kind::Roam)}));
    w.pre();
    v.reenter_on = native::Roam;  // Roam's enter handler dispatches the chooser again, inside the chooser's SetState
    w.update(v);
    CHECK_EQ(w.engine.stats.nested.load(), 1u);
    CHECK_EQ(w.engine.stats.decisions.load(), 1u);
    CHECK(rt.invocation == 1 && v.vanilla == 0);
    CHECK(v.calls == calls({{'D', native::WantsRoam}, {'S', native::Roam}}));
    // the exit phase of DecideTask ran inside that SetState: the game's no-op, not a choice
    CHECK_EQ(v.vanilla_phase2, 1);
    CHECK_EQ(events(rt).size(), 2u);
    // the next worker call starts clean: the flag was the call's, not the thread's for good
    v.reenter_on = 0xff;
    v.state() = native::DecideTask;
    w.update(v);
    CHECK_EQ(w.engine.stats.decisions.load(), 2u);
}

void test_sleep() {
    World w;
    Villager v(7);
    Runtime& rt = w.bind(v, wire("m:day", Kind::Idle, {all_day(Kind::Sleep)}));
    w.pre();
    // the game does not allow sleep now: refused, and the fallback is a task of its own
    w.update(v);
    CHECK(v.calls == calls({{'Z', 0}, {'D', native::WantsIdle}, {'S', native::Idle}}));
    CHECK(rt.task == Kind::Idle && rt.window == -1 && rt.invocation == 2);
    CHECK(events(rt) == (std::vector<Seen>{{TaskState::None, TaskState::Chosen, Reason::None, Kind::Sleep},
                                            {TaskState::Chosen, TaskState::Refused, Reason::SleepNotAllowed, Kind::Sleep},
                                            {TaskState::None, TaskState::Chosen, Reason::None, Kind::Idle},
                                            {TaskState::Chosen, TaskState::Started, Reason::None, Kind::Idle}}));
    // it does: its own check took FindBed; the hooks add no setter of their own
    v.state() = native::DecideTask;
    v.calls.clear();
    v.sleep_allowed = true;
    w.update(v);
    CHECK(v.calls == calls({{'Z', 0}, {'D', native::WantsSleep}, {'S', native::FindBed}}));
    CHECK(rt.task == Kind::Sleep && rt.window == 0 && rt.state == TaskState::Started && rt.native_state == native::FindBed);
    events(rt);
    // FindBed -> GoToTarget -> Sleep -> done: completed only when the sleep itself finishes
    w.run(v, [](Villager& x) { g_engine->game.finish_state(x.ctx, 0); });
    w.run(v, [](Villager& x) { g_engine->game.finish_state(x.ctx, 0); });
    CHECK(v.state() == native::Sleep && rt.state == TaskState::Started && rt.n_events == 0);
    w.run(v, [](Villager& x) { g_engine->game.finish_state(x.ctx, 0); });
    CHECK(events(rt) == (std::vector<Seen>{{TaskState::Started, TaskState::Completed, Reason::None, Kind::Sleep}}));
}

void test_task_tracking() {
    World w;
    Villager v(7);
    Runtime& rt = w.bind(v, wire("m:day", Kind::Idle, {all_day(Kind::Sit)}));
    w.pre();
    w.update(v);
    events(rt);
    // the target search fails: refused, and the next decision in this window takes the fallback once
    w.run(v, [](Villager& x) { g_engine->game.finish_state(x.ctx, 1); });
    CHECK(events(rt) == (std::vector<Seen>{{TaskState::Started, TaskState::Refused, Reason::TaskFailed, Kind::Sit}}));
    CHECK_EQ(v.failures(), 1);
    w.run(v, [](Villager& x) { g_engine->game.finish_state(x.ctx, 0); });  // FinishTask -> DecideTask, which decides
    CHECK(rt.task == Kind::Idle && rt.window == -1 && v.state() == native::Idle);
    events(rt);
    w.run(v, [](Villager& x) { g_engine->game.finish_state(x.ctx, 0); });
    CHECK(events(rt) == (std::vector<Seen>{{TaskState::Started, TaskState::Completed, Reason::None, Kind::Idle}}));
    w.run(v, [](Villager& x) { g_engine->game.finish_state(x.ctx, 0); });  // decides again: the window's task once more
    CHECK(rt.task == Kind::Sit && rt.window == 0 && v.state() == native::FindTarget);
    events(rt);
    // FindTarget and GoToTarget finish: progress, no event; Sitting finishes: completed
    w.run(v, [](Villager& x) { g_engine->game.finish_state(x.ctx, 0); });
    w.run(v, [](Villager& x) { g_engine->game.finish_state(x.ctx, 0); });
    CHECK(v.state() == native::Sitting && rt.n_events == 0 && rt.native_state == native::Sitting);
    w.run(v, [](Villager& x) { g_engine->game.finish_state(x.ctx, 0); });
    CHECK(events(rt) == (std::vector<Seen>{{TaskState::Started, TaskState::Completed, Reason::None, Kind::Sit}}));

    // a safety state takes the villager: suspended, never completed
    World x;
    Villager y(7);
    Runtime& r = x.bind(y, wire("m:day", Kind::Idle, {all_day(Kind::Inspect)}));
    x.pre();
    x.update(y);
    events(r);
    for (uint8_t safety : {native::AvoidWater, native::AvoidBuildingCursor, native::InsideWater}) {
        r.state = TaskState::Started;
        x.run(y, safety == native::AvoidWater    ? +[](Villager& z) { g_engine->game.set_state(z.ctx, native::AvoidWater); }
                 : safety == native::InsideWater ? +[](Villager& z) { g_engine->game.set_state(z.ctx, native::InsideWater); }
                                                 : +[](Villager& z) { g_engine->game.set_state(z.ctx, native::AvoidBuildingCursor); });
        CHECK(events(r) == (std::vector<Seen>{{TaskState::Started, TaskState::Suspended, Reason::Safety, Kind::Inspect}}));
        CHECK_EQ(r.native_state, safety);
    }
    // a finish in a state that is not the task's says nothing about the task
    r.state = TaskState::Started;
    x.run(y, [](Villager& z) { g_engine->game.finish_state(z.ctx, 0); });
    CHECK_EQ(r.n_events, 0u);

    // FinishState has nowhere to go from DecideTask: refused, the fallback next time
    World n;
    Villager m(7);
    Runtime& q = n.bind(m, wire("m:day", Kind::Roam, {all_day(Kind::Sit)}));
    n.pre();
    m.finish_from_decide = false;
    n.update(m);
    CHECK(events(q) == (std::vector<Seen>{{TaskState::None, TaskState::Chosen, Reason::None, Kind::Sit},
                                           {TaskState::Chosen, TaskState::Refused, Reason::NoTransition, Kind::Sit}}));
    CHECK(m.state() == native::DecideTask);
    n.update(m);
    CHECK(q.task == Kind::Roam && q.window == -1 && m.state() == native::Roam);

    // something else took the villager before the task ended (a command, the way home): the next decision closes it
    World i;
    Villager j(7);
    Runtime& k = i.bind(j, wire("m:day", Kind::Idle, {all_day(Kind::Roam)}));
    i.pre();
    i.update(j);
    events(k);
    j.state() = native::DecideTask;
    i.update(j);
    CHECK(events(k).front() == (Seen{TaskState::Started, TaskState::Suspended, Reason::Interrupted, Kind::Roam}));
    // the game's own choice has no end the hooks can name: the next decision is its end
    World e;
    Villager f(7);
    Runtime& g = e.bind(f, wire("m:day", Kind::Vanilla));
    e.pre();
    e.update(f);
    events(g);
    e.run(f, [](Villager& z) { g_engine->game.finish_state(z.ctx, 1); });
    CHECK_EQ(g.n_events, 0u);
    f.state() = native::DecideTask;
    e.update(f);
    CHECK(events(g).front() == (Seen{TaskState::Started, TaskState::Completed, Reason::None, Kind::Vanilla}));
}

void test_event_overflow_keeps_the_newest() {
    World w;
    Villager v(7);
    Runtime& rt = w.bind(v, wire("m:day", Kind::Idle));
    w.pre();
    for (int i = 0; i < 12; ++i) {
        v.state() = native::DecideTask;
        w.update(v);
    }
    // 12 decisions in one step: 2 + 11 * 3 changes
    CHECK_EQ(rt.n_events, Runtime::kEvents);
    CHECK_EQ(rt.dropped, 2u + 11 * 3 - Runtime::kEvents);
    CHECK(rt.events[Runtime::kEvents - 1].to == TaskState::Started && rt.events[Runtime::kEvents - 1].invocation == 12);
    CHECK(rt.state == TaskState::Started && rt.invocation == 12);
}

// Each villager on its own thread, many steps; the list is published again between the steps, as Pre does.
void test_threads() {
    constexpr int kVillagers = 8, kSteps = 300;
    World w;
    std::vector<std::unique_ptr<Villager>> all;
    std::vector<Runtime*> rts;
    for (int i = 0; i < kVillagers; ++i) {
        all.push_back(std::make_unique<Villager>(100 + i));
        rts.push_back(&w.bind(*all.back(), wire("m:day", i % 2 ? Kind::Roam : Kind::Idle, {Window{0, 720, false, 0, i % 2 ? Kind::Idle : Kind::Sit}})));
    }
    Villager stranger(999);
    for (int step = 0; step < kSteps; ++step) {
        w.pre();
        std::vector<std::thread> jobs;
        for (int i = 0; i < kVillagers; ++i)
            jobs.emplace_back([&, i] {
                Villager& v = *all[i];
                v.now(step % 24);
                v.state() = native::DecideTask;
                v.script_phase = 1;
                v.script = [](Villager& x) { dispatch(x, x.script_phase); };
                w.engine.on_worker(v.ctx);
            });
        jobs.emplace_back([&] {
            stranger.state() = native::DecideTask;
            stranger.script_phase = 1;
            stranger.script = [](Villager& x) { dispatch(x, x.script_phase); };
            w.engine.on_worker(stranger.ctx);
        });
        for (auto& j : jobs) j.join();
        for (Runtime* rt : rts) rt->n_events = 0, rt->dropped = 0;  // Post
    }
    uint64_t decisions = 0;
    for (int i = 0; i < kVillagers; ++i) {
        CHECK_EQ(all[i]->vanilla, 0);
        CHECK_EQ(rts[i]->decisions, static_cast<uint64_t>(kSteps));
        decisions += rts[i]->decisions;
        // each villager's own plan decided, whatever ran beside it
        const Kind last = rts[i]->task;
        CHECK(last == (i % 2 ? Kind::Idle : Kind::Sit));  // the last step is hour 11: inside the window
    }
    CHECK_EQ(stranger.vanilla, kSteps);
    CHECK_EQ(w.engine.invocations(), decisions);
    CHECK_EQ(w.engine.stats.owned.load(), static_cast<uint64_t>(kVillagers) * kSteps);
    std::printf("threads: %d villagers on their own threads for %d steps, %llu decisions\n", kVillagers, kSteps,
                static_cast<unsigned long long>(decisions));
}

}  // namespace

void test_state();  // state_test part, below

int main(int argc, char** argv) {
    if (argc > 1 && !std::strcmp(argv[1], "threads")) {
        test_threads();
        std::printf("kiln.routines (threads): %d passed, %d failed\n", g_pass, g_fail);
        return g_fail ? 1 : 0;
    }
    test_sha256();
    test_plan_golden();
    test_plan_refusals();
    test_select();
    test_unowned_passes_through();
    test_stale_and_foreign();
    test_tasks_use_the_games_setters();
    test_phases();
    test_suspended_runs_the_game();
    test_previous_state_survives_finish_task();
    test_nested_set_state_does_not_choose_again();
    test_sleep();
    test_task_tracking();
    test_event_overflow_keeps_the_newest();
    test_threads();
    test_state();
    std::printf("kiln.routines: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}

// -------------------------------------------------------------------------------------------------- state ----
namespace {

void done(const KilnRoutineResult*, void*) {}

bool has(const std::string& text, const char* part) { return text.find(part) != std::string::npos; }

struct Service {
    World world;
    Book book{world.engine};
    std::map<uint32_t, uintptr_t> where;
    uint64_t step = 0;
    std::vector<Completion> completions;
    std::vector<std::string> events;

    uint64_t assign(KilnOwner owner, uint32_t npc, const std::string& text, KilnResult want = KILN_OK, const char* says = nullptr) {
        uint64_t ticket = 77;
        std::string problem;
        const KilnResult rc = book.assign(owner, npc, text, &done, nullptr, &ticket, &problem);
        CHECK_EQ(rc, want);
        if (want != KILN_OK) CHECK_EQ(ticket, 0u);
        if (says && !has(problem, says)) {
            ++g_fail;
            std::printf("FAIL assign says \"%s\", wanted \"%s\"\n", problem.c_str(), says);
        }
        return ticket;
    }
    void pre() {
        book.pre(++step, [this](uint32_t npc) {
            auto it = where.find(npc);
            return it == where.end() ? uintptr_t{0} : it->second;
        });
    }
    void post() {
        book.post();
        for (auto& c : book.take_completions()) completions.push_back(std::move(c));
        for (auto& e : book.take_events()) events.push_back(std::move(e));
    }
    void tick() {
        pre();
        post();
    }
    std::string inspect(uint32_t npc) {
        std::string json;
        return book.inspect(npc, json) ? json : std::string();
    }
};

bool is(const Completion& c, uint64_t ticket, bool ok, uint32_t code, const char* reason, uint32_t npc, uint64_t revision) {
    return c.ticket == ticket && c.ok == ok && c.code == code && c.reason == reason && c.npc_id == npc && c.revision == revision && c.done == &done;
}

void test_one_completion_per_change() {
    Service s;
    const std::string day = wire("a:day", Kind::Idle, {all_day(Kind::Sit)});
    const uint64_t t = s.assign(1, 7, day);
    CHECK_EQ(t, 1u);
    CHECK(s.inspect(7).empty());  // queued, not committed
    CHECK_EQ(s.book.queued(), 1u);
    s.pre();
    // committed; the villager is not in the world: still ok, PendingSpawn
    CHECK(has(s.inspect(7), "\"assignment\":\"PendingSpawn\"") && has(s.inspect(7), "\"owner\":\"a\"") && has(s.inspect(7), "\"revision\":1"));
    s.post();
    CHECK_EQ(s.completions.size(), 1u);
    CHECK(is(s.completions[0], t, true, KILN_ROUTINE_OK, "", 7, 1));
    CHECK_EQ(s.events.size(), 1u);
    CHECK(has(s.events[0], "{\"type\":\"assignment\",\"before\":null,\"after\":{\"npcId\":7,\"owner\":\"a\",\"key\":\"a:day\",\"planVersion\":1,\"revision\":1,"
                           "\"assignment\":\"PendingSpawn\",\"windowIndex\":-1,\"invocation\":0,\"task\":null,\"taskState\":\"None\",\"nativeState\":0,"
                           "\"lastReason\":null,\"paused\":false},\"reason\":\"Assigned\",\"teardown\":false}"));
    s.tick();
    s.tick();
    CHECK_EQ(s.completions.size(), 1u);  // never a second time
    CHECK_EQ(s.events.size(), 1u);

    // a paused villager: the assignment completes ok, the pause shows in its snapshot
    Villager v(8);
    s.where[8] = v.dynamic();
    const uint64_t t2 = s.assign(1, 8, day);
    s.pre();
    s.world.update(v, native::PhasePause);
    s.post();
    CHECK(is(s.completions[1], t2, true, KILN_ROUTINE_OK, "", 8, 2));
    CHECK(has(s.inspect(8), "\"assignment\":\"Assigned\"") && has(s.inspect(8), "\"paused\":true") && has(s.inspect(8), "\"taskState\":\"None\""));
    CHECK_EQ(s.events.size(), 2u);

    // the task runs and the game fails it: task events only, no completion
    s.pre();
    s.world.update(v, native::PhaseResume);
    s.world.run(v, [](Villager& x) { g_engine->game.finish_state(x.ctx, 1); });
    s.post();
    CHECK_EQ(s.completions.size(), 2u);
    CHECK_EQ(s.events.size(), 5u);
    CHECK(s.events[2] == "{\"type\":\"task\",\"npcId\":8,\"key\":\"a:day\",\"planVersion\":1,\"revision\":2,\"invocation\":1,\"windowIndex\":0,"
                         "\"task\":\"sit\",\"from\":\"None\",\"to\":\"Chosen\",\"reason\":null,\"teardown\":false}");
    CHECK(has(s.events[3], "\"from\":\"Chosen\",\"to\":\"Started\""));
    CHECK(has(s.events[4], "\"from\":\"Started\",\"to\":\"Refused\",\"reason\":\"TaskFailed\""));
    CHECK(has(s.inspect(8), "\"taskState\":\"Refused\"") && has(s.inspect(8), "\"lastReason\":\"TaskFailed\"") &&
          has(s.inspect(8), "\"nativeState\":17") && has(s.inspect(8), "\"task\":\"sit\"") && has(s.inspect(8), "\"paused\":false"));
    // the villager leaves the world: pending again, without an event or a revision
    s.where.erase(8);
    s.tick();
    CHECK(has(s.inspect(8), "\"assignment\":\"PendingSpawn\"") && has(s.inspect(8), "\"revision\":2") && has(s.inspect(8), "\"nativeState\":0"));
    CHECK_EQ(s.events.size(), 5u);
}

void test_already_in_effect_and_owners() {
    Service s;
    uint64_t t = 0;
    // nothing assigned: remove is ok with no revision and no event; resume is a refusal
    CHECK_EQ(s.book.remove(1, 7, &done, nullptr, &t), KILN_OK);
    uint64_t t2 = 0;
    CHECK_EQ(s.book.resume(1, 7, &done, nullptr, &t2), KILN_OK);
    s.tick();
    CHECK(is(s.completions[0], t, true, KILN_ROUTINE_OK, "", 7, 0));
    CHECK(is(s.completions[1], t2, false, KILN_ROUTINE_NOT_ASSIGNED, "NotAssigned", 7, 0));
    CHECK(s.events.empty() && s.book.revision() == 0);
    // assigned and not suspended: resume is ok, the old revision, no event
    s.assign(1, 7, wire("a:day", Kind::Idle));
    s.tick();
    CHECK_EQ(s.book.resume(1, 7, &done, nullptr, &t), KILN_OK);
    s.tick();
    CHECK(is(s.completions[3], t, true, KILN_ROUTINE_OK, "", 7, 1));
    CHECK(s.events.size() == 1 && s.book.revision() == 1);

    // another mod: assign, remove and resume are refused with the owner's name; nothing is written
    const uint64_t a = s.assign(2, 7, wire("b:day", Kind::Roam));
    uint64_t r = 0, u = 0;
    CHECK_EQ(s.book.remove(2, 7, &done, nullptr, &r), KILN_OK);
    CHECK_EQ(s.book.resume(2, 7, &done, nullptr, &u), KILN_OK);
    s.tick();
    CHECK(is(s.completions[4], a, false, KILN_ROUTINE_OWNED_BY, "OwnedBy:a", 7, 1));
    CHECK(is(s.completions[5], r, false, KILN_ROUTINE_OWNED_BY, "OwnedBy:a", 7, 1));
    CHECK(is(s.completions[6], u, false, KILN_ROUTINE_OWNED_BY, "OwnedBy:a", 7, 1));
    CHECK(s.events.size() == 1 && s.book.revision() == 1 && has(s.inspect(7), "\"key\":\"a:day\""));

    // the owner replaces its plan, then removes it; the villager is free for the other mod in the same step
    const uint64_t rep = s.assign(1, 7, wire("a:night", Kind::Roam, {}, 3));
    s.tick();
    CHECK(is(s.completions[7], rep, true, KILN_ROUTINE_OK, "", 7, 2));
    CHECK(has(s.events[1], "\"reason\":\"Replaced\"") && has(s.events[1], "\"before\":{\"npcId\":7,\"owner\":\"a\",\"key\":\"a:day\"") &&
          has(s.events[1], "\"after\":{\"npcId\":7,\"owner\":\"a\",\"key\":\"a:night\",\"planVersion\":3,\"revision\":2"));
    CHECK_EQ(s.book.remove(1, 7, &done, nullptr, &r), KILN_OK);
    const uint64_t next = s.assign(2, 7, wire("b:day", Kind::Roam));
    s.tick();
    CHECK(is(s.completions[8], r, true, KILN_ROUTINE_OK, "", 7, 3));
    CHECK(is(s.completions[9], next, true, KILN_ROUTINE_OK, "", 7, 4));
    CHECK(has(s.events[2], "\"assignment\":\"Removed\"") && has(s.events[2], "\"reason\":\"Removed\"") && has(s.events[2], "\"revision\":3"));
    CHECK(has(s.events[3], "\"before\":null") && has(s.events[3], "\"owner\":\"b\""));
    // an earlier success stays a success when a later change of the same step removes it
    const uint64_t x = s.assign(1, 9, wire("a:day", Kind::Idle));
    CHECK_EQ(s.book.remove(1, 9, &done, nullptr, &r), KILN_OK);
    s.tick();
    CHECK(is(s.completions[10], x, true, KILN_ROUTINE_OK, "", 9, 5));
    CHECK(is(s.completions[11], r, true, KILN_ROUTINE_OK, "", 9, 6));
    CHECK(s.inspect(9).empty());
}

void test_bad_arguments_and_the_owners_mod() {
    Service s;
    const std::string day = wire("a:day", Kind::Idle);
    uint64_t t = 5;
    CHECK_EQ(s.book.assign(0, 7, day, &done, nullptr, &t), KILN_ERR_INVALID_ARGUMENT);
    CHECK_EQ(s.book.assign(1, 0, day, &done, nullptr, &t), KILN_ERR_INVALID_ARGUMENT);
    CHECK_EQ(s.book.assign(1, 7, day, nullptr, nullptr, &t), KILN_ERR_INVALID_ARGUMENT);
    CHECK_EQ(s.book.assign(1, 7, day, &done, nullptr, nullptr), KILN_ERR_INVALID_ARGUMENT);
    CHECK_EQ(s.book.remove(1, 7, nullptr, nullptr, &t), KILN_ERR_INVALID_ARGUMENT);
    CHECK_EQ(s.book.resume(0, 7, &done, nullptr, &t), KILN_ERR_INVALID_ARGUMENT);
    s.assign(1, 7, replaced(day, "\"format\":1", "\"format\":3"), KILN_ERR_INVALID_ARGUMENT, "format 3");
    CHECK_EQ(s.book.queued(), 0u);
    // a refused plan binds nothing: the owner can still become another mod's
    s.assign(1, 7, wire("b:day", Kind::Idle));
    // bound: this owner's plans are b's, and b's plans are this owner's
    s.assign(1, 8, day, KILN_ERR_INVALID_ARGUMENT, "not a plan of b");
    s.assign(2, 8, wire("b:night", Kind::Idle), KILN_ERR_INVALID_ARGUMENT, "another owner");
    s.assign(2, 8, wire("bb:night", Kind::Idle));  // a mod whose id only starts like b's is its own
    s.tick();
    CHECK_EQ(s.completions.size(), 2u);
    CHECK(s.events.size() == 2);
}

void test_queue_limit() {
    Service s;
    const std::string day = wire("a:day", Kind::Idle);
    for (uint32_t i = 1; i <= KILN_ROUTINES_MAX_QUEUED; ++i) s.assign(1, i, day);
    CHECK_EQ(s.book.queued(), 256u);
    s.assign(1, 1000, day, KILN_ERR_BUFFER_TOO_SMALL);
    uint64_t t = 9;
    CHECK_EQ(s.book.remove(1, 1, &done, nullptr, &t), KILN_ERR_BUFFER_TOO_SMALL);
    CHECK_EQ(t, 0u);
    s.tick();
    CHECK_EQ(s.completions.size(), 256u);  // every admitted change completed; the refused ones never do
    CHECK_EQ(s.events.size(), 256u);       // and no event was dropped
    bool all = true;
    for (size_t i = 0; i < s.completions.size(); ++i) all = all && s.completions[i].ok && s.completions[i].revision == i + 1;
    CHECK(all);
    CHECK(s.inspect(1000).empty());
    s.assign(1, 1000, day);  // free again after the step
    s.tick();
    CHECK(!s.inspect(1000).empty());
}

void test_revoke() {
    Service s;
    s.assign(1, 7, wire("a:day", Kind::Idle, {all_day(Kind::Roam)}));
    s.assign(2, 8, wire("b:day", Kind::Idle));
    Villager v(7);
    s.where[7] = v.dynamic();
    s.pre();
    s.world.update(v);  // a task is running when the owner goes
    s.post();
    const size_t events = s.events.size();
    const uint64_t queued = s.assign(1, 9, wire("a:day", Kind::Idle));
    const uint64_t others = s.assign(2, 10, wire("b:day", Kind::Idle));
    std::thread([&] { s.book.revoke_owner(1); }).join();  // any thread
    CHECK(!s.inspect(7).empty());                          // until the next Pre
    // the mod's next owner (a reload) can queue at once, also for the villager the old owner still holds
    const uint64_t again = s.assign(3, 7, wire("a:day", Kind::Roam));
    s.tick();
    CHECK(is(s.completions[2], queued, false, KILN_ROUTINE_REVOKED, "Revoked", 9, 0));
    CHECK(is(s.completions[3], others, true, KILN_ROUTINE_OK, "", 10, 4));
    CHECK(is(s.completions[4], again, true, KILN_ROUTINE_OK, "", 7, 5));
    CHECK(s.inspect(9).empty());
    CHECK(has(s.events[events], "\"type\":\"task\"") && has(s.events[events], "\"to\":\"Suspended\",\"reason\":\"Removed\""));
    CHECK(has(s.events[events + 1], "\"assignment\":\"Removed\"") && has(s.events[events + 1], "\"reason\":\"OwnerUnloaded\"") &&
          has(s.events[events + 1], "\"revision\":3"));
    CHECK(has(s.inspect(7), "\"revision\":5") && has(s.inspect(7), "\"taskState\":\"None\""));
    CHECK(!s.inspect(8).empty());
    // the old owner's villager is no longer chosen for by the old plan
    v.state() = native::DecideTask;
    s.pre();
    s.world.update(v);
    CHECK(v.state() == native::Roam);
    s.post();
}

void test_world_stop() {
    Service s;
    s.assign(1, 7, wire("a:day", Kind::Idle, {all_day(Kind::Roam)}));
    s.assign(1, 8, wire("a:day", Kind::Idle));
    Villager v(7);
    s.where[7] = v.dynamic();
    s.pre();
    s.world.update(v);
    s.post();
    const size_t events = s.events.size();
    const uint64_t queued = s.assign(1, 9, wire("a:day", Kind::Idle));
    s.book.stop();
    s.post();
    CHECK(is(s.completions.back(), queued, false, KILN_ROUTINE_WORLD_STOPPED, "WorldStopped", 9, 0));
    CHECK_EQ(s.events.size(), events + 3);
    CHECK(has(s.events[events], "\"type\":\"task\",\"npcId\":7") && has(s.events[events], "\"from\":\"Started\",\"to\":\"Suspended\",\"reason\":\"WorldStopped\",\"teardown\":true"));
    CHECK(has(s.events[events + 1], "\"before\":{\"npcId\":7") && has(s.events[events + 1], "\"assignment\":\"Assigned\"") &&
          has(s.events[events + 1], "\"assignment\":\"PendingSpawn\"") && has(s.events[events + 1], "\"reason\":\"WorldStopped\",\"teardown\":true") &&
          has(s.events[events + 1], "\"taskState\":\"Suspended\""));
    CHECK(has(s.events[events + 2], "\"after\":{\"npcId\":8") && has(s.events[events + 2], "\"teardown\":true"));
    // nothing of the world is kept: no assignment, no owned villager, revisions from 1 in the next world
    CHECK(s.inspect(7).empty() && s.book.assigned() == 0 && s.world.engine.empty());
    v.state() = native::DecideTask;
    s.world.update(v);
    CHECK_EQ(v.vanilla, 1);
    const size_t n = s.completions.size();
    s.book.stop();
    s.tick();
    CHECK_EQ(s.completions.size(), n);
    s.assign(1, 7, wire("a:day", Kind::Idle));
    s.tick();
    CHECK(s.completions.back().ok && s.completions.back().revision == 1);
}

void test_replace_closes_the_running_task() {
    Service s;
    s.assign(1, 7, wire("a:day", Kind::Idle, {all_day(Kind::Roam)}));
    Villager v(7);
    s.where[7] = v.dynamic();
    s.pre();
    s.world.update(v);
    s.post();
    const size_t events = s.events.size();
    s.assign(1, 7, wire("a:night", Kind::Idle));
    s.tick();
    CHECK(has(s.events[events], "\"key\":\"a:day\"") && has(s.events[events], "\"to\":\"Suspended\",\"reason\":\"Replaced\""));
    CHECK(has(s.events[events + 1], "\"reason\":\"Replaced\""));
    CHECK(has(s.inspect(7), "\"task\":null") && has(s.inspect(7), "\"windowIndex\":-1"));
    // the hooks run the new plan from this step on
    v.state() = native::DecideTask;
    s.pre();
    s.world.update(v);
    s.post();
    CHECK(v.state() == native::Idle && has(s.events.back(), "\"key\":\"a:night\""));
}

}  // namespace

void test_state() {
    test_one_completion_per_change();
    test_already_in_effect_and_owners();
    test_bad_arguments_and_the_owners_mod();
    test_queue_limit();
    test_revoke();
    test_world_stop();
    test_replace_closes_the_running_task();
}
