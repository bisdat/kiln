// SPDX-License-Identifier: MIT
// gate-priority-test (kit.h): the run file, the padding rule, and the loop. Threads:
//   - kiln_native_init: the game's main thread at phase boot;
//   - on_pre / on_post: the server world's tick thread (records the step's times and publishes its Pre and Post edges
//     for the loop's timing; never blocks, allocates or writes a file once the run is armed);
//   - on_world: the world's thread (STARTED) or the destroying thread (STOPPING);
//   - batch_body / batch_done: a kiln.tasks long-running slot. One batch at a time: each batch's done submits the next,
//     so everything of the loop (the target, the buffers, the files) is touched by one batch after another.
#include "kit.h"

#ifdef _WIN32
#include <windows.h>
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <memory>
#include <thread>

#include "services/kiln_caps.h"
#include "services/kiln_engine.h"
#include "services/kiln_players.h"
#include "services/kiln_saves.h"
#include "services/kiln_tasks.h"
#include "services/kiln_world.h"

namespace gpt {

int (*g_priority_hook)() = nullptr;
int current_thread_priority() {
    if (g_priority_hook) return g_priority_hook();
#ifdef _WIN32
    return GetThreadPriority(GetCurrentThread());
#else
    return 0;
#endif
}

// ---------------------------------------------------------------------------------------------- the run file ----
namespace {
std::string trim(std::string_view s) {
    const auto b = s.find_first_not_of(" \t\r\n"), e = s.find_last_not_of(" \t\r\n");
    return b == std::string_view::npos ? std::string() : std::string(s.substr(b, e - b + 1));
}
std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
    return s;
}
bool number(const std::string& v, long lo, long hi, long& out) {
    if (v.empty() || v.size() > 9 || v.find_first_not_of("0123456789") != std::string::npos) return false;
    out = std::strtol(v.c_str(), nullptr, 10);
    return out >= lo && out <= hi;
}
}  // namespace

RunConfig parse_run(std::string_view text) {
    RunConfig c;
    if (text.rfind("\xEF\xBB\xBF", 0) == 0) text.remove_prefix(3);
    size_t at = 0;
    while (at <= text.size()) {
        const size_t end = std::min(text.find('\n', at), text.size());
        std::string line = trim(text.substr(at, end - at));
        at = end + 1;
        if (line.empty() || line[0] == '#') continue;
        const auto eq = line.find('=');
        if (eq == std::string::npos) {
            c.warnings.push_back("a line without '=' was ignored: " + line.substr(0, 40));
            continue;
        }
        const std::string key = lower(trim(std::string_view(line).substr(0, eq)));
        const std::string value = trim(std::string_view(line).substr(eq + 1));
        long n = 0;
        if (key == "world") {
            std::string v = lower(value);
            v.erase(std::remove(v.begin(), v.end(), '-'), v.end());  // a GUID written with dashes is the same GUID
            if (v.size() == 32 && v.find_first_not_of("0123456789abcdef") == std::string::npos) c.world = v;
            else c.warnings.push_back("world= is not a GUID of 32 hex digits; no world is named");
        } else if (key == "component") {
            c.component = value;
        } else if (key == "offset") {
            if (number(value, 0, 65535, n)) c.offset = static_cast<int>(n);
            else c.warnings.push_back("offset= is not a byte offset 0..65535; ignored");
        } else if (key == "rate") {
            if (number(value, 1, 1000, n)) c.rate = static_cast<uint32_t>(n);
            else c.warnings.push_back("rate= is not 1..1000 pairs a second; 50 is used");
        } else if (key == "minutes") {
            if (number(value, 1, 120, n)) c.minutes = static_cast<uint32_t>(n);
            else c.warnings.push_back("minutes= is not 1..120; 15 is used");
        } else if (key == "seconds") {
            if (number(value, 1, 7200, n)) c.seconds = static_cast<uint32_t>(n);
            else c.warnings.push_back("seconds= is not 1..7200; minutes= is used");
        } else if (key == "warmup") {
            if (number(value, 0, 600, n)) c.warmup_s = static_cast<uint32_t>(n);
            else c.warnings.push_back("warmup= is not 0..600 seconds; 30 is used");
        } else if (key == "mode") {
            if (lower(value) == "noop") c.noop = true;
            else if (lower(value) != "write") c.warnings.push_back("mode= is not write or noop; write is used");
        } else if (key == "label") {
            std::string l;
            for (char ch : value)
                if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '.' || ch == '_' || ch == '-')
                    l += ch;
            c.label = l.substr(0, 48);
        } else {
            c.warnings.push_back("unknown key " + key.substr(0, 32) + " ignored");
        }
    }
    return c;
}

// ------------------------------------------------------------------------------------------- the padding rule ----
// A type's layout is explained when every byte is covered by a reflected member (a base class that is not empty, a
// field, an array element) or is a gap that alignment explains: the next member's offset is the end of the one before
// it rounded up to the next member's alignment, and the tail is the last member's end rounded up to the type's
// alignment. Those gaps are padding. Anything else (members that overlap, as in a union; a gap no alignment explains,
// where a member the reflection does not list, a bit-field among them, may live; a type without any reflected
// member; a kind that owns memory or points somewhere) leaves the component without padding we may use.
namespace {
struct TypeFacts {
    KilnTypeInfo info{};
    bool ok = false;
};
TypeFacts facts(const KilnTypesApi* t, const KilnType* type) {
    TypeFacts f;
    f.info.size = sizeof f.info;
    f.ok = type && t->type_info(type, &f.info) == KILN_OK;
    return f;
}
bool leaf(uint8_t kind) { return (kind >= KILN_TYPE_BOOL && kind <= KILN_TYPE_BITMASK64) || kind == KILN_TYPE_GUID; }
bool empty_struct(const KilnTypesApi* t, const KilnType* type, int depth) {
    const TypeFacts f = facts(t, type);
    // C++'s empty class: one byte, no member. A fieldless struct of more bytes holds something the reflection omits.
    if (!f.ok || f.info.kind != KILN_TYPE_STRUCT || f.info.field_count || f.info.type_size > 1 || depth > 32) return false;
    return !f.info.inner || empty_struct(t, f.info.inner, depth + 1);
}
uint64_t round_up(uint64_t v, uint64_t a) { return (v + a - 1) / a * a; }

// Appends the padding of `type` at `base` to `pads`; "" when the layout is explained, else why not.
std::string walk(const KilnTypesApi* t, const KilnType* type, uint64_t base, std::vector<uint32_t>& pads, int depth) {
    const TypeFacts f = facts(t, type);
    if (!f.ok) return "a type kiln.types cannot describe";
    if (depth > 32) return "nested too deep";
    const KilnTypeInfo& in = f.info;
    if (leaf(in.kind)) return "";
    if (in.kind == KILN_TYPE_TYPEDEF) {
        const TypeFacts inner = facts(t, in.inner);
        if (!inner.ok || inner.info.type_size != in.type_size) return "a typedef of another size";
        return walk(t, in.inner, base, pads, depth + 1);
    }
    if (in.kind == KILN_TYPE_STATIC_ARRAY) {
        const TypeFacts e = facts(t, in.inner);
        if (!e.ok || !e.info.type_size || in.type_size % e.info.type_size) return "an array whose size is not whole elements";
        if (leaf(e.info.kind)) return "";
        std::vector<uint32_t> one;
        const std::string why = walk(t, in.inner, 0, one, depth + 1);
        if (!why.empty()) return why;
        for (uint64_t i = 0, n = in.type_size / e.info.type_size; i < n && !one.empty(); ++i)
            for (uint32_t b : one) pads.push_back(static_cast<uint32_t>(base + i * e.info.type_size + b));
        return "";
    }
    if (in.kind != KILN_TYPE_STRUCT) return "a kind that owns memory or points somewhere";
    struct Member {
        uint64_t offset, size;
        const KilnType* type;
        bool empty;
    };
    std::vector<Member> members;
    if (in.inner && !empty_struct(t, in.inner, depth + 1)) {
        const TypeFacts b = facts(t, in.inner);
        if (!b.ok) return "a base class kiln.types cannot describe";
        members.push_back({0, b.info.type_size, in.inner, false});
    }
    for (uint32_t i = 0; i < in.field_count; ++i) {
        KilnTypeField field{};
        if (t->type_field(type, i, &field) != KILN_OK || !field.type) return "a field kiln.types cannot describe";
        const TypeFacts ft = facts(t, field.type);
        if (!ft.ok || !ft.info.type_size) return "a field without a size";
        members.push_back({field.offset, ft.info.type_size, field.type, empty_struct(t, field.type, depth + 1)});
    }
    if (members.empty()) return "no reflected member";
    std::stable_sort(members.begin(), members.end(), [](const Member& a, const Member& b) { return a.offset < b.offset; });
    uint64_t end = 0;
    for (const Member& m : members) {
        if (m.offset < end) return "members overlap (a union?)";
        if (m.offset > end) {
            const TypeFacts mt = facts(t, m.type);
            const uint64_t a = mt.ok ? mt.info.alignment : 0;
            if (!a || round_up(end, a) != m.offset) return "a gap the alignment does not explain";
            for (uint64_t b = end; b < m.offset; ++b) pads.push_back(static_cast<uint32_t>(base + b));
        }
        if (!m.empty) {
            const std::string why = walk(t, m.type, base + m.offset, pads, depth + 1);
            if (!why.empty()) return why;
        }
        end = m.offset + m.size;
    }
    if (end > in.type_size) return "a member past the end";
    if (end < in.type_size) {
        if (!in.alignment || round_up(end, in.alignment) != in.type_size) return "a tail the alignment does not explain";
        for (uint64_t b = end; b < in.type_size; ++b) pads.push_back(static_cast<uint32_t>(base + b));
    }
    return "";
}
}  // namespace

