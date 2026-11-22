// SPDX-License-Identifier: MIT
// Host tests of developer debugging's descriptor and control worker (src/debug_descriptor.cpp, its plain C++ part):
// the descriptor's fields, the pipe protocol (challenge echo, wrong nonce, bad requests, the 16 KiB frame limit), the
// worker answering while a fake tick holds its lock (and a control snapshot that does take that lock, which must not
// answer), and start()'s decision: off = no descriptor, no pipe, no worker. Run by test-debug-descriptor.sh.
#include "../src/debug_descriptor.cpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

using namespace kiln::debug_descriptor;
using namespace std::chrono_literals;
using json = nlohmann::json;

namespace {

int g_failed = 0, g_passed = 0;
#define CHECK(cond)                                                                    \
    do {                                                                               \
        if (cond) {                                                                    \
            ++g_passed;                                                                \
        } else {                                                                       \
            ++g_failed;                                                                \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);      \
        }                                                                              \
    } while (0)

const char* kNonce = "0123456789abcdef0123456789abcdef";

Identity sample() {
    Identity id;
    id.pid = 4242;
    id.creation_100ns = 134070123456789017ull;  // above 2^53: a JSON number would lose its last digits
    id.image = "Z:\\srv\\labs\\a\\enshrouded_server.exe";
    id.image_sha256 = std::string(64, 'a');
    id.nonce = kNonce;
    id.runtime_version = "8.0.11";
    id.pipe = pipe_name(id.pid, id.nonce);
    id.side = "server";
    id.kiln = "0.2.0-dev";
    return id;
}

std::atomic<bool> t_ready{true};
std::atomic<int> t_mode{static_cast<int>(Mode::Hosted)};
bool ready_atomic() { return t_ready.load(); }
Mode mode_atomic() { return static_cast<Mode>(t_mode.load()); }
const Snapshot kSnap{&ready_atomic, &mode_atomic};

// ---- an in-memory transport ----
struct Pipe {
    std::mutex m;
    std::condition_variable cv;
    std::deque<char> to_server, to_client;
    bool client_closed = false, server_closed = false;
};

struct MemConn final : Conn {
    std::shared_ptr<Pipe> p;
    explicit MemConn(std::shared_ptr<Pipe> pipe) : p(std::move(pipe)) {}
    ~MemConn() override {
        std::lock_guard l(p->m);
        p->server_closed = true;
        p->cv.notify_all();
    }
    int read(char* buf, size_t n, int timeout_ms) override {
        std::unique_lock l(p->m);
        if (!p->cv.wait_for(l, std::chrono::milliseconds(timeout_ms), [&] { return !p->to_server.empty() || p->client_closed; })) return -1;
        if (p->to_server.empty()) return 0;
        size_t k = 0;
        while (k < n && !p->to_server.empty()) {
            buf[k++] = p->to_server.front();
            p->to_server.pop_front();
        }
        return static_cast<int>(k);
    }
    bool write(std::string_view d) override {
        std::lock_guard l(p->m);
        p->to_client.insert(p->to_client.end(), d.begin(), d.end());
        p->cv.notify_all();
        return true;
    }
};

struct MemListener final : Listener {
    std::mutex m;
    std::condition_variable cv;
    std::deque<std::shared_ptr<Pipe>> pending;
    bool stopped = false;
    std::unique_ptr<Conn> accept() override {
        std::unique_lock l(m);
        cv.wait(l, [&] { return !pending.empty() || stopped; });
        if (pending.empty()) return nullptr;
        auto p = pending.front();
        pending.pop_front();
        return std::make_unique<MemConn>(p);
    }
    std::shared_ptr<Pipe> connect() {
        auto p = std::make_shared<Pipe>();
        std::lock_guard l(m);
        pending.push_back(p);
        cv.notify_all();
        return p;
    }
    void stop() {
        std::lock_guard l(m);
        stopped = true;
        cv.notify_all();
    }
};

void send(Pipe& p, std::string_view s) {
    std::lock_guard l(p.m);
    p.to_server.insert(p.to_server.end(), s.begin(), s.end());
    p.cv.notify_all();
}

void close_client(Pipe& p) {
    std::lock_guard l(p.m);
    p.client_closed = true;
    p.cv.notify_all();
}

std::optional<std::string> recv_line(Pipe& p, std::chrono::milliseconds wait) {
    std::unique_lock l(p.m);
    auto has_line = [&] { return std::find(p.to_client.begin(), p.to_client.end(), '\n') != p.to_client.end(); };
    if (!p.cv.wait_for(l, wait, has_line)) return std::nullopt;
    std::string s;
    while (true) {
        const char c = p.to_client.front();
        p.to_client.pop_front();
        if (c == '\n') break;
        s.push_back(c);
    }
    return s;
}

