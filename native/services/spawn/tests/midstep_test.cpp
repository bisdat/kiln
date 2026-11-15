// SPDX-License-Identifier: MIT
// kiln.spawn's mid-step gate (midstep.h): create and destroy refuse while kiln.world's in_program answers 1, and
// spawn.cpp's create and destroy entry points go through it before any game call. Host only: no game, no Wine.
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>

#include "midstep.h"

namespace {
int checks = 0, failures = 0;
void check(bool ok, const char* what) {
    ++checks;
    if (!ok) {
        ++failures;
        std::printf("FAIL: %s\n", what);
    }
}

uint32_t g_inside = 0;
uint32_t KILN_CALL fake_in_program() { return g_inside; }

// The body of the first function whose definition starts with `head` (up to its closing brace at column 0).
std::string body_of(const std::string& src, const std::string& head) {
    const size_t at = src.find(head);
    if (at == std::string::npos) return {};
    const size_t end = src.find("\n}\n", at);
    return end == std::string::npos ? std::string() : src.substr(at, end - at);
}
}  // namespace

int main(int argc, char** argv) {
    using kspawn::Gate;
    KilnWorldApi w{};
    w.header = {sizeof(KilnWorldApi), KILN_WORLD_VERSION};
    w.in_program = &fake_in_program;

    check(kspawn::structural_gate(false, &w) == Gate::OffTick, "off the tick thread: refused as before");
    g_inside = 0;
    check(kspawn::structural_gate(true, &w) == Gate::Ok, "on the tick thread in Pre or Post: allowed");
    g_inside = 1;
    check(kspawn::structural_gate(true, &w) == Gate::MidStep,
          "on the tick thread inside a wrap stage, a Kiln program or a system callback: refused as mid-step");
    check(kspawn::structural_gate(false, &w) == Gate::OffTick, "a worker inside a stage is still off the tick thread");
    check(kspawn::structural_gate(true, nullptr) == Gate::Ok, "without kiln.world: the dev.28 behaviour");
    KilnWorldApi old = w;
    old.header.size = static_cast<uint32_t>(offsetof(KilnWorldApi, in_program));
    check(kspawn::structural_gate(true, &old) == Gate::Ok, "a kiln.world without in_program: the dev.28 behaviour");
    KilnWorldApi none = w;
    none.in_program = nullptr;
    check(kspawn::structural_gate(true, &none) == Gate::Ok, "a null in_program: the dev.28 behaviour");
    check(std::strstr(kspawn::kMidStepText, "slice.Post") && std::strstr(kspawn::kMidStepText, "Scheduler.Post"),
          "the refusal says how to defer the call");

    // spawn.cpp: every create and destroy entry point passes the gate; the reads do not.
    if (argc < 2) {
        std::printf("usage: midstep_test <spawn.cpp>\n");
        return 2;
    }
    std::ifstream in(argv[1]);
    std::stringstream ss;
    ss << in.rdbuf();
    const std::string src = ss.str();
    check(!src.empty(), "spawn.cpp is read");
    for (const char* f : {"KilnResult do_spawn(", "KilnResult i_destroy(", "KilnResult i_cleanup("}) {
        const std::string b = body_of(src, f);
        const size_t gate = b.find("structural_call(");
        const size_t game = b.find("game::");
        char what[128];
        std::snprintf(what, sizeof what, "%s passes the mid-step gate before any game call", f);
        check(!b.empty() && gate != std::string::npos && (game == std::string::npos || gate < game), what);
    }
    for (const char* f : {"KilnResult i_entity_info(", "KilnResult i_find(", "KilnResult i_refresh("}) {
        const std::string b = body_of(src, f);
        char what[128];
        std::snprintf(what, sizeof what, "%s (a read) keeps its dev.28 thread rule", f);
        check(!b.empty() && b.find("structural_call(") == std::string::npos, what);
    }
    // spawn and spawn_item create through do_spawn; nothing else creates
    check(body_of(src, "KilnResult i_spawn(").find("do_spawn(") != std::string::npos &&
              body_of(src, "KilnResult i_spawn_item(").find("do_spawn(") != std::string::npos,
          "spawn and spawn_item create through do_spawn");

    std::printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
