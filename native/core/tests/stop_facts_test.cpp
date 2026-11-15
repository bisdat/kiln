// SPDX-License-Identifier: MIT
// The stop's facts (src/stop_facts.h) under the thread sanitizer: boot() changes and publishes its facts while another
// thread's stop extends and serialises them. `stop_facts_test old` runs the 0.2.0-dev.29 round-2 pattern instead (an
// atomic pointer to the live facts, read without a lock): the sanitizer reports the race, so that run fails.
// Run by test-stop-facts.sh.
#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "stop_facts.h"

struct Facts {
    std::string started;
    std::vector<std::string> errors;
};

std::string serialise(const Facts& f) {  // what write_boot_json reads
    std::string s = f.started;
    for (const std::string& e : f.errors) s += "|" + e;
    return s;
}

int main(int argc, char** argv) {
    const bool old = argc > 1 && std::strcmp(argv[1], "old") == 0;
    std::atomic<bool> go{false}, written{false};
    std::string doc;
    if (old) {
        Facts f;  // boot()'s frame
        std::atomic<Facts*> p{&f};
        std::thread stop([&] {
            while (!go.load()) std::this_thread::yield();
            Facts* g = p.load();
            g->errors.push_back("the roster holds at most 128 NPCs");  // the writer's line
            doc = serialise(*g);
            written = true;
        });
        go = true;
        for (int i = 0; i < 2000; ++i) f.errors.push_back("plugin " + std::to_string(i) + ": failed");  // boot() goes on
        stop.join();
        std::printf("stop_facts_test old: %zu bytes written from the live facts\n", doc.size());
        return 0;  // the sanitizer's report is the failure
    }
    kiln::FactsBox<Facts> box;
    Facts f;
    f.started = "2026-11-15T12:00:00Z";
    box.publish(f);
    std::thread stop([&] {
        while (!go.load()) std::this_thread::yield();
        const bool ok = box.with([&](Facts& g) {
            g.errors.push_back("the roster holds at most 128 NPCs");
            doc = serialise(g);
        });
        written = ok;
    });
    go = true;
    for (int i = 0; i < 2000; ++i) {
        f.errors.push_back("plugin " + std::to_string(i) + ": failed");  // boot() changes its own facts
        box.publish(f);                                                   // and publishes them
    }
    stop.join();
    int fail = 0;
    if (!written || doc.find("2026-11-15T12:00:00Z") != 0 || doc.find("the roster holds at most 128 NPCs") == std::string::npos) {
        std::printf("FAIL the stop's document: %s\n", doc.substr(0, 120).c_str());
        ++fail;
    }
    kiln::FactsBox<Facts> empty;  // before boot() published anything: the writer writes nothing
    if (empty.with([](Facts&) {})) {
        std::printf("FAIL an empty box ran the writer\n");
        ++fail;
    }
    // a stop on the boot thread itself (boot() is inside the call, not inside publish): no deadlock
    if (!box.with([&](Facts& g) { g.errors.push_back("same thread"); })) ++fail;
    std::printf("stop_facts_test: %s (%zu bytes written while boot() published)\n", fail ? "FAILED" : "PASS", doc.size());
    return fail ? 1 : 0;
}
