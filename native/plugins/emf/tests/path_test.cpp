// SPDX-License-Identifier: MIT
// Pinned EMF 0.6.22 v49 ABI and production path refusals, with no game or engine services.
// v47/v48/v49 queries (52/56/60 bytes) must never be mistaken for supported planning. ASan checks exact old
// allocations; no callback is allowed. Move orders (v40/v41, 128/136 bytes) are carried since 0.2.0-dev.31.13:
// tests/orders_test.cpp (api_orders.cpp), and fill_v40 leaves their slots to fill_orders.
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
void fill_terrain(EmfApi*) {}  // api_terrain.cpp has its own tests
}  // namespace kemf
using namespace kemf;

// Upstream sdk/include/emf/emf_api.h and src/core/api_sizes.h at a47284006f0eb22240639f8870bbc5ff5fcb8ef2.
static_assert(EMF_API_VERSION == 49 && kEmfApiVersion == 49 && sizeof(EmfApi) == 1160 && v26::kApiV49Size == 1160);
static_assert(v26::kPathQueryV37Size == 52 && offsetof(EmfPathQuery, as_entity) == 52 && v26::kPathQueryV48Size == 56);
static_assert(offsetof(EmfPathQuery, as_state) == 56 && v26::kPathQueryV49Size == 60 && sizeof(EmfPathQuery) == 60);
static_assert(v26::kPathResultV42Size == 32 && offsetof(EmfPathResult, moves) == 32 && offsetof(EmfPathResult, jump_up) == 34 &&
              offsetof(EmfPathResult, jump_down) == 35 && offsetof(EmfPathResult, width) == 36 &&
              offsetof(EmfPathResult, height) == 40 && v26::kPathResultV48Size == 44 && sizeof(EmfPathResult) == 48);
static_assert(offsetof(EmfMoveOrder, why) == 32 && v26::kMoveOrderV40Size == 128 && sizeof(EmfMoveOrder) == 136);

static int callbacks = 0;
static void path_cb(EmfWorld*, uint32_t, const EmfPathResult*, const EmfPathPoint*, void*) { ++callbacks; }

int main() {
    EmfApi t{};
    fill_v40(&t);
    CHECK(t.path_find && t.path_cancel && !t.enemy_move && !t.enemy_move_cancel && !t.enemy_moving);
    if (!t.path_find || !t.path_cancel) return 1;
    auto* w = reinterpret_cast<EmfWorld*>(uintptr_t{0x1000});
    const std::string not_yet = "not available on Kiln yet";

    EmfPathQuery q{};
    q.from[0] = 1;
    q.to[0] = 20;
    q.moves = EMF_PATH_JUMP_UP | EMF_PATH_OPEN_DOOR;
    q.closest = 1;
    q.max_nodes = 500;
    q.as_entity = 4242;
    // Real old-size heap buffers: neither as_entity nor as_state may be read beyond the caller's allocation.
    for (const uint32_t size : {v26::kPathQueryV37Size, v26::kPathQueryV48Size, v26::kPathQueryV49Size}) {
        q.size = size;
        auto old = std::make_unique<uint8_t[]>(size);
        std::memcpy(old.get(), &q, size);
        const int before = unsupported_calls;
        CHECK(t.path_find(w, reinterpret_cast<const EmfPathQuery*>(old.get()), &path_cb, &callbacks) == 0);
        CHECK(std::memcmp(old.get(), &q, size) == 0);
        CHECK(unsupported_calls == before + 1 && last_why == not_yet);
    }
    // Calm, Guard and unrecognized states are all explicit no-action refusals, including Guard without a creature.
    q.size = sizeof q;
    for (const uint32_t state : {EMF_PATH_AS_CALM, EMF_PATH_AS_GUARD, 2u, UINT32_MAX}) {
        for (const uint32_t entity : {0u, 4242u}) {
            q.as_entity = entity;
            q.as_state = state;
            q.moves = UINT16_MAX;  // unknown movement bits cannot be silently masked into a supported path
            const EmfPathQuery saved = q;
            const int before = unsupported_calls;
            CHECK(t.path_find(w, &q, &path_cb, nullptr) == 0);
            CHECK(std::memcmp(&saved, &q, sizeof q) == 0);
            CHECK(unsupported_calls == before + 1 && last_why == not_yet);
        }
    }
    // Unsupported means no query reads at all, even for short or partial-extension sizes, no world or no callback.
    for (const uint32_t size : {0u, 4u, 51u, 53u, 55u, 57u, 59u, 64u, UINT32_MAX}) {
        q.size = size;
        CHECK(t.path_find(w, &q, &path_cb, nullptr) == 0 && last_why == not_yet);
    }
    CHECK(t.path_find(nullptr, &q, &path_cb, nullptr) == 0);
    CHECK(t.path_find(w, nullptr, &path_cb, nullptr) == 0 && t.path_find(w, &q, nullptr, nullptr) == 0);
    CHECK(!t.path_cancel(0) && !t.path_cancel(1) && !t.path_cancel(UINT32_MAX));

    CHECK(callbacks == 0 && refusals == 0);
    for (const char* fn : {"path_find", "path_cancel"})
        CHECK(unsupported_fns.count(fn) == 1);
    std::printf("path_test (EmfApi v49, v47/v48 callers, paths refused): %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