bool server_closed(Pipe& p, std::chrono::milliseconds wait) {
    std::unique_lock l(p.m);
    return p.cv.wait_for(l, wait, [&] { return p.server_closed; });
}

std::string identify(std::string_view nonce, std::string_view challenge) {
    return json{{"v", 1}, {"op", "identify"}, {"nonce", nonce}, {"challenge", challenge}}.dump();
}

// ---- the tests ----

void descriptor_fields() {
    const Identity id = sample();
    const std::string text = descriptor_json(id, true, "2026-11-13T20:00:00Z");
    CHECK(!text.empty() && text.back() == '\n');
    const json d = json::parse(text);
    CHECK(d["format"] == 1);
    CHECK(d["windowsPid"] == 4242);
    CHECK(d["creationTime100ns"].is_string());
    CHECK(d["creationTime100ns"] == "134070123456789017");  // exact: a decimal string, no rounding
    CHECK(d["sessionNonce"] == kNonce);
    CHECK(d["pipe"] == "\\\\.\\pipe\\kiln-debug-4242-0123456789abcdef0123456789abcdef");
    CHECK(d["image"] == id.image && d["imageSha256"] == id.image_sha256);
    CHECK(d["side"] == "server" && d["kiln"] == "0.2.0-dev" && d["runtimeVersion"] == "8.0.11");
    CHECK(d["elevated"] == false && d["managedReady"] == true);
    CHECK(d["written"] == "2026-11-13T20:00:00Z");
    // exactly these keys (schema/state-debug.schema.json allows no others)
    CHECK(d.size() == 13);
    const uint8_t b[3] = {0x00, 0xab, 0xff};
    CHECK(to_hex(b, 3) == "00abff");
}

void protocol() {
    const Identity id = sample();
    const json ok = json::parse(answer(id, identify(kNonce, "challenge-0001"), kSnap));
    CHECK(ok["ok"] == true && ok["v"] == 1);
    CHECK(ok["challenge"] == "challenge-0001");
    CHECK(ok["identity"]["windowsPid"] == 4242);
    CHECK(ok["identity"]["creationTime100ns"] == "134070123456789017");
    CHECK(ok["identity"]["sessionNonce"] == kNonce);
    CHECK(ok["identity"]["image"] == id.image && ok["identity"]["imageSha256"] == id.image_sha256);
    CHECK(ok["managedReady"] == true && ok["sessionMode"] == "hosted");
    // a fresh challenge each time, echoed each time
    CHECK(json::parse(answer(id, identify(kNonce, "another-challenge"), kSnap))["challenge"] == "another-challenge");
    // the snapshot is read at every answer
    t_ready = false;
    t_mode = static_cast<int>(Mode::Menu);
    const json now = json::parse(answer(id, identify(kNonce, "challenge-0002"), kSnap));
    CHECK(now["managedReady"] == false && now["sessionMode"] == "menu");
    t_ready = true;
    t_mode = static_cast<int>(Mode::Hosted);
    // wrong nonce: refused, and no identity in the answer
    const json wrong = json::parse(answer(id, identify("ffffffffffffffffffffffffffffffff", "challenge-0003"), kSnap));
    CHECK(wrong["ok"] == false && wrong["code"] == "identity_changed");
    CHECK(!wrong.contains("identity") && !wrong.contains("challenge"));
    const json missing = json::parse(answer(id, R"({"v":1,"op":"identify","challenge":"challenge-0004"})", kSnap));
    CHECK(missing["code"] == "identity_changed");
    // bad requests
    CHECK(json::parse(answer(id, "not json", kSnap))["code"] == "bad_request");
    CHECK(json::parse(answer(id, "[1,2]", kSnap))["code"] == "bad_request");
    CHECK(json::parse(answer(id, R"({"v":2,"op":"identify"})", kSnap))["code"] == "bad_request");
    CHECK(json::parse(answer(id, R"({"v":1,"op":"run","nonce":"x"})", kSnap))["code"] == "unknown_op");
    for (const char* c : {"", "short", "has space in it", "\"quoted\"-challenge"})
        CHECK(json::parse(answer(id, identify(kNonce, c), kSnap))["code"] == "bad_request");
    CHECK(json::parse(answer(id, identify(kNonce, std::string(129, 'a')), kSnap))["code"] == "bad_request");
    CHECK(json::parse(answer(id, identify(kNonce, std::string(128, 'a')), kSnap))["ok"] == true);
}