Padding padding_of(const KilnTypesApi* types, const KilnType* type) {
    Padding p;
    if (!types || !types->type_info || !types->type_field) {
        p.why = "no kiln.types";
        return p;
    }
    p.why = walk(types, type, 0, p.bytes, 0);
    if (!p.why.empty()) p.bytes.clear();
    else if (p.bytes.empty()) p.why = "no padding";
    std::sort(p.bytes.begin(), p.bytes.end());
    p.bytes.erase(std::unique(p.bytes.begin(), p.bytes.end()), p.bytes.end());
    return p;
}

// -------------------------------------------------------------------------------------------------- the mod ----
namespace {
namespace fs = std::filesystem;
using clock = std::chrono::steady_clock;

uint64_t steady_ns() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now().time_since_epoch()).count());
}
int64_t utc_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}
std::string hex(const uint8_t* p, size_t n) {
    std::string s;
    char b[3];
    for (size_t i = 0; i < n; ++i) {
        std::snprintf(b, sizeof b, "%02x", p[i]);
        s += b;
    }
    return s;
}
std::string json_text(const std::string& s) {
    std::string o = "\"";
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') { o += '\\'; o += static_cast<char>(c); }
        else if (c < 0x20) { char b[8]; std::snprintf(b, sizeof b, "\\u%04x", c); o += b; }
        else o += static_cast<char>(c);
    }
    return o + "\"";
}
const char* status_name(uint32_t s) {
    switch (s) {
        case KILN_ECS_OK: return "ok";
        case KILN_ECS_ABSENT: return "absent";
        case KILN_ECS_STALE: return "stale";
        case KILN_ECS_UNREADABLE: return "unreadable";
        case KILN_ECS_UNAVAILABLE: return "unavailable";
        case KILN_ECS_BUSY: return "busy";
        case KILN_ECS_CONFLICT: return "conflict";
        case KILN_ECS_NOT_WRITABLE: return "not-writable";
        case KILN_ECS_UNCERTAIN: return "uncertain";
        default: return "other";
    }
}
constexpr int kStatuses = 10;
constexpr size_t kMaxHolds = 1 << 21;  // the summary's sample (16 MiB); a long, fast run keeps its first two million
// Kit 2's timing (GitHub #58). An update holds the gate from before its Pre until after its Post (and the engine a
// little longer: the rest of its update), and the gate's busy latch answers this owner busy for the rest of the server
// step of a busy answer: a try is only worth making between two updates, after a Post edge whose step is past the
// latched one. Busy tries are counted in server steps, and each op has a wall-clock limit for when no step runs.
constexpr int kChangeTries = 4;        // a change (or a no-op) busy for this many steps is skipped; the next pair tries
constexpr int kRestoreTries = 300;     // a restore busy for this many steps (about 5 s at 60 a second) stops the run
constexpr uint64_t kChangeWallNs = 1'000'000'000ull;    // ... or busy this long (no step ran): skipped as well
constexpr uint64_t kRestoreWallNs = 10'000'000'000ull;  // ... or this long: the restore (and the final one) gives up
constexpr uint64_t kPollNs = 500'000;  // a wait's poll (Windows sleeps in whole milliseconds: about 1 ms there)
constexpr uint64_t kEdgeWaitNs = 250'000'000;  // no Post edge this long: a timeout (post_edge_timeouts)
constexpr uint64_t kMaxPostDelayNs = 8'000'000;  // the learned wait after a Post edge, at most (see Loop::learn)

// Steps recorded on the tick thread, taken by the loop: one producer, one consumer, fixed size, no lock.
struct StepRow {
    uint64_t step = 0, pre_ns = 0, post_ns = 0;
    int64_t utc_ms = 0;
};
class StepRing {
public:
    static constexpr uint64_t kRows = 1 << 15;  // about nine minutes of 60 Hz steps between two takes
    void push(const StepRow& r) noexcept {
        const uint64_t h = head_.load(std::memory_order_relaxed);
        if (h - tail_.load(std::memory_order_acquire) >= kRows) {
            dropped_.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        rows_[h % kRows] = r;
        head_.store(h + 1, std::memory_order_release);
    }
    template <class Fn> size_t take(Fn&& fn) {
        const uint64_t h = head_.load(std::memory_order_acquire);
        uint64_t t = tail_.load(std::memory_order_relaxed);
        const size_t n = static_cast<size_t>(h - t);
        for (; t < h; ++t) fn(rows_[t % kRows]);
        tail_.store(t, std::memory_order_release);
        return n;
    }
    uint64_t dropped() const noexcept { return dropped_.load(std::memory_order_relaxed); }

private:
    std::unique_ptr<StepRow[]> rows_{new StepRow[kRows]};
    std::atomic<uint64_t> head_{0}, tail_{0}, dropped_{0};
};

enum class Phase : int { Off, Waiting, Armed, Done };

struct Mod {
    const KilnApi* api = nullptr;
    KilnOwner owner = 0;
    const KilnWorldApi* world = nullptr;
    const KilnTasksApi* tasks = nullptr;
    const KilnSavesApi* saves = nullptr;
    const KilnPlayersApi* players = nullptr;
    const KilnTypesApi* types = nullptr;
    const KilnEngineApi* engine = nullptr;
    const KilnCapsApi* caps = nullptr;
    std::string version;   // this mod's, from native.json
    RunConfig config;
    fs::path run_file, logs;
    std::atomic<Phase> phase{Phase::Off};
    std::atomic<bool> recording{false}, world_stopping{false};
    std::atomic<uint32_t> server_session{0};
    // The server's step edges as the tick thread last saw them: the step of the last Pre, the step and steady time of
    // the last Post. The loop reads them to try the gate only between two updates (kit 2, GitHub #58).
    std::atomic<uint64_t> edge_pre{0}, edge_post{0}, edge_post_ns{0};
    StepRing steps;
    // tick thread only
    uint64_t pre_step = 0, pre_ns = 0, next_look_ns = 0;
    int64_t pre_utc = 0;
    bool have_pre = false;
    uint32_t logged_session = 0;  // the server session whose GUID line was logged
};
Mod* g = nullptr;

void say(int32_t level, const char* fmt, ...) {
    char text[1024];
    va_list ap;
    va_start(ap, fmt);
    const int n = std::vsnprintf(text, sizeof text, fmt, ap);
    va_end(ap);
    char line[1100];
    const int m = std::snprintf(line, sizeof line, "gate-priority-test: %s", text);
    if (g && g->api && g->api->log && n >= 0 && m > 0)
        g->api->log(g->owner, level, KilnStr{line, std::min<size_t>(static_cast<size_t>(m), sizeof line - 1)});
}
std::string path_of(int32_t kind) {
    if (!g->api->path) return "";
    char buf[1024];
    size_t len = 0;
    if (g->api->path(kind, buf, sizeof buf, &len) != KILN_OK || len > sizeof buf) return "";
    return std::string(buf, len);
}
fs::path utf8_path(const std::string& s) { return fs::path(std::u8string(reinterpret_cast<const char8_t*>(s.data()), s.size())); }

std::string raise_mode(std::string* note) {
    if (!g->caps || !g->caps->find) return "unknown";
    KilnCapRow row{};
    row.size = sizeof row;
    static const char name[] = "engine.gate_priority_raise";
    if (g->caps->find(KilnStr{name, sizeof name - 1}, &row) != KILN_OK) return "unknown";
    if (note && row.text.ptr) note->assign(row.text.ptr, row.text.len);
    return row.state == KILN_CAP_AVAILABLE ? "on" : row.state == KILN_CAP_UNAVAILABLE ? "off" : "unknown";
}

// ---- the loop (touched only by batches, one after another) ----
struct Loop {
    KilnEntity player = 0;
    KilnObjectRef ref{};
    uint32_t hash = 0, size = 0, offset = 0;
    std::string name, pick, mode = "write", end_reason, raise, raise_note, profile, label;
    bool started = false, finished = false, flipped = false, restored = true, verified = false;
    uint8_t original = 0;                // the target byte before the change
    std::vector<uint8_t> cur, expected, desired, mask, confirmed, snap;
    uint64_t warmup_end = 0, deadline = 0, next_due = 0, period = 0, next_gate = 0, pairs = 0;
    int64_t started_utc = 0, writes_utc = 0;
    uint64_t counts[kStatuses + 1] = {};  // by status; [kStatuses]: ok without a write (no-op)
    uint64_t verify_ok = 0, verify_bad = 0, reopened = 0;
    uint64_t busy_step = 0;                      // the step the last answer may have latched (see exchange)
    bool latched = false;                        // the last answer was busy: no try before a step past busy_step
    bool aligned = false;                        // the next try comes right at post_delay after a Post edge
    uint64_t post_delay = 0;                     // how long after a Post edge the gate has been free (learned)
    uint64_t edge_waits = 0, edge_timeouts = 0;  // waits for a Post edge, and those that saw none for 250 ms
    std::vector<uint64_t> holds;          // hold_ns of every successful exchange (the summary's percentiles)
    std::vector<uint64_t> restores;       // restore_ns of the same, when this Kiln measures it
    // Where this Kiln ends a hold (setup, from kiln.engine's gate counters): "scene-release" (the Scene word's release;
    // the priority restore is timed apart, in restore_ns), "release-end" (an older Kiln: the hold includes the rest of
    // the release and the restore, and restore_ns is not filled) or "unknown" (no gate counters).
    std::string hold_ends = "unknown";
    bool restore_measured() const { return hold_ends == "scene-release"; }
    fs::path dir;
    std::ofstream writes, steps, gate;
    int loop_priority = 0;

