// SPDX-License-Identifier: MIT
// The core's world-stop drain per world kind (src/tick_drain.h) and hk_world_destroy's use of it (src/tick.cpp): the
// engine's WaitWorldJobs for the server and the client world before any STOPPING, the prediction world after the client
// world's wait. Host only: the decision is pure; tick.cpp is checked as source (its hooks need the game).
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

#include "../src/tick_drain.h"

namespace {
int checks = 0, failures = 0;
void check(bool ok, const char* what) {
    ++checks;
    if (!ok) {
        ++failures;
        std::printf("FAIL: %s\n", what);
    }
}
}  // namespace

int main(int argc, char** argv) {
    using kiln::tick::StopDrain;
    using kiln::tick::stop_drain_of;
    using kiln::tick::waits_before_stop;
    constexpr uint32_t S = KILN_WORLD_SERVER, P = KILN_WORLD_PREDICTION, C = KILN_WORLD_CLIENT;
    check(stop_drain_of(S, KILN_SIDE_SERVER, true, true, true) == StopDrain::Engine,
          "a dedicated server's world: drained by the engine");
    check(stop_drain_of(C, KILN_SIDE_SERVER, true, true, true) == StopDrain::NoWorld &&
              stop_drain_of(P, KILN_SIDE_SERVER, true, true, true) == StopDrain::NoWorld,
          "no client or prediction world in a dedicated server");
    check(stop_drain_of(S, KILN_SIDE_CLIENT, true, true, true) == StopDrain::Engine &&
              stop_drain_of(C, KILN_SIDE_CLIENT, true, true, true) == StopDrain::Engine &&
              stop_drain_of(P, KILN_SIDE_CLIENT, true, true, true) == StopDrain::Engine,
          "a client: every kind drained by the engine (the client world's own wait, the prediction world with it)");
    check(stop_drain_of(C, KILN_SIDE_CLIENT, true, true, false) == StopDrain::Bounded &&
              stop_drain_of(S, KILN_SIDE_SERVER, true, true, false) == StopDrain::Bounded,
          "without WaitWorldJobs: a bounded wait only");
    check(stop_drain_of(C, KILN_SIDE_CLIENT, false, true, true) == StopDrain::None &&
              stop_drain_of(S, KILN_SIDE_SERVER, true, false, true) == StopDrain::None,
          "a tick or destroy hook that is not there or faulted: no stop at all");
    check(stop_drain_of(7, KILN_SIDE_CLIENT, true, true, true) == StopDrain::NoWorld, "an unknown kind has no world");
    check(waits_before_stop(S) && waits_before_stop(C) && !waits_before_stop(P),
          "the server and the client world objects are waited; the prediction world is not a world object");

    // tick.cpp: hk_world_destroy waits (when WaitWorldJobs resolved) for every kind waits_before_stop names, before the
    // first STOPPING it dispatches (the prediction world's, then the world's own)
    if (argc < 2) {
        std::printf("usage: tick_drain_test <tick.cpp>\n");
        return 2;
    }
    std::ifstream in(argv[1]);
    std::stringstream ss;
    ss << in.rdbuf();
    const std::string src = ss.str();
    const size_t fn = src.find("void __fastcall hk_world_destroy(");
    const size_t end = fn == std::string::npos ? fn : src.find("\n}\n", fn);
    const std::string body = fn == std::string::npos || end == std::string::npos ? std::string() : src.substr(fn, end - fn);
    const size_t wait = body.find("if (waits_before_stop(kind) && g_wait_jobs) {");
    const size_t call = body.find("g_wait_jobs(world);", wait == std::string::npos ? 0 : wait);
    const size_t first_stop = body.find("dispatch_event(KILN_WORLD_STOPPING");
    check(!body.empty() && wait != std::string::npos && call != std::string::npos && first_stop != std::string::npos &&
              wait < first_stop && call < first_stop,
          "hk_world_destroy calls WaitWorldJobs for waits_before_stop kinds before any STOPPING");
    check(body.find("kind == KILN_WORLD_SERVER && g_wait_jobs") == std::string::npos,
          "hk_world_destroy no longer waits for the server world only");
    std::printf("tick drain: %s %d checks, %d failures\n", failures ? "FAIL" : "PASS", checks, failures);
    return failures ? 1 : 0;
}
