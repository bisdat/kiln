// SPDX-License-Identifier: MIT
// kiln.spawn's mid-step gate (midstep.h), in the three cases the world's step can reach it on the world's own thread:
// a program wrap's stage and a Kiln program's callback (kiln.world's in_program answers 1; kiln.world's host test proves
// it for both, in_program_rows and the client-world program rows): refused; a kiln.types system-hook callback
// (in_system_callback answers 1, and in_program with it): allowed as on dev.28, warned once per call kind. And spawn.cpp's
// create and destroy entry points go through the gate before any game call. Host only: no game, no Wine.
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

uint32_t g_inside = 0, g_hook = 0;
uint32_t KILN_CALL fake_in_program() { return g_inside || g_hook; }  // kiln.world's: it includes the system-hook part
uint32_t KILN_CALL fake_in_system_callback() { return g_hook; }

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
    KilnTypesApi t{};
    t.header = {sizeof(KilnTypesApi), KILN_TYPES_VERSION};
    t.in_system_callback = &fake_in_system_callback;

    check(kspawn::structural_gate(false, &w, &t) == Gate::OffTick, "off the tick thread: refused as before");
    check(kspawn::structural_gate(true, &w, &t) == Gate::Ok, "on the tick thread in Pre or Post: allowed");
    // case 1: a program wrap's stage on the world's own thread (t_in_slice: in_program 1, no system hook)
    g_inside = 1;
    check(kspawn::structural_gate(true, &w, &t) == Gate::MidStep, "a wrap stage on the world's thread: refused");
    check(kspawn::structural_gate(false, &w, &t) == Gate::OffTick, "a worker inside a stage is still off the tick thread");
    // case 2: a Kiln ECS program's callback, server, client or prediction world (t_in_program: the same answer)
    check(kspawn::structural_gate(true, &w, &t) == Gate::MidStep, "a Kiln program's callback on the world's thread: refused");
    g_inside = 0;
    // case 3: a kiln.types system-hook callback (in_system_callback 1, and kiln.world's in_program with it)
    g_hook = 1;
    check(kspawn::structural_gate(true, &w, &t) == Gate::InSystemHook,
          "a system-hook callback on the world's thread: allowed as on dev.28 (warned), not refused");
    check(kspawn::structural_gate(true, &w, nullptr) == Gate::MidStep,
          "without kiln.types the hook cannot be told from a stage: in_program's answer stands (refused)");
    KilnTypesApi old_types = t;
    old_types.header.size = static_cast<uint32_t>(offsetof(KilnTypesApi, in_system_callback));
    check(kspawn::structural_gate(true, &w, &old_types) == Gate::MidStep,
          "a kiln.types without in_system_callback: in_program's answer stands");
    g_hook = 0;
    check(kspawn::structural_gate(true, nullptr, nullptr) == Gate::Ok, "without kiln.world: the dev.28 behaviour");
    KilnWorldApi old = w;
    old.header.size = static_cast<uint32_t>(offsetof(KilnWorldApi, in_program));
    g_inside = 1;
    check(kspawn::structural_gate(true, &old, &t) == Gate::Ok, "a kiln.world without in_program: the dev.28 behaviour");
    KilnWorldApi none = w;
    none.in_program = nullptr;
    check(kspawn::structural_gate(true, &none, &t) == Gate::Ok, "a null in_program: the dev.28 behaviour");
    g_inside = 0;
    // the warning in a system hook: once per call kind
    kspawn::OncePerKind once;
    check(once.first(kspawn::kKindSpawn) && !once.first(kspawn::kKindSpawn) && once.first(kspawn::kKindDestroy) &&
              !once.first(kspawn::kKindDestroy) && once.first(kspawn::kKindSpawnItem) && once.first(kspawn::kKindCleanup) &&
              !once.first(kspawn::kKindSpawn),
          "a system-hook call is warned once per call kind");
    check(std::strstr(kspawn::kMidStepText, "slice.Post") && std::strstr(kspawn::kMidStepText, "Scheduler.Post"),
          "the refusal says how to defer the call");
    check(std::strstr(kspawn::kSystemHookText, "middle of the world's step") &&
              std::strstr(kspawn::kSystemHookText, "worker threads") && std::strstr(kspawn::kSystemHookText, "Scheduler.Post"),
          "the system-hook warning says what it is, why it is unsafe and how to defer the call");

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
    check(body_of(src, "KilnResult structural_call(").find("case Gate::InSystemHook:") != std::string::npos &&
              body_of(src, "KilnResult structural_call(").find("return KILN_OK;") != std::string::npos,
          "spawn.cpp lets a system-hook call through (with its warning)");
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