    bool open_ref() {
        ref = KilnObjectRef{};
        return g->world->entity_open(g->owner, g->owner, KILN_WORLD_SERVER, 0, player, &ref) == KILN_OK;
    }
    // A stable copy of component `c`, read again while a frame changes it under the copy (bounded).
    KilnResult snapshot(uint32_t c, std::vector<uint8_t>& out, uint32_t bytes) {
        out.assign(bytes, 0);
        uint32_t req = 0;
        KilnResult rc = KILN_ERR_UNAVAILABLE;
        for (int i = 0; i < 20; ++i) {
            rc = g->world->ecs_snapshot(g->owner, &ref, c, out.data(), bytes, &req);
            if (rc != KILN_ECS_ERR_UNREADABLE && rc != KILN_WORLD_ERR_CHANGED) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return rc;
    }
    KilnResult read(std::vector<uint8_t>& out) { return snapshot(hash, out, size); }
    std::string choose();
    bool setup();
    void row(const char* op, int attempt, KilnResult rc, const KilnMutationReply& r, uint64_t call_ns, int64_t at, int prio,
             uint64_t post_step, uint64_t since_post_ns) {
        char restore[24] = "";  // empty: this Kiln does not measure it (its hold_ns includes the restore)
        if (restore_measured()) std::snprintf(restore, sizeof restore, "%llu", static_cast<unsigned long long>(r.restore_ns));
        char line[320];
        const int n = std::snprintf(line, sizeof line, "%lld,%s,%d,%s,%d,%u,%u,0x%x,%llu,%llu,%llu,%d,%llu,%llu,%s\n",
                                    static_cast<long long>(at), op, attempt, status_name(r.status), rc, r.why_code,
                                    r.engine_reason, r.flags, static_cast<unsigned long long>(r.hold_ns),
                                    static_cast<unsigned long long>(call_ns), static_cast<unsigned long long>(r.step), prio,
                                    static_cast<unsigned long long>(post_step), static_cast<unsigned long long>(since_post_ns),
                                    restore);
        if (n > 0) writes.write(line, std::min<int>(n, sizeof line - 1));
    }
    // One compare-exchange of the whole component: `want` is the target byte's new value (write mode), or none (no-op).
    KilnResult exchange(const char* op, int attempt, KilnMutationReply& r, bool change, uint8_t want) {
        expected = cur;
        desired = cur;
        if (change) desired[offset] = want;
        r = KilnMutationReply{};
        r.size = sizeof r;
        confirmed.assign(size, 0);
        const int prio = current_thread_priority();
        const uint64_t post_step = g->edge_post.load(std::memory_order_acquire);
        const uint64_t post_ns = g->edge_post_ns.load(std::memory_order_relaxed);
        const int64_t at = utc_ms();
        const uint64_t t0 = steady_ns();
        const KilnResult rc = g->world->ecs_compare_exchange(g->owner, &ref, hash, expected.data(), change ? mask.data() : nullptr,
                                                             desired.data(), size, confirmed.data(), &r);
        const uint64_t took = steady_ns() - t0;
        // The step a busy answer latches (the gate's live step) for after_busy: the larger of the server step
        // kiln.world saw in the call and the last Pre the tick thread saw since. One step low when a Pre lands inside
        // the call after kiln.world read its step and before this mod's Pre ran: the retry after the next Post then
        // meets the latch once (engine reason 45), and that answer's step is right. Accepted: it corrects itself.
        busy_step = std::max<uint64_t>(r.step, g->edge_pre.load(std::memory_order_acquire));
        if (rc == KILN_ERR_INVALID_ARGUMENT) r.status = KILN_ECS_UNAVAILABLE;  // refused before the reply was filled
        const uint64_t since = post_step && t0 > post_ns ? t0 - post_ns : 0;
        row(op, attempt, rc, r, took, at, prio, post_step, since);
        latched = r.status == KILN_ECS_BUSY || r.status == KILN_ECS_UNREADABLE;
        learn(r, post_step, since);
        counts[std::min<uint32_t>(r.status, kStatuses - 1)] += 1;
        if (r.status == KILN_ECS_OK && (r.flags & KILN_MUTATION_NO_OP)) counts[kStatuses] += 1;
        if (r.status == KILN_ECS_OK && holds.size() < kMaxHolds) holds.push_back(r.hold_ns);  // writes.csv has them all
        if (r.status == KILN_ECS_OK && restore_measured() && restores.size() < kMaxHolds) restores.push_back(r.restore_ns);
        if ((r.flags & KILN_MUTATION_CONFIRMED) && (r.status == KILN_ECS_OK || r.status == KILN_ECS_CONFLICT)) cur = confirmed;
        return rc;
    }
    // How long past its Post the engine keeps the gate (the rest of its update), learned from the answers: a try made
    // right at post_delay after a Post edge that the engine answered busy (scene or registry word) while no step had
    // begun since (kiln.world's step is still the Post's) came too early, so wait past that moment plus 1 ms after the
    // next edge; one that a new update answered (a step began) came too late: halve the wait. Every success of such a
    // try shrinks it by 1/32, so a wait learned from one slow update does not stay.
    void learn(const KilnMutationReply& r, uint64_t post_step, uint64_t since) {
        const bool between = post_step && r.step == post_step;
        // the engine's own words busy: not another Kiln holder (44) nor this owner's latch (45)
        const bool engine_busy = r.status == KILN_ECS_BUSY && r.engine_reason && r.engine_reason != 44 && r.engine_reason != 45;
        if (aligned && post_step && engine_busy) {
            if (between) post_delay = std::min<uint64_t>(kMaxPostDelayNs, std::max(post_delay, since) + 1'000'000);
            else post_delay /= 2;
        }
        if (aligned && between && r.status == KILN_ECS_OK) post_delay -= post_delay / 32;
        aligned = false;
    }
    void sleep_ms(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }
    bool stopping(const KilnTaskRun* run) const {
        return g->world_stopping.load() || (run && run->cancelled && run->cancelled(run));
    }
    // Between two updates, as far as the step edges show: the last Post edge is of the step whose Pre came last.
    bool between_steps() const {
        const uint64_t post = g->edge_post.load(std::memory_order_acquire);
        return post && post >= g->edge_pre.load(std::memory_order_acquire);
    }
    enum class Edge { Seen, Timeout, Stop };
    // Waits until the update of a step past `after` is over (its Post edge) and `delay_ns` more have passed, at most
    // until `until` (Timeout; a wait that saw no Post edge for kEdgeWaitNs counts in post_edge_timeouts). Stop: the
    // server world stops, or the task is cancelled when `cancellable`.
    Edge wait_edge(uint64_t after, uint64_t delay_ns, const KilnTaskRun* run, bool cancellable, uint64_t until) {
        ++edge_waits;
        const uint64_t began = steady_ns();
        for (;;) {
            std::this_thread::sleep_for(std::chrono::nanoseconds(kPollNs));
            if (g->world_stopping.load() || (cancellable && run && run->cancelled && run->cancelled(run))) return Edge::Stop;
            const uint64_t now = steady_ns();
            if (g->edge_post.load(std::memory_order_acquire) > after &&
                now >= g->edge_post_ns.load(std::memory_order_relaxed) + delay_ns)
                return Edge::Seen;
            if (now >= until) {
                if (now - began >= kEdgeWaitNs) ++edge_timeouts;
                return Edge::Timeout;
            }
        }
    }
    // Before the try after a busy answer: a Post edge past busy_step, then post_delay, plus 1..3 ms more for the 2nd to
    // 4th busy answer in a row (then from 0 again), so a retry cannot lock onto one moment of the step. A wait that sees
    // no Post edge for kEdgeWaitNs goes on to the try only when a Pre came past busy_step (the latch is gone, its Post
    // did not come); with no step at all it keeps waiting: a try could only meet the latch. false: `deadline` passed,
    // or Stop (the world stops; the task is cancelled when `cancellable`).
    bool after_busy(int busy_in_a_row, const KilnTaskRun* run, bool cancellable, uint64_t deadline) {
        const uint64_t delay = post_delay + static_cast<uint64_t>((busy_in_a_row - 1) % 4) * 1'000'000ull;
        for (;;) {
            const uint64_t now = steady_ns();
            if (now >= deadline) return false;
            switch (wait_edge(busy_step, delay, run, cancellable, std::min(now + kEdgeWaitNs, deadline))) {
                case Edge::Seen:
                    latched = false;
                    aligned = true;
                    return true;
                case Edge::Stop: return false;
                case Edge::Timeout:
                    if (g->edge_pre.load(std::memory_order_acquire) > busy_step) {
                        latched = false;
                        return true;
                    }
                    break;  // no step ran: the latch is still set
            }
        }
    }
    // Why a restore gave up: its tries, the server steps and the time they took, and the waits that saw no step.
    std::string stayed_busy(const char* op, const KilnMutationReply& r, int tries, uint64_t began, uint64_t step0,
                            uint64_t timeouts0) const {
        char text[300];
        std::snprintf(text, sizeof text, "the %s stayed %s: %d %s in %llu server steps and %.1f s", op, status_name(r.status),
                      tries, tries == 1 ? "try" : "tries",
                      static_cast<unsigned long long>(g->edge_pre.load() - std::min(step0, g->edge_pre.load())),
                      static_cast<double>(steady_ns() - began) / 1e9);
        std::string s = text;
        if (edge_timeouts > timeouts0)
            s += "; " + std::to_string(edge_timeouts - timeouts0) + " waits saw no server step for 250 ms (post_edge_timeouts)";
        return s;
    }
    // Retries an exchange while the gate is busy or a read faulted: one try per server step, at most `tries` of them and
    // `wall_ns` (bounded). false: a refusal that ends the run.
    bool attempt(const char* op, bool change, int tries, uint64_t wall_ns, const KilnTaskRun* run, KilnMutationReply& r) {
        const uint64_t began = steady_ns(), step0 = g->edge_pre.load(std::memory_order_acquire), timeouts0 = edge_timeouts;
        int busy = 0, made = 0;
        for (; made < tries;) {
            const uint8_t want = flipped ? original : static_cast<uint8_t>(original ^ 1u);
            exchange(op, made++, r, change, want);
            switch (r.status) {
                case KILN_ECS_OK: return true;
                case KILN_ECS_CONFLICT:
                    // The game changed the component (`cur` holds it now). Before our change: nothing of ours is in
                    // it, try again at the next pair. After it: put the byte back from the new value (a no-op when the
                    // byte is already as it was).
                    if (!flipped) original = cur[offset];
                    if (!change || !flipped) return true;
                    continue;
                case KILN_ECS_BUSY:
                case KILN_ECS_UNREADABLE:
                    if (stopping(run) && !flipped) return true;
                    // Also after the last try: the next pair's first try must not meet the latch either. A restore keeps
                    // trying through a cancellation.
                    if (after_busy(++busy, run, !flipped, began + wall_ns)) continue;
                    if (!flipped) return true;  // stopping, cancelled or out of time: no more tries; the next pair tries
                    if (g->world_stopping.load()) {
                        end_reason = "the server world stopped";
                        return false;
                    }
                    end_reason = stayed_busy(op, r, made, began, step0, timeouts0);
                    return false;
                case KILN_ECS_STALE:
                case KILN_ECS_ABSENT:
                    ++reopened;
                    if (!open_ref() || read(cur) != KILN_OK) {
                        end_reason = "the player is gone";
                        return false;
                    }
                    if (!flipped) original = cur[offset];
                    continue;
                case KILN_ECS_UNAVAILABLE:
                    // the gate closes at a stopping world a moment before the world's STOPPING reaches this mod
                    for (int w = 0; w < 50 && !g->world_stopping.load(); ++w) sleep_ms(1);
                    if (g->world_stopping.load()) {
                        end_reason = "the server world stopped";
                        return false;
                    }
                    [[fallthrough]];
                default:
                    end_reason = std::string("the gate refused the write: ") + status_name(r.status) + " (why " +
                                 std::to_string(r.why_code) + ")";
                    return false;
            }
        }
        if (flipped) {
            end_reason = stayed_busy(op, r, made, began, step0, timeouts0);
            return false;
        }
        return true;  // the change stayed busy: try again at the next pair
    }
    bool pair(const KilnTaskRun* run) {
        KilnMutationReply r{};
        ++pairs;
        // The last busy answer's latch may still be set (no step since it, a change that ran out of time): wait for a
        // step past it first, or skip this pair.
        if (latched && !after_busy(1, run, true, steady_ns() + kChangeWallNs)) return true;
        // A pair starts between two updates, post_delay or more after the Post edge: one due while an update runs waits
        // for that update's Post (an update holds the gate throughout; a try inside one is only a busy answer and a
        // latched step), one due sooner after a Post waits until post_delay after it.
        const uint64_t post = g->edge_post.load(std::memory_order_acquire);
        if (post) {
            const bool in_update = !between_steps();
            const uint64_t post_ns = g->edge_post_ns.load(std::memory_order_relaxed), now = steady_ns();
            if (in_update || now < post_ns + post_delay) {
                const Edge e = wait_edge(in_update ? g->edge_pre.load(std::memory_order_acquire) - 1 : post - 1, post_delay, run,
                                         true, now + kEdgeWaitNs);
                if (e == Edge::Stop) return true;  // run_for ends the run
                aligned = e == Edge::Seen;
            }
        }
        if (mode == "noop") return attempt("noop", false, kChangeTries, kChangeWallNs, run, r);
        if (!attempt("change", true, kChangeTries, kChangeWallNs, run, r)) return false;
        if (r.status == KILN_ECS_OK && (r.flags & KILN_MUTATION_WRITTEN)) flipped = true;
        if (flipped) {
            if (!attempt("restore", true, kRestoreTries, kRestoreWallNs, run, r)) return false;
            if (r.status == KILN_ECS_OK) flipped = false;  // written back (or already as it was: a no-op)
        }
        if (pairs % 64 == 0 && read(snap) == KILN_OK) {
            (snap[offset] == original ? verify_ok : verify_bad) += 1;
            cur = snap;
        }
        return true;
    }
    void take_steps() {
        g->steps.take([&](const StepRow& s) {
            char line[128];
            const int n = std::snprintf(line, sizeof line, "%llu,%lld,%llu,%llu\n", static_cast<unsigned long long>(s.step),
                                        static_cast<long long>(s.utc_ms), static_cast<unsigned long long>(s.pre_ns),
                                        static_cast<unsigned long long>(s.post_ns));
            if (n > 0) steps.write(line, n);
        });
    }
    // Whether kiln.engine's gate counters carry the restore fields (gate_stats leaves the size it wrote in s.size).
    static bool has_restore_stats(const KilnEngineGateStats& s) {
        return s.size >= offsetof(KilnEngineGateStats, restore_ns_max) + sizeof s.restore_ns_max;
    }
    // A Kiln whose gate counters carry the restore fields ends a hold at the Scene word's release and fills
    // KilnMutationReply.restore_ns (GitHub #58); an older one measured each hold to the end of the gate's release.
    std::string hold_end() const {
        if (!g->engine || !g->engine->gate_stats) return "unknown";
        KilnEngineGateStats s{};
        s.size = sizeof s;
        if (g->engine->gate_stats(&s) != KILN_OK) return "unknown";
        return has_restore_stats(s) ? "scene-release" : "release-end";
    }
    void gate_row() {
        if (!g->engine || !g->engine->gate_stats) return;
        KilnEngineGateStats s{};
        s.size = sizeof s;
        if (g->engine->gate_stats(&s) != KILN_OK) return;
        char restore[72] = ",,";  // empty cells: this Kiln's counters have no restore fields
        if (has_restore_stats(s))
            std::snprintf(restore, sizeof restore, "%llu,%llu,%llu", static_cast<unsigned long long>(s.restores),
                          static_cast<unsigned long long>(s.restore_ns_total), static_cast<unsigned long long>(s.restore_ns_max));
        char line[400];
        const int n = std::snprintf(line, sizeof line, "%lld,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%s\n",
                                    static_cast<long long>(utc_ms()), static_cast<unsigned long long>(s.attempts),
                                    static_cast<unsigned long long>(s.acquired), static_cast<unsigned long long>(s.busy_registry),
                                    static_cast<unsigned long long>(s.busy_scene), static_cast<unsigned long long>(s.refused_in_frame),
                                    static_cast<unsigned long long>(s.refused_world_thread),
                                    static_cast<unsigned long long>(s.refused_other), static_cast<unsigned long long>(s.hold_ns_total),
                                    static_cast<unsigned long long>(s.hold_ns_max), static_cast<unsigned long long>(s.views),
                                    static_cast<unsigned long long>(s.view_failures), restore);
        if (n > 0) gate.write(line, std::min<int>(n, sizeof line - 1));
    }
    std::string header() const {
        return std::string("# gate-priority-test ") + kKitVersion + "; raise=" + raise + "; label=" + label + "; mode=" + mode +
               "; profile=" + profile + "; hold=" + hold_ends + "\n";
    }
    void write_run_json(bool final);
    void finish(const std::string& why, const KilnTaskRun* run);
    void run_for(const KilnTaskRun* run, uint64_t budget_ns);
};
Loop* g_loop = nullptr;

uint64_t percentile(std::vector<uint64_t>& v, double p) {
    if (v.empty()) return 0;
    const size_t i = std::min(v.size() - 1, static_cast<size_t>(p * static_cast<double>(v.size() - 1) + 0.5));
    std::nth_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(i), v.end());
    return v[i];
}

// The target: the component the run file names, else the smallest component of the player that the write policy
// admits and whose reflected layout has padding. "" when one was found; else why not.
std::string Loop::choose() {
    struct Cand {
        uint32_t hash, size, offset;
        std::string name;
    };
    std::vector<Cand> cands;
    std::vector<KilnEcsRegistryRow> rows;
    KilnEcsRegistryInfo info{};
    info.size = sizeof info;
    uint32_t required = 0;
    if (!g->types || g->types->header.size < offsetof(KilnTypesApi, ecs_registry) + sizeof(void*) || !g->types->ecs_registry)
        return "kiln.types has no component registry";
    g->types->ecs_registry(KILN_WORLD_SERVER, &info, nullptr, 0, &required);
    rows.resize(required);
    if (!required || g->types->ecs_registry(KILN_WORLD_SERVER, &info, rows.data(), required, &required) != KILN_OK)
        return "kiln.types' component registry is empty";
    uint32_t wanted = 0;
    if (!g->config.component.empty()) {
        std::string q = g->config.component;
        if (q.find("::") == std::string::npos) q = "keen::ecs::" + q;
        const KilnType* t = g->types->type_by_name ? g->types->type_by_name(KilnStr{q.data(), q.size()}) : nullptr;
        KilnTypeInfo ti{};
        ti.size = sizeof ti;
        if (!t || g->types->type_info(t, &ti) != KILN_OK) return "no component named " + g->config.component;
        wanted = ti.hash;
    }
    size_t writable = 0, present = 0;
    std::string why_named;
    for (const auto& row : rows) {
        if (wanted && row.component != wanted) continue;
        KilnEcsWritePolicy p{};
        p.size = sizeof p;
        if (g->world->ecs_write_policy(KILN_WORLD_SERVER, row.component, &p) != KILN_OK || p.status != KILN_ECS_OK) {
            if (wanted) why_named = std::string("the write policy refuses it: ") + p.why;
            continue;
        }
        ++writable;
        std::vector<uint8_t> b;
        if (snapshot(row.component, b, p.component_size ? p.component_size : 1) != KILN_OK) {
            if (wanted) why_named = "the player has no such component";
            continue;
        }
        ++present;
        const KilnType* t = g->types->type_by_hash(row.component);
        KilnTypeInfo ti{};
        ti.size = sizeof ti;
        const std::string tname = t && g->types->type_info(t, &ti) == KILN_OK ? std::string(ti.qualified_name.ptr, ti.qualified_name.len) : "?";
        const Padding pad = padding_of(g->types, t);
        if (wanted && g->config.offset >= 0) {
            // the tester's own byte: only one of the component's padding bytes, by the same rule as the automatic pick
            const uint32_t at = static_cast<uint32_t>(g->config.offset);
            if (at >= p.component_size) return "offset= is past the end of " + tname;
            if (!std::binary_search(pad.bytes.begin(), pad.bytes.end(), at))
                return "offset= " + std::to_string(at) + " is not a padding byte of " + tname + " (" +
                       (pad.bytes.empty() ? pad.why : "its padding bytes start at " + std::to_string(pad.bytes.front())) + ")";
            cands.push_back({row.component, p.component_size, at, tname});
            pick = "named in the run file, with its byte";
            break;
        }
        if (pad.bytes.empty() || pad.bytes.front() >= p.component_size) {
            if (wanted) why_named = "no padding byte we may use: " + pad.why;
            continue;
        }
        cands.push_back({row.component, p.component_size, pad.bytes.front(), tname});
    }
    say(KILN_LOG_INFO, "%zu components may be written from a task, %zu of them on player %u, %zu with a padding byte",
        writable, present, player, cands.size());
    if (cands.empty()) return wanted ? (why_named.empty() ? "the named component cannot be used" : why_named) : "no writable component of the player has a padding byte";
    std::stable_sort(cands.begin(), cands.end(), [](const Cand& a, const Cand& b) {
        return a.size != b.size ? a.size < b.size : a.name < b.name;
    });
    std::string list;
    for (size_t i = 0; i < cands.size() && i < 8; ++i)
        list += cands[i].name + " (" + std::to_string(cands[i].size) + " bytes, byte " + std::to_string(cands[i].offset) + "), ";
    say(KILN_LOG_INFO, "candidates, smallest first: %s", list.c_str());
    hash = cands.front().hash;
    size = cands.front().size;
    offset = cands.front().offset;
    name = cands.front().name;
    if (pick.empty()) pick = wanted ? "named in the run file; its first padding byte" : "the smallest writable component of the player with a padding byte";
    return "";
}

bool Loop::setup() {
    started = true;
    started_utc = utc_ms();
    loop_priority = current_thread_priority();
    label = g->config.label;
    raise = raise_mode(&raise_note);
    hold_ends = hold_end();
    if (g->engine && g->engine->identity) {
        KilnEngineIdentity id{};
        id.size = sizeof id;
        if (g->engine->identity(&id) == KILN_OK) profile = id.profile_id[0] ? id.profile_id : "none";
    }
    // the output folder: <logs>/gate-priority-test/run-<UTC time>
    const std::time_t now = std::time(nullptr);
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &now);
#else
    gmtime_r(&now, &tm);
#endif
    char stamp[32];
    std::strftime(stamp, sizeof stamp, "run-%Y%m%d-%H%M%S", &tm);
    std::error_code ec;
    dir = g->logs / stamp;
    fs::create_directories(dir, ec);
    writes.open(dir / "writes.csv", std::ios::binary | std::ios::trunc);
    steps.open(dir / "steps.csv", std::ios::binary | std::ios::trunc);
    gate.open(dir / "gate.csv", std::ios::binary | std::ios::trunc);
    if (!writes || !steps || !gate) {
        say(KILN_LOG_ERROR, "cannot write its files in kiln/logs/gate-priority-test/%s; the run does not start", stamp);
        end_reason = "no output files";
        finished = true;
        return false;
    }
    if (!open_ref()) {
        end_reason = "the player could not be opened";
        say(KILN_LOG_WARN, "player %u could not be opened; the run does not start", player);
        write_run_json(true);
        finished = true;
        return false;
    }
    std::string why = choose();
    if (!why.empty() && g->config.noop) why.clear();  // no-op mode needs no padding: any writable component will do
    if (!why.empty() && !g->config.component.empty()) {
        end_reason = why;
        say(KILN_LOG_WARN, "%s; the run does not start", why.c_str());
        write_run_json(true);
        finished = true;
        return false;
    }
    if (g->config.noop) mode = "noop";
    if (!why.empty()) {
        say(KILN_LOG_WARN, "%s: it runs no-op exchanges instead (they take the gate and change nothing)", why.c_str());
        mode = "noop";
        pick = "no padding byte: no-op exchanges";
    }
    if (mode == "noop" && !hash) {
        // the smallest writable component of the player
        std::vector<KilnEcsRegistryRow> rows;
        KilnEcsRegistryInfo info{};
        info.size = sizeof info;
        uint32_t required = 0;
        g->types->ecs_registry(KILN_WORLD_SERVER, &info, nullptr, 0, &required);
        rows.resize(required);
        if (required) g->types->ecs_registry(KILN_WORLD_SERVER, &info, rows.data(), required, &required);
        for (const auto& row : rows) {
            KilnEcsWritePolicy p{};
            p.size = sizeof p;
            if (g->world->ecs_write_policy(KILN_WORLD_SERVER, row.component, &p) != KILN_OK || p.status != KILN_ECS_OK) continue;
            std::vector<uint8_t> b;
            if (snapshot(row.component, b, p.component_size) != KILN_OK) continue;
            if (!hash || p.component_size < size) {
                hash = row.component;
                size = p.component_size;
                const KilnType* t = g->types->type_by_hash(row.component);
                KilnTypeInfo ti{};
                ti.size = sizeof ti;
                name = t && g->types->type_info(t, &ti) == KILN_OK ? std::string(ti.qualified_name.ptr, ti.qualified_name.len) : "?";
            }
        }
        if (!hash) {
            end_reason = "the player has no component the write policy admits";
            say(KILN_LOG_WARN, "%s; the run does not start", end_reason.c_str());
            write_run_json(true);
            finished = true;
            return false;
        }
    }
    if (read(cur) != KILN_OK) {
        end_reason = "the component could not be read";
        say(KILN_LOG_WARN, "%s could not be read; the run does not start", name.c_str());
        write_run_json(true);
        finished = true;
        return false;
    }
    mask.assign(size, 0);
    if (mode == "write") mask[offset] = 1;
    original = cur[offset];
    writes << header() << "utc_ms,op,attempt,status,result,why,engine_reason,flags,hold_ns,call_ns,step,priority,post_step,"
                          "since_post_ns,restore_ns\n";
    steps << header() << "step,utc_ms,pre_ns,post_ns\n";
    gate << header()
         << "utc_ms,attempts,acquired,busy_registry,busy_scene,refused_in_frame,refused_world_thread,refused_other,"
            "hold_ns_total,hold_ns_max,views,view_failures,restores,restore_ns_total,restore_ns_max\n";
    const uint64_t t = steady_ns();
    warmup_end = t + uint64_t{g->config.warmup_s} * 1'000'000'000ull;
    deadline = warmup_end + (g->config.seconds ? uint64_t{g->config.seconds} : uint64_t{g->config.minutes} * 60) * 1'000'000'000ull;
    period = 1'000'000'000ull / g->config.rate;
    next_due = warmup_end;
    next_gate = t;
    write_run_json(false);
    say(KILN_LOG_INFO, "run %s: %s, %s byte %u of %s (%u bytes; %s), %u pairs a second for %u s after %u s of steps only; "
        "priority raise %s; files in kiln/logs/gate-priority-test/%s", label.empty() ? "-" : label.c_str(), mode.c_str(),
        mode == "noop" ? "no change to" : "flipping bit 0 of", offset, name.c_str(), size, pick.c_str(), g->config.rate,
        g->config.seconds ? g->config.seconds : g->config.minutes * 60, g->config.warmup_s, raise.c_str(), stamp);
    if (raise == "off")
        say(KILN_LOG_WARN, "the priority raise is OFF for this run (%s); delete the switch file after the run", raise_note.c_str());
    return true;
}