void modes() {
    CHECK(mode_of(true, KILN_OK, KILN_SESSION_NONE) == Mode::Menu);
    CHECK(mode_of(true, KILN_OK, KILN_SESSION_DEDICATED) == Mode::Hosted);
    CHECK(mode_of(true, KILN_OK, KILN_SESSION_HOST) == Mode::Hosted);
    CHECK(mode_of(true, KILN_OK, KILN_SESSION_REMOTE) == Mode::Unknown);  // never "joined": no connection observer
    CHECK(mode_of(false, KILN_OK, KILN_SESSION_HOST) == Mode::Unknown);   // no world tracking (safe idle)
    CHECK(mode_of(true, KILN_ERR_UNAVAILABLE, KILN_SESSION_NONE) == Mode::Unknown);  // a faulted tick
    CHECK(mode_of(true, KILN_OK, 99) == Mode::Unknown);
    CHECK(std::string(mode_name(Mode::Joined)) == "joined");
}

void worker_lines_and_frames() {
    const Identity id = sample();
    MemListener l;
    std::thread w([&] { serve(l, id, kSnap); });
    // two requests in one write, one split over two writes, CRLF
    auto a = l.connect();
    send(*a, identify(kNonce, "challenge-a001") + "\n" + identify(kNonce, "challenge-a002") + "\r\n");
    const std::string split = identify(kNonce, "challenge-a003");
    send(*a, split.substr(0, 10));
    send(*a, split.substr(10) + "\n");
    for (const char* c : {"challenge-a001", "challenge-a002", "challenge-a003"}) {
        const auto r = recv_line(*a, 2s);
        CHECK(r && json::parse(*r)["challenge"] == c);
    }
    close_client(*a);
    CHECK(server_closed(*a, 2s));
    // a line of exactly 16384 bytes is a frame; 16385 is refused and the connection closed
    auto b = l.connect();
    std::string req = identify(kNonce, "challenge-b001");
    std::string at_limit = req + std::string(kMaxFrame - req.size(), ' ');
    CHECK(at_limit.size() == kMaxFrame);
    send(*b, at_limit + "\n");
    const auto rb = recv_line(*b, 2s);
    CHECK(rb && json::parse(*rb)["ok"] == true);
    send(*b, std::string(kMaxFrame + 1, ' '));
    const auto rbig = recv_line(*b, 2s);
    CHECK(rbig && json::parse(*rbig)["code"] == "frame_too_large");
    CHECK(server_closed(*b, 2s));
    // the next client is served as before
    auto c = l.connect();
    send(*c, identify(kNonce, "challenge-c001") + "\n");
    const auto rc = recv_line(*c, 2s);
    CHECK(rc && json::parse(*rc)["ok"] == true);
    close_client(*c);
    l.stop();
    w.join();
}

// The game's tick (or a thread stopped by the debugger) holds its lock: the worker still answers, because what it
// reads are atomics. A snapshot that takes that lock (as a tick-queued command would) does not answer: the control
// shows the test can fail.
std::mutex g_tick;
bool ready_under_tick() {
    std::lock_guard l(g_tick);
    return true;
}
const Snapshot kTickSnap{&ready_under_tick, &mode_atomic};

void worker_answers_while_the_tick_is_blocked() {
    const Identity id = sample();
    std::unique_lock tick(g_tick);  // the tick is blocked from here on
    MemListener l;
    std::thread w([&] { serve(l, id, kSnap); });
    auto a = l.connect();
    const auto t0 = std::chrono::steady_clock::now();
    send(*a, identify(kNonce, "challenge-t001") + "\n");
    const auto r = recv_line(*a, 2s);
    CHECK(r && json::parse(*r)["ok"] == true);
    CHECK(std::chrono::steady_clock::now() - t0 < 1s);
    close_client(*a);
    l.stop();
    w.join();
    // control: a snapshot behind the tick's lock gets no answer while the tick is blocked
    MemListener l2;
    std::thread w2([&] { serve(l2, id, kTickSnap); });
    auto b = l2.connect();
    send(*b, identify(kNonce, "challenge-t002") + "\n");
    CHECK(!recv_line(*b, 500ms).has_value());
    tick.unlock();  // the tick goes on: now it answers
    const auto late = recv_line(*b, 2s);
    CHECK(late && json::parse(*late)["ok"] == true);
    close_client(*b);
    l2.stop();
    w2.join();
}

// ---- start(): the decision and its order, with a counting platform ----
struct Calls {
    std::vector<std::string> log;
    bool identity_ok = true, listen_ok = true, write_ok = true, spawn_ok = true;
    std::string written;
};
Calls* g_calls = nullptr;

