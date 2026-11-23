// SPDX-License-Identifier: MIT
// EmfApi v48 (EMF 0.6.20) and path_find through the production api_v40.cpp, against minimal fakes: no game. Built by
// tests/run.sh (ASan) and natively on Windows (MSVC, CMake target emf-path-test).
// v48 is the version only for the table (1160 bytes, as v47); EmfPathQuery gains as_entity (52 -> 56 bytes) and
// EmfPathResult the values the search planned with (32 -> 48). path_find is not carried: a mod built with EMF 0.6.19's
// header (a 52-byte query) and one built with 0.6.20's (56 bytes, with or without as_entity) get EMF's answer for a
// feature that is off on the game build: 0, logged, no callback, so no result of either size is ever handed out.
#include "emf_internal.h"
#include "v26_model.h"

#include <cstdio>
#include <cstring>
#include <memory>
#include <set>
#include <string>

static int checks = 0, failures = 0;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; std::printf("FAIL %d: %s\n", __LINE__, #x); } } while (0)
namespace kemf {
const KilnApi* g_api;
uint32_t g_side = KILN_SIDE_SERVER;
static Mod mod;
Mod* caller_mod(const void*) { return &mod; }
int unsupported_calls = 0, refusals = 0;
std::set<std::string> unsupported_fns;
std::string last_why;
void unsupported(const Mod*, const char* fn, const char* why) {
    ++unsupported_calls;
    if (fn) unsupported_fns.insert(fn);
    last_why = why ? why : "";
}
void refuse(const Mod*, const char*, const char*, bool) { ++refusals; }
void fill_terrain(EmfApi*) {}  // the block family (api_terrain.cpp) has its own tests
}  // namespace kemf
using namespace kemf;

// The layouts a mod's compiler gives these structs (EMF's api_sizes.h at 0.6.20).
static_assert(EMF_API_VERSION == 48 && kEmfApiVersion == 48 && sizeof(EmfApi) == 1160 && v26::kApiV48Size == 1160);
static_assert(v26::kPathQueryV37Size == 52 && offsetof(EmfPathQuery, as_entity) == 52 && sizeof(EmfPathQuery) == 56);
static_assert(v26::kPathResultV42Size == 32 && offsetof(EmfPathResult, moves) == 32 && offsetof(EmfPathResult, jump_up) == 34 &&
              offsetof(EmfPathResult, jump_down) == 35 && offsetof(EmfPathResult, width) == 36 &&
              offsetof(EmfPathResult, height) == 40 && v26::kPathResultV48Size == 44 && sizeof(EmfPathResult) == 48);

static int callbacks = 0;
static void path_cb(EmfWorld*, uint32_t, const EmfPathResult*, const EmfPathPoint*, void*) { ++callbacks; }

int main() {
    EmfApi t{};
    fill_v40(&t);
    CHECK(t.path_find && t.path_cancel);
    if (!t.path_find || !t.path_cancel) return 1;
    auto* w = reinterpret_cast<EmfWorld*>(uintptr_t{0x1000});
    const std::string not_yet = "not available on Kiln yet";

    EmfPathQuery q{};
    q.from[0] = 1;
    q.to[0] = 20;
    q.moves = EMF_PATH_JUMP_UP | EMF_PATH_OPEN_DOOR;
    q.closest = 1;
    q.max_nodes = 500;
    // a v47 caller: exactly 52 bytes on the heap, so a read past max_nodes would be caught (ASan on the host)
    q.size = v26::kPathQueryV37Size;
    auto v47 = std::make_unique<uint8_t[]>(v26::kPathQueryV37Size);
    std::memcpy(v47.get(), &q, v26::kPathQueryV37Size);
    CHECK(t.path_find(w, reinterpret_cast<const EmfPathQuery*>(v47.get()), &path_cb, &callbacks) == 0);
    CHECK(std::memcmp(v47.get(), &q, v26::kPathQueryV37Size) == 0);
    CHECK(unsupported_fns.count("path_find") == 1 && last_why == not_yet);
    // a v48 caller: 56 bytes, planning as a creature (any owner, any world): the same answer
    q.size = sizeof q;
    q.as_entity = 4242;
    const EmfPathQuery saved = q;
    CHECK(t.path_find(w, &q, &path_cb, &callbacks) == 0 && std::memcmp(&saved, &q, sizeof q) == 0);
    q.owner = 7;
    CHECK(t.path_find(w, &q, &path_cb, nullptr) == 0);
    CHECK(t.path_find(nullptr, &q, &path_cb, nullptr) == 0);
    q.as_entity = 0;  // a v48 query without as_entity
    CHECK(t.path_find(w, &q, &path_cb, nullptr) == 0);
    CHECK(t.path_find(w, nullptr, &path_cb, nullptr) == 0 && t.path_find(w, &q, nullptr, nullptr) == 0);
    CHECK(last_why == not_yet);
    CHECK(!t.path_cancel(0) && !t.path_cancel(1) && !t.path_cancel(UINT32_MAX));
    CHECK(unsupported_calls == 10);  // each call says so (the plugin's log keeps one line per mod and function)
    CHECK(unsupported_fns.size() == 2 && unsupported_fns.count("path_cancel") == 1);
    CHECK(callbacks == 0 && refusals == 0);
    std::printf("path_test (EmfApi v48): %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