void Loop::write_run_json(bool final) {
    std::ofstream j(dir / "run.json", std::ios::binary | std::ios::trunc);
    if (!j) return;
    std::vector<uint64_t> h = holds;
    const char* kiln = "";
    std::string kv;
    if (g->api->version) {
        const KilnStr v = g->api->version();
        kv.assign(v.ptr ? v.ptr : "", v.len);
        kiln = kv.c_str();
    }
    j << "{\n  \"kit\": " << json_text(kKitVersion) << ",\n  \"mod\": " << json_text(g->version)
      << ",\n  \"kiln\": " << json_text(kiln) << ",\n  \"label\": " << json_text(label) << ",\n  \"mode\": " << json_text(mode)
      << ",\n  \"raise\": " << json_text(raise) << ",\n  \"raise_note\": " << json_text(raise_note)
      << ",\n  \"profile\": " << json_text(profile) << ",\n  \"hold_ends\": " << json_text(hold_ends)
      << ",\n  \"side\": "
      << json_text(g->api->size >= offsetof(KilnApi, side) + sizeof(void*) && g->api->side && g->api->side() == KILN_SIDE_CLIENT
                       ? "client"
                       : "server")
      << ",\n  \"cpus\": " << std::thread::hardware_concurrency() << ",\n  \"rate\": " << g->config.rate
      << ",\n  \"seconds\": " << (g->config.seconds ? g->config.seconds : g->config.minutes * 60)
      << ",\n  \"warmup_s\": " << g->config.warmup_s
      << ",\n  \"component\": " << json_text(name) << ",\n  \"component_size\": " << size << ",\n  \"offset\": " << offset
      << ",\n  \"pick\": " << json_text(pick) << ",\n  \"loop_priority\": " << loop_priority
      << ",\n  \"started_utc_ms\": " << started_utc << ",\n  \"writes_from_utc_ms\": "
      << (writes_utc ? writes_utc : started_utc + static_cast<int64_t>(g->config.warmup_s) * 1000)
      << ",\n  \"final\": " << (final ? "true" : "false");
    if (final) {
        j << ",\n  \"ended_utc_ms\": " << utc_ms() << ",\n  \"end\": " << json_text(end_reason) << ",\n  \"pairs\": " << pairs
          << ",\n  \"counts\": {";
        for (int s = 0; s < kStatuses; ++s) j << (s ? ", " : "") << json_text(status_name(static_cast<uint32_t>(s))) << ": " << counts[s];
        j << ", \"no_op\": " << counts[kStatuses] << "}"
          << ",\n  \"hold_ns\": {\"n\": " << h.size() << ", \"p50\": " << percentile(h, 0.5) << ", \"p90\": " << percentile(h, 0.9)
          << ", \"p99\": " << percentile(h, 0.99) << ", \"max\": " << (h.empty() ? 0 : *std::max_element(h.begin(), h.end())) << "}";
        if (restore_measured()) {
            std::vector<uint64_t> rs = restores;
            j << ",\n  \"restore_ns\": {\"n\": " << rs.size() << ", \"p50\": " << percentile(rs, 0.5) << ", \"p90\": "
              << percentile(rs, 0.9) << ", \"p99\": " << percentile(rs, 0.99)
              << ", \"max\": " << (rs.empty() ? 0 : *std::max_element(rs.begin(), rs.end())) << "}";
        }
        j << ",\n  \"restored\": " << (restored ? "true" : "false") << ",\n  \"verified\": " << (verified ? "true" : "false")
          << ",\n  \"verify_ok\": " << verify_ok << ",\n  \"verify_mismatches\": " << verify_bad << ",\n  \"reopened\": " << reopened
          << ",\n  \"steps_dropped\": " << g->steps.dropped() << ",\n  \"post_edge_waits\": " << edge_waits
          << ",\n  \"post_edge_timeouts\": " << edge_timeouts << ",\n  \"post_delay_ns\": " << post_delay;
    }
    j << "\n}\n";
}