struct NullListener final : Listener {
    std::unique_ptr<Conn> accept() override { return nullptr; }
};

Platform counting() {
    Platform p;
    p.identity = [](Identity* out) {
        g_calls->log.push_back("identity");
        *out = sample();
        return g_calls->identity_ok;
    };
    p.listen = [](const std::string& pipe) -> std::unique_ptr<Listener> {
        g_calls->log.push_back("listen " + pipe);
        if (!g_calls->listen_ok) return nullptr;
        return std::make_unique<NullListener>();
    };
    p.write_descriptor = [](uint32_t pid, const std::string& text) {
        g_calls->log.push_back("write " + std::to_string(pid));
        g_calls->written = text;
        return g_calls->write_ok;
    };
    p.remove_descriptor = [](uint32_t pid) { g_calls->log.push_back("remove " + std::to_string(pid)); };
    p.spawn = [](std::unique_ptr<Listener> l, const Identity&, const Snapshot&) {
        g_calls->log.push_back(l ? "spawn" : "spawn without a listener");
        return g_calls->spawn_ok;
    };
    p.at_exit = [](uint32_t pid) { g_calls->log.push_back("at_exit " + std::to_string(pid)); };
    p.info = [](const std::string& line) { g_calls->log.push_back("info " + line.substr(0, 40)); };
    p.warn = [](const std::string& line) { g_calls->log.push_back("warn " + line.substr(0, 40)); };
    return p;
}

void start_off_makes_nothing() {
    Calls c;
    g_calls = &c;
    CHECK(!start_with(counting(), false, true, "2026-11-13T20:00:00Z", kSnap));
    CHECK(c.log.empty());  // no identity read, no pipe, no descriptor, no worker, not even a log line
    CHECK(c.written.empty());
    g_calls = nullptr;
}

void start_on_and_its_undo() {
    const std::string pipe = "listen " + pipe_name(4242, kNonce);
    {
        Calls c;
        g_calls = &c;
        CHECK(start_with(counting(), true, true, "2026-11-13T20:00:00Z", kSnap));
        const std::vector<std::string> want = {"identity", pipe, "write 4242", "spawn", "at_exit 4242"};
        CHECK(c.log.size() == 6 && std::equal(want.begin(), want.end(), c.log.begin()));
        CHECK(c.log.back().rfind("info ", 0) == 0);
        CHECK(json::parse(c.written)["managedReady"] == true);
    }
    {
        Calls c;  // the .NET host did not start: the descriptor says so
        g_calls = &c;
        CHECK(start_with(counting(), true, false, "2026-11-13T20:00:00Z", kSnap));
        CHECK(json::parse(c.written)["managedReady"] == false);
    }
    {
        Calls c;  // no identity: nothing else
        c.identity_ok = false;
        g_calls = &c;
        CHECK(!start_with(counting(), true, true, "t", kSnap));
        CHECK(c.log.size() == 2 && c.log[0] == "identity" && c.log[1].rfind("warn ", 0) == 0);
    }
    {
        Calls c;  // no pipe: no descriptor
        c.listen_ok = false;
        g_calls = &c;
        CHECK(!start_with(counting(), true, true, "t", kSnap));
        CHECK(c.log.size() == 3 && c.log[1] == pipe && c.log[2].rfind("warn ", 0) == 0);
    }
    {
        Calls c;  // the descriptor cannot be written: no worker
        c.write_ok = false;
        g_calls = &c;
        CHECK(!start_with(counting(), true, true, "t", kSnap));
        CHECK(c.log.size() == 4 && c.log[2] == "write 4242" && c.log[3].rfind("warn ", 0) == 0);
    }
    {
        Calls c;  // no worker: the descriptor is taken back
        c.spawn_ok = false;
        g_calls = &c;
        CHECK(!start_with(counting(), true, true, "t", kSnap));
        CHECK(c.log.size() == 6 && c.log[3] == "spawn" && c.log[4] == "remove 4242" && c.log[5].rfind("warn ", 0) == 0);
    }
    g_calls = nullptr;
}

}  // namespace

int main() {
    descriptor_fields();
    protocol();
    modes();
    worker_lines_and_frames();
    worker_answers_while_the_tick_is_blocked();
    start_off_makes_nothing();
    start_on_and_its_undo();
    std::printf("debug_descriptor_test: %d passed, %d failed\n", g_passed, g_failed);
    if (g_failed) return 1;
    std::printf("PASS debug.descriptor-host\n");
    return 0;
}
