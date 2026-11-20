// SPDX-License-Identifier: MIT
// kiln-emf's disk thread (src/io_worker.h) and the slow-Pre line (src/slow_parts.h), host only: no game, no Wine.
//  - the tick's calls return while the disk is slow; a waiting file's newer text replaces it; jobs and writes keep their
//    order; flush waits; stop drains; the poll runs between jobs; without a thread, pump runs everything on the caller;
//  - SlowParts names the parts of a slow call, says nothing for a fast one, and gives at most one line a minute;
//  - the sources: the server world's per-tick path (dispatch_server_pre, commands_server_pre, game_server_pre's parts,
//    write_status, on_aside_pre) makes no file-system call (argv[1] = the plugin's src directory).
#include <atomic>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "io_worker.h"
#include "slow_parts.h"

namespace {
int checks = 0, failures = 0;
void check(bool ok, const char* what) {
    ++checks;
    if (!ok) {
        ++failures;
        std::printf("FAIL: %s\n", what);
    }
}

double ms_since(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

struct FakeDisk {
    std::mutex mu;
    std::vector<std::pair<std::string, std::string>> writes;  // in the order they reached the disk
    std::atomic<int> delay_ms{0};
    std::atomic<int> polls{0};
    bool write(const std::filesystem::path& p, const std::string& text) {
        if (int d = delay_ms.load()) std::this_thread::sleep_for(std::chrono::milliseconds(d));
        std::lock_guard lock(mu);
        writes.push_back({p.string(), text});
        return p.string() != "fail.json";
    }
};

kemf::io::Worker::Hooks hooks(FakeDisk& d, std::vector<std::string>* slow = nullptr, std::vector<std::string>* failed = nullptr) {
    kemf::io::Worker::Hooks h;
    h.write = [&d](const std::filesystem::path& p, const std::string& t) { return d.write(p, t); };
    h.poll = [&d] { ++d.polls; };
    h.poll_ms = 20;
    h.slow_ms = 100;
    if (slow) h.slow = [slow](const std::string& what, double) { slow->push_back(what); };
    if (failed) h.write_failed = [failed](const std::filesystem::path& p) { failed->push_back(p.string()); };
    return h;
}

void worker_tests() {
    {  // the caller never waits for a slow disk; the newer text of a waiting file wins; order is kept
        FakeDisk d;
        d.delay_ms = 300;
        std::vector<std::string> slow;
        kemf::io::Worker w(hooks(d, &slow));
        check(w.start(), "the worker starts");
        const auto t0 = std::chrono::steady_clock::now();
        w.write("emf.json", "1");  // taken at once by the thread: on the disk for 300 ms
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        w.write("emf.json", "2");
        w.write("other.ini", "a");
        w.write("emf.json", "3");  // replaces "2", which still waits
        std::atomic<bool> job_ran{false};
        w.post("a job", [&] { job_ran = true; });
        check(ms_since(t0) < 150, "write and post return while the disk takes 300 ms");
        check(w.coalesced() == 1, "a waiting file's newer text replaced it (one coalesced)");
        check(w.flush(5000), "flush waits until everything is on disk");
        check(ms_since(t0) >= 590, "flush waited for the slow disk (two more 300 ms writes)");
        check(job_ran.load(), "the posted job ran");
        std::lock_guard lock(d.mu);
        check(d.writes.size() == 3, "three writes reached the disk (2 was replaced by 3)");
        if (d.writes.size() == 3) {
            check(d.writes[0].second == "1" && d.writes[1].first == "emf.json" && d.writes[1].second == "3" &&
                      d.writes[2].first == "other.ini",
                  "in order: 1, then 3 in the place of 2, then other.ini");
        }
        check(!slow.empty() && slow.front() == "writing emf.json", "a write over slow_ms is told, by the file's name");
        w.stop(1000);
    }
    {  // a job and the writes around it keep their order (a world settings read after a write of the same file)
        FakeDisk d;
        kemf::io::Worker w(hooks(d));
        w.start();
        std::vector<std::string> seen;
        std::mutex mu;
        w.write("ws.ini", "old");
        w.post("read", [&] {
            std::lock_guard lock(d.mu);
            std::lock_guard l2(mu);
            seen.push_back(d.writes.empty() ? "nothing" : d.writes.back().second);
        });
        w.write("ws.ini", "new");
        check(w.flush(2000), "flush");
        check(seen.size() == 1 && seen[0] == "old", "the job ran after the write queued before it, before the one after");
        std::lock_guard lock(d.mu);
        check(d.writes.size() == 2 && d.writes[1].second == "new",
              "a write queued behind a job is not merged into the one before the job (both written)");
        w.stop(1000);
    }
    {  // the poll runs while idle; a failed write is told; stop drains what waits
        FakeDisk d;
        std::vector<std::string> failed;
        kemf::io::Worker w(hooks(d, nullptr, &failed));
        w.start();
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        check(d.polls.load() >= 3, "the poll runs about every poll_ms while nothing waits");
        d.delay_ms = 50;
        w.write("fail.json", "x");
        w.write("last.json", "y");
        w.stop(2000);
        check(!w.running(), "stopped");
        std::lock_guard lock(d.mu);
        check(d.writes.size() == 2, "stop wrote what was waiting");
        check(failed.size() == 1 && failed[0] == "fail.json", "a failed write is told once");
        const int polls = d.polls.load();
        std::this_thread::sleep_for(std::chrono::milliseconds(80));
        check(d.polls.load() == polls, "no poll after stop");
    }
    {  // without a thread (it never started): pump runs the work on the caller
        FakeDisk d;
        kemf::io::Worker w(hooks(d));
        w.write("a", "1");
        w.write("a", "2");
        w.post("j", [] {});
        check(w.waiting() == 2, "two waiting (the second text of a replaced the first)");
        check(w.pump() == 2, "pump runs both");
        check(w.flush(10), "flush without a thread pumps and returns");
        std::lock_guard lock(d.mu);
        check(d.writes.size() == 1 && d.writes[0].second == "2", "the last text was written");
    }
    {  // a job that throws does not end the thread
        FakeDisk d;
        kemf::io::Worker w(hooks(d));
        w.start();
        w.post("throws", [] { throw 1; });
        w.write("after", "ok");
        check(w.flush(2000), "flush after a throwing job");
        std::lock_guard lock(d.mu);
        check(d.writes.size() == 1, "the write after a throwing job ran");
        w.stop(500);
    }
}

void slow_parts_tests() {
    using kemf::SlowParts;
    const uint64_t ms = 1000000;  // ns per ms
    SlowParts p(100.0, 60000);
    p.begin(0);
    p.part("net", 1 * ms / 10);
    p.part("status", 50 * ms);
    check(!p.end(60 * ms, 1000).has_value(), "a 60 ms call gives no line");
    p.begin(0);
    p.part("net", ms / 10);
    p.part("commands", 2 * ms);
    p.part("status", 1931 * ms);
    auto l = p.end(1932 * ms, 2000);
    check(l.has_value(), "a 1.9 s call gives a line");
    if (l) {
        check(l->find("1932 ms") == 0, "the line starts with the total");
        check(l->find("status 1929 ms") != std::string::npos, "the slow part is named with its time");
        check(l->find("commands 2 ms") != std::string::npos, "a part over 1 ms is named");
        check(l->find("net") == std::string::npos, "a part under 1 ms is left out");
    }
    p.begin(0);
    p.part("status", 500 * ms);
    check(!p.end(500 * ms, 30000).has_value(), "a second slow call within a minute gives no line");
    p.begin(0);
    p.part("saves", 300 * ms);
    auto l2 = p.end(400 * ms, 62001);
    check(l2.has_value(), "a minute later: a line again");
    if (l2) {
        check(l2->find("saves 300 ms") != std::string::npos && l2->find("other 100 ms") != std::string::npos,
              "time outside the named parts is 'other'");
        check(l2->find("1 more slow call") != std::string::npos, "it counts the slow calls it left out");
    }
}

std::string read(const std::string& p) {
    std::ifstream f(p, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// The body of the first function whose definition starts with `head` (up to its closing brace at column 0).
std::string body_of(const std::string& src, const std::string& head) {
    const size_t a = src.find(head);
    if (a == std::string::npos) return {};
    const size_t b = src.find("\n}\n", a);
    return src.substr(a, b == std::string::npos ? std::string::npos : b - a);
}

void no_disk_on_tick(const std::string& dir) {
    const std::string plugin = read(dir + "/plugin.cpp"), commands = read(dir + "/commands.cpp"), game = read(dir + "/api_game.cpp");
    check(!plugin.empty() && !commands.empty() && !game.empty(), "the sources are readable");
    const char* kDisk[] = {"std::filesystem::exists", "std::filesystem::file_size", "std::filesystem::remove",
                           "std::filesystem::rename", "std::filesystem::create", "std::filesystem::last_write_time",
                           "read_file(", "write_file_atomic(", "ofstream", "ifstream", "MoveFile", "CreateFile", "fopen"};
    struct Fn {
        const std::string* src;
        const char* head;
    } fns[] = {
        {&plugin, "void dispatch_server_pre() {"},
        {&plugin, "void write_status() {"},
        {&commands, "void commands_server_pre() {"},
        {&game, "void game_server_pre(SlowParts* parts) {"},
        {&game, "void poll_saves() {"},
        {&game, "void apply_ws_queue() {"},
        {&game, "void poll_ready() {"},
    };
    for (const Fn& f : fns) {
        const std::string body = body_of(*f.src, f.head);
        check(!body.empty(), (std::string("found ") + f.head).c_str());
        for (const char* d : kDisk)
            check(body.find(d) == std::string::npos, (std::string(f.head) + " makes no disk call: " + d).c_str());
    }
    // on_aside_pre reads emf_core.log, but on the disk thread (inside io_post)
    const std::string aside = body_of(plugin, "void KILN_CALL on_aside_pre(");
    check(aside.find("io_post(") != std::string::npos && aside.find("read_file(") > aside.find("io_post("),
          "on_aside_pre reads emf_core.log inside io_post");
    // the kept world settings: read and written inside io_post
    const std::string load = body_of(game, "void persisted_settings_after_load(uint16_t slot) {");
    check(load.find("io_post(") != std::string::npos && load.find("read_file(") > load.find("io_post("),
          "a world load reads the kept settings inside io_post");
    const std::string save = body_of(game, "void save_persist_locked() {");
    check(save.find("io_post(") != std::string::npos && save.find("write_file_atomic(") > save.find("io_post("),
          "the kept settings are written inside io_post");
    // the chat path reads the parsed configuration, not the file
    const std::string admin = body_of(commands, "bool commands_is_admin(");
    check(admin.find("read_config()") == std::string::npos, "commands_is_admin does not read emf_core.ini");
    const std::string chat = body_of(commands, "bool KILN_CALL on_chat_line(");
    check(!chat.empty() && chat.find("read_config()") == std::string::npos, "on_chat_line does not read emf_core.ini");
}

}  // namespace

int main(int argc, char** argv) {
    worker_tests();
    slow_parts_tests();
    if (argc > 1) no_disk_on_tick(argv[1]);
    else check(false, "argv[1]: the plugin's src directory");
    std::printf("io_worker_test: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