void Loop::finish(const std::string& why, const KilnTaskRun* run) {
    if (end_reason.empty()) end_reason = why;
    // A change kiln.world could not prove (uncertain) may have landed although no reply said WRITTEN: the component as
    // it is now decides whether the byte has to be put back.
    if (mode == "write" && !flipped && read(snap) == KILN_OK && snap[offset] != original) {
        flipped = true;
        cur = snap;
    }
    if (flipped && mode == "write") {
        KilnMutationReply r{};
        // put the byte back: bounded as a restore (one busy try per server step, kRestoreTries and kRestoreWallNs), as
        // long as the world is not stopping (the gate is closed then); a cancellation does not stop it
        const uint64_t until = steady_ns() + kRestoreWallNs;
        int busy = 0;
        for (int i = 0; flipped && i < kRestoreTries && !g->world_stopping.load(); ++i) {
            if (latched && !after_busy(++busy, run, false, until)) break;  // also the latch of the restore that gave up
            exchange("final-restore", i, r, true, original);
            if (r.status == KILN_ECS_OK) flipped = false;
            else if (r.status == KILN_ECS_CONFLICT && cur[offset] == original) flipped = false;
            else if (r.status == KILN_ECS_STALE || r.status == KILN_ECS_ABSENT) {
                if (!open_ref() || read(cur) != KILN_OK) break;
            } else if (r.status != KILN_ECS_BUSY && r.status != KILN_ECS_UNREADABLE && r.status != KILN_ECS_CONFLICT) break;
            // busy or unreadable: the next round waits for a step past it; a conflict: again at once, from the new value
        }
    }
    // What a read shows: an unproven restore may have put the byte back, an unproven change may have left it changed.
    if (mode == "noop") {
        restored = verified = true;
    } else if (read(snap) == KILN_OK) {
        restored = verified = snap[offset] == original;
    } else {
        restored = !flipped;
        verified = false;
    }
    take_steps();
    gate_row();
    writes.flush();
    steps.flush();
    gate.flush();
    write_run_json(true);
    std::vector<uint64_t> h = holds;
    char restore[120] = "restore not measured (this Kiln's hold includes the release and the restore)";
    if (restore_measured()) {
        std::vector<uint64_t> rs = restores;
        std::snprintf(restore, sizeof restore, "restore p50 %.1f us, p99 %.1f us, max %.1f us", percentile(rs, 0.5) / 1000.0,
                      percentile(rs, 0.99) / 1000.0, (rs.empty() ? 0 : *std::max_element(rs.begin(), rs.end())) / 1000.0);
    }
    say(restored ? KILN_LOG_INFO : KILN_LOG_WARN,
        "done (%s): %llu pairs; ok %llu (no-op %llu), conflict %llu, busy %llu, unreadable %llu, stale %llu, absent %llu, "
        "not writable %llu, uncertain %llu, unavailable %llu; hold p50 %.1f us, p99 %.1f us, max %.1f us; %s; %s; raise %s",
        end_reason.c_str(), static_cast<unsigned long long>(pairs), static_cast<unsigned long long>(counts[KILN_ECS_OK]),
        static_cast<unsigned long long>(counts[kStatuses]), static_cast<unsigned long long>(counts[KILN_ECS_CONFLICT]),
        static_cast<unsigned long long>(counts[KILN_ECS_BUSY]), static_cast<unsigned long long>(counts[KILN_ECS_UNREADABLE]),
        static_cast<unsigned long long>(counts[KILN_ECS_STALE]), static_cast<unsigned long long>(counts[KILN_ECS_ABSENT]),
        static_cast<unsigned long long>(counts[KILN_ECS_NOT_WRITABLE]), static_cast<unsigned long long>(counts[KILN_ECS_UNCERTAIN]),
        static_cast<unsigned long long>(counts[KILN_ECS_UNAVAILABLE]), percentile(h, 0.5) / 1000.0, percentile(h, 0.99) / 1000.0,
        (h.empty() ? 0 : *std::max_element(h.begin(), h.end())) / 1000.0, restore,
        mode == "noop" ? "nothing was changed"
        : !restored    ? "the last change could not be put back (an untracked padding byte, never saved)"
        : verified     ? "the byte is back and read back"
                       : "the byte is back but could not be read back",
        raise.c_str());
    writes.close();
    steps.close();
    gate.close();
    finished = true;
    g->recording.store(false);
}

void Loop::run_for(const KilnTaskRun* run, uint64_t budget_ns) {
    const uint64_t until = steady_ns() + budget_ns;
    while (!finished) {
        const uint64_t now = steady_ns();
        if (now >= next_gate) {
            gate_row();
            take_steps();
            next_gate = now + 10'000'000'000ull;
        }
        if (g->world_stopping.load()) return finish("the server world stopped", run);
        if (run && run->cancelled && run->cancelled(run)) return finish("cancelled", run);
        if (now >= deadline) return finish("the run's time is up", run);
        if (now >= until) break;
        if (now < warmup_end) {
            std::this_thread::sleep_for(std::chrono::nanoseconds(std::min<uint64_t>({warmup_end - now, until - now, 50'000'000ull})));
            continue;
        }
        if (!writes_utc) writes_utc = utc_ms();
        if (now < next_due) {
            std::this_thread::sleep_for(std::chrono::nanoseconds(std::min(next_due, until) - now));
            continue;
        }
        next_due = std::max(next_due + period, now);  // after a stall, no burst to catch up
        if (!pair(run)) return finish(end_reason, run);
    }
    take_steps();
    writes.flush();
    steps.flush();
    gate.flush();
}

// ---- the batches ----
void KILN_CALL batch_body(const KilnTaskRun* run, void*) {
    Loop& L = *g_loop;
    if (!L.started && !L.setup()) return;
    if (!L.finished) L.run_for(run, 250'000'000ull);
}
void KILN_CALL batch_done(KilnHandle task, uint32_t state, void*);
bool submit_batch() {
    KilnTaskInfo info{};
    info.size = sizeof info;
    info.flags = KILN_TASK_LONG_RUNNING;
    static const char name[] = "gate-priority-test loop";
    info.name = KilnStr{name, sizeof name - 1};
    KilnHandle task = 0;
    uint32_t refusal = 0;
    if (g->tasks->submit(g->owner, &info, &batch_body, &batch_done, nullptr, &task, &refusal) == KILN_OK) return true;
    say(KILN_LOG_WARN, "kiln.tasks refused the loop's task (refusal %u)", refusal);
    return false;
}
void KILN_CALL batch_done(KilnHandle task, uint32_t state, void*) {
    g->tasks->release(g->owner, task);
    Loop& L = *g_loop;
    if (state != KILN_TASK_COMPLETED && !L.finished) {
        say(KILN_LOG_WARN, "the loop's task ended in state %u; the run stops", state);
        L.end_reason = "the task ended in state " + std::to_string(state);
        if (L.started && state != KILN_TASK_FAULTED) L.finish(L.end_reason, nullptr);
        L.finished = true;
    }
    if (!L.finished) {
        if (submit_batch()) return;  // the next batch owns the loop from here on: nothing of it is touched below
        L.finish("kiln.tasks refused the next batch", nullptr);
    }
    g->recording.store(false);
    g->phase.store(Phase::Done);
}

// ---- the tick and the world ----
void look(const KilnTickInfo* tick) {
    KilnSavesStatus st{};
    st.size = sizeof st;
    if (!g->saves || g->saves->status(&st) != KILN_OK || !st.has_guid) return;
    const std::string guid = hex(st.world_guid, 16);
    const uint32_t session = g->server_session.load();
    if (g->config.world.empty() || guid != g->config.world) {
        if (g->logged_session != session) {
            g->logged_session = session;
            if (g->config.world.empty())
                say(KILN_LOG_INFO, "this world is %s; put world=%s in kiln/data/gate-priority-test/run.txt to run the "
                                   "test on it (it does nothing else until then)", guid.c_str(), guid.c_str());
            else
                say(KILN_LOG_INFO, "this world is %s, the run file names %s; the test stays off on this world", guid.c_str(),
                    g->config.world.c_str());
        }
        return;
    }
    static KilnPlayer list[KILN_PLAYERS_MAX_SLOTS];  // the tick thread's only
    uint32_t n = 0;
    if (!g->players) return;
    const KilnResult rc = g->players->list(KILN_PLAYERS_LIST_IN_GAME, list, KILN_PLAYERS_MAX_SLOTS, &n);
    KilnEntity player = 0;
    if (rc == KILN_OK || rc == KILN_ERR_BUFFER_TOO_SMALL)
        for (uint32_t i = 0; i < std::min<uint32_t>(n, KILN_PLAYERS_MAX_SLOTS) && !player; ++i)
            if (list[i].state == KILN_PLAYER_IN_GAME && list[i].entity) player = list[i].entity;
    if (!player && g->world->find && g->world->component_hash) {
        // kiln.players names nobody (yet): the first entity of the engine's player range (1..1023) with a transform
        static std::vector<KilnEntity> ids;  // the tick thread's only
        if (ids.empty()) ids.resize(1 << 16);
        static const char transform[] = "keen::ecs::CurrentTransform";
        uint32_t found = 0;
        const KilnResult frc = g->world->find(g->world->component_hash(KilnStr{transform, sizeof transform - 1}), ids.data(),
                                              static_cast<uint32_t>(ids.size()), &found);
        if (frc == KILN_OK || frc == KILN_ERR_BUFFER_TOO_SMALL)
            for (uint32_t i = 0; i < std::min<uint32_t>(found, static_cast<uint32_t>(ids.size())) && !player; ++i)
                if (ids[i] && ids[i] <= 1023) player = ids[i];
    }
    if (!player) return;
    g_loop = new Loop;
    g_loop->player = player;
    g->world_stopping.store(false);
    g->recording.store(true);
    g->phase.store(Phase::Armed);
    say(KILN_LOG_INFO, "world %s: starting the run with player %u at step %llu", guid.c_str(), player,
        static_cast<unsigned long long>(tick ? tick->step : 0));
    if (!submit_batch()) {
        g->recording.store(false);
        g->phase.store(Phase::Done);
    }
}
void KILN_CALL on_pre(const KilnTickInfo* tick, void*) {
    const uint64_t now = steady_ns();
    if (tick) g->edge_pre.store(tick->step, std::memory_order_release);
    if (g->recording.load(std::memory_order_acquire) && tick) {
        if (g->have_pre) g->steps.push({g->pre_step, g->pre_ns, 0, g->pre_utc});  // a step without a Post
        g->pre_step = tick->step;
        g->pre_ns = now;
        g->pre_utc = utc_ms();
        g->have_pre = true;
    }
    if (g->phase.load() == Phase::Waiting && now >= g->next_look_ns) {
        g->next_look_ns = now + 2'000'000'000ull;
        look(tick);
    }
}
void KILN_CALL on_post(const KilnTickInfo* tick, void*) {
    if (!tick) return;
    const uint64_t now = steady_ns();
    g->edge_post_ns.store(now, std::memory_order_relaxed);  // before the step: a reader of the step sees this time
    g->edge_post.store(tick->step, std::memory_order_release);
    if (!g->recording.load(std::memory_order_acquire) || !g->have_pre) return;
    g->steps.push({g->pre_step, g->pre_ns, tick->step == g->pre_step ? now : 0, g->pre_utc});
    g->have_pre = false;
}
void KILN_CALL on_world(const KilnWorldEvent* e, void*) {
    if (!e || e->kind != KILN_WORLD_SERVER) return;
    if (e->event == KILN_WORLD_STARTED) g->server_session.store(e->session);
    if (e->event == KILN_WORLD_STOPPING) g->world_stopping.store(true);
}

template <class T> const T* service(const char* name, uint32_t version, size_t need) {
    const auto* t = static_cast<const T*>(g->api->service(KilnStr{name, std::strlen(name)}, version));
    return t && t->header.size >= need ? t : nullptr;
}

}  // namespace
}  // namespace gpt

using namespace gpt;

extern "C" KILN_EXPORT int KILN_CALL kiln_native_init(const KilnNativeContext* ctx) {
    if (!ctx || ctx->size < sizeof(KilnNativeContext) || !ctx->api) return KILN_NATIVE_INIT_UNSUPPORTED;
    if (g) return KILN_NATIVE_INIT_OK;
    g = new Mod;
    g->api = ctx->api;
    g->owner = ctx->owner;
    g->version.assign(ctx->version.ptr ? ctx->version.ptr : "", ctx->version.len);
    const std::string data = path_of(KILN_PATH_DATA), logs = path_of(KILN_PATH_LOGS);
    if (data.empty() || logs.empty()) {
        say(KILN_LOG_WARN, "Kiln did not name its data and logs folders; the test stays off");
        return KILN_NATIVE_INIT_OK;
    }
    g->run_file = utf8_path(data) / "gate-priority-test" / "run.txt";
    g->logs = utf8_path(logs) / "gate-priority-test";
    std::error_code ec;
    if (!fs::exists(g->run_file, ec)) {
        say(KILN_LOG_INFO, "no run file (kiln/data/gate-priority-test/run.txt); the test stays off");
        return KILN_NATIVE_INIT_OK;
    }
    std::string text;
    {
        std::ifstream in(g->run_file, std::ios::binary);
        char buf[4096];
        in.read(buf, sizeof buf);
        text.assign(buf, static_cast<size_t>(in.gcount()));
    }
    g->config = parse_run(text);
    for (const auto& w : g->config.warnings) say(KILN_LOG_WARN, "run file: %s", w.c_str());
    if (!ctx->hooks_enabled) {
        say(KILN_LOG_WARN, "Kiln runs without hooks here (safe idle); the test stays off");
        return KILN_NATIVE_INIT_OK;
    }
    g->world = service<KilnWorldApi>(KILN_WORLD_SERVICE, KILN_WORLD_VERSION,
                                     offsetof(KilnWorldApi, ecs_write_unsubscribe) + sizeof(void*));
    g->tasks = service<KilnTasksApi>(KILN_TASKS_SERVICE, KILN_TASKS_VERSION, sizeof(KilnTasksApi));
    g->saves = service<KilnSavesApi>(KILN_SAVES_SERVICE, KILN_SAVES_VERSION, offsetof(KilnSavesApi, status) + sizeof(void*));
    g->players = service<KilnPlayersApi>(KILN_PLAYERS_SERVICE, KILN_PLAYERS_VERSION, offsetof(KilnPlayersApi, list) + sizeof(void*));
    g->types = service<KilnTypesApi>(KILN_TYPES_SERVICE, KILN_TYPES_VERSION, offsetof(KilnTypesApi, ecs_registry) + sizeof(void*));
    g->engine = service<KilnEngineApi>(KILN_ENGINE_SERVICE, KILN_ENGINE_VERSION, sizeof(KilnEngineApi));
    g->caps = service<KilnCapsApi>(KILN_CAPS_SERVICE, KILN_CAPS_VERSION, offsetof(KilnCapsApi, find) + sizeof(void*));
    const char* missing = !g->world    ? "kiln.world with compare-exchange writes"
                          : !g->tasks  ? "kiln.tasks"
                          : !g->saves  ? "kiln.saves"
                          : !g->players ? "kiln.players"
                          : !g->types  ? "kiln.types with its component registry"
                                       : nullptr;
    if (missing) {
        say(KILN_LOG_WARN, "%s is not here; the test stays off", missing);
        return KILN_NATIVE_INIT_OK;
    }
    if (g->world->capabilities && !(g->world->capabilities() & KILN_WORLD_CAP_ECS_WRITE))
        say(KILN_LOG_WARN, "kiln.world's compare-exchange writes are off on this build (see world.ecs_write in "
                           "kiln/state/capabilities.json); the run will report unavailable");
    KilnHandle h = 0;
    if (g->api->subscribe_tick(g->owner, KILN_TICK_PRE, &on_pre, nullptr, &h) != KILN_OK ||
        g->api->subscribe_tick(g->owner, KILN_TICK_POST, &on_post, nullptr, &h) != KILN_OK) {
        say(KILN_LOG_WARN, "no server tick here; the test stays off");
        return KILN_NATIVE_INIT_OK;
    }
    if (g->api->size >= offsetof(KilnApi, subscribe_world) + sizeof(void*) && g->api->subscribe_world)
        g->api->subscribe_world(g->owner, &on_world, nullptr, &h);
    g->phase.store(Phase::Waiting);
    std::string note;
    const std::string raise = raise_mode(&note);
    say(KILN_LOG_INFO, "armed: %s; waits for a server world and a player (priority raise %s)",
        g->config.world.empty() ? "no world named yet (it logs the world's GUID)" : ("world " + g->config.world).c_str(),
        raise.c_str());
    return KILN_NATIVE_INIT_OK;
}
