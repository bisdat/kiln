// SPDX-License-Identifier: MIT
// kiln.repl.net host tests (no game; clang-cl, run under plain Wine by tests/run.sh, so SEH is the real thing): the
// layout of kiln.repl.net's records; the item remap over a component's sites (game ids and 0 stay, a mapped Kiln-only id
// becomes its look-alike, anything else becomes 0, arrays by stride, never past the component); the list-1 mask test
// of the snapshot hook (versioned components, a malformed list); a filter call under its own guard (a forced access
// violation and a filter that says it failed both count as a failure, a normal one does not).
#include <windows.h>

#include <cstddef>
#include <cstdio>
#include <cstring>
#include <vector>

#include "../snapcore.h"
#include "services/kiln_repl.h"

namespace {

int g_checks = 0, g_failed = 0;
void check(bool ok, const char* what) {
    ++g_checks;
    if (!ok) {
        ++g_failed;
        std::printf("FAIL %s\n", what);
    }
}
#define CHECK(x) check((x), #x)

uint32_t u32(const std::vector<uint8_t>& d, size_t off) {
    uint32_t v;
    std::memcpy(&v, d.data() + off, 4);
    return v;
}
void put(std::vector<uint8_t>& d, size_t off, uint32_t v) { std::memcpy(d.data() + off, &v, 4); }

void abi() {
    static_assert(offsetof(KilnReplNetApi, capabilities) == 8 && offsetof(KilnReplNetApi, remove) == 16 &&
                  offsetof(KilnReplNetApi, track) == 24 && offsetof(KilnReplNetApi, filter_add) == 40 &&
                  offsetof(KilnReplNetApi, touch) == 56 && offsetof(KilnReplNetApi, remap_set) == 64 &&
                  offsetof(KilnReplNetApi, message_hook) == 88 && offsetof(KilnReplNetApi, schema_find) == 104 &&
                  offsetof(KilnReplNetApi, schema_encode) == 128 && offsetof(KilnReplNetApi, peers) == 136);
    static_assert(offsetof(KilnReplFilterCtx, entity) == 12 && offsetof(KilnReplFilterCtx, component_hash) == 20 &&
                  offsetof(KilnReplFilterCtx, failed) == 32);
    static_assert(offsetof(KilnReplItemSite, offset) == 4 && offsetof(KilnReplItemSite, stride) == 8);
    CHECK(KILN_REPL_NET_CAP_FILTER == 4 && KILN_REPL_NET_CAP_REMAP == 16 && KILN_REPL_TRACK_GAP == 5);
}

void remap() {
    // a 40-byte component: one id at 0, an array of 3 ids at 8 (stride 8), one id at 36
    const repl::RemapSite sites[] = {{0, 1, 0}, {8, 3, 8}, {36, 1, 0}};
    const uint32_t game[] = {100, 200, 300};                   // sorted
    const KilnReplItemMap map[] = {{5000, 200}, {6000, 0}, {7000, 999}};  // sorted; 999 is not a game item
    std::vector<uint8_t> d(40, 0xee);
    put(d, 0, 5000);   // mapped -> 200
    put(d, 8, 100);    // game id stays
    put(d, 16, 6000);  // mapped to 0 (no look-alike) -> 0
    put(d, 24, 0);     // empty stays
    put(d, 36, 8000);  // neither a game item nor mapped -> 0 (fails closed)
    put(d, 4, 5000);   // not a site: untouched
    uint64_t remapped = 0, zeroed = 0;
    repl::remap_ids(sites, 3, game, 3, map, 3, d.data(), 40, &remapped, &zeroed);
    CHECK(u32(d, 0) == 200);
    CHECK(u32(d, 8) == 100);
    CHECK(u32(d, 16) == 0);
    CHECK(u32(d, 24) == 0);
    CHECK(u32(d, 36) == 0);
    CHECK(u32(d, 4) == 5000);
    CHECK(remapped == 1 && zeroed == 2);
    // a look-alike that is not a game item is not trusted: 0
    put(d, 0, 7000);
    remapped = zeroed = 0;
    repl::remap_ids(sites, 1, game, 3, map, 3, d.data(), 40, &remapped, &zeroed);
    CHECK(u32(d, 0) == 0 && zeroed == 1 && remapped == 0);
    // never past the component: a site array that would run out of a 20-byte component stops at its end
    std::vector<uint8_t> small(20, 0);
    put(small, 8, 8000);
    put(small, 16, 8000);
    remapped = zeroed = 0;
    repl::remap_ids(sites + 1, 1, game, 3, map, 3, small.data(), 20, &remapped, &zeroed);
    CHECK(u32(small, 8) == 0 && u32(small, 16) == 0 && zeroed == 2);
    // zero_ids: every site of the component, nothing else
    std::vector<uint8_t> z(40, 0x11);
    repl::zero_ids(sites, 3, z.data(), 40);
    CHECK(u32(z, 0) == 0 && u32(z, 8) == 0 && u32(z, 16) == 0 && u32(z, 24) == 0 && u32(z, 36) == 0);
    CHECK(u32(z, 4) == 0x11111111u && u32(z, 12) == 0x11111111u);
}

// a state blob {u16 size, u16 count, u32 tick, list 1 (u16 index [+ u8 version])...}
std::vector<uint8_t> state(const std::vector<uint16_t>& idx, const std::vector<bool>& versioned) {
    std::vector<uint8_t> s(8, 0);
    for (uint16_t i : idx) {
        s.push_back(static_cast<uint8_t>(i));
        s.push_back(static_cast<uint8_t>(i >> 8));
        if (versioned[i]) s.push_back(7);
    }
    s.resize(s.size() + 16, 0);
    const uint16_t total = static_cast<uint16_t>(s.size()), count = static_cast<uint16_t>(idx.size());
    std::memcpy(s.data(), &total, 2);
    std::memcpy(s.data() + 2, &count, 2);
    return s;
}

void mask_test() {
    std::vector<bool> versioned(640, false);
    versioned[3] = true;
    auto ver = [&](uint32_t i) { return static_cast<bool>(versioned[i]); };
    uint64_t mask[10] = {};
    mask[0] |= 1ull << 9;    // a filter on index 9
    mask[1] |= 1ull << 1;    // and on index 65
    CHECK(repl::list1_match(mask, state({1, 2, 3}, versioned).data(), 640, ver) == 0);
    CHECK(repl::list1_match(mask, state({3, 9}, versioned).data(), 640, ver) == 1);   // after a versioned index
    CHECK(repl::list1_match(mask, state({65}, versioned).data(), 640, ver) == 1);
    CHECK(repl::list1_match(mask, state({}, versioned).data(), 640, ver) == 0);
    // an index past the registry: malformed
    CHECK(repl::list1_match(mask, state({700}, versioned).data(), 640, ver) == -1);
    // a count longer than the blob: malformed
    std::vector<uint8_t> cut = state({1, 2}, versioned);
    const uint16_t total = 10, count = 40;
    std::memcpy(cut.data(), &total, 2);
    std::memcpy(cut.data() + 2, &count, 2);
    CHECK(repl::list1_match(mask, cut.data(), 640, ver) == -1);
}

int g_calls = 0;
void KILN_CALL ok_filter(uint8_t* s, uint32_t n, const KilnReplFilterCtx*, void*) {
    ++g_calls;
    if (n) s[0] = 0x42;
}
void KILN_CALL faulting_filter(uint8_t* s, uint32_t, const KilnReplFilterCtx*, void*) {
    ++g_calls;
    s[0] = 0x43;
    *reinterpret_cast<volatile int*>(static_cast<uintptr_t>(0x10)) = 1;  // access violation
}
void KILN_CALL failing_filter(uint8_t*, uint32_t, const KilnReplFilterCtx* ctx, void*) {
    ++g_calls;
    *ctx->failed = 1;
}

void filter_calls() {
    uint8_t scratch[8] = {};
    KilnReplFilterCtx ctx{};
    ctx.size = sizeof ctx;
    uintptr_t where = 0;
    CHECK(repl::call_filter(&ok_filter, scratch, 8, &ctx, nullptr, &where) && scratch[0] == 0x42 && where == 0);
    CHECK(!repl::call_filter(&faulting_filter, scratch, 8, &ctx, nullptr, &where) && where != 0);
    where = 0;
    CHECK(!repl::call_filter(&failing_filter, scratch, 8, &ctx, nullptr, &where));
    CHECK(g_calls == 3);
}

}  // namespace

int main() {
    abi();
    remap();
    mask_test();
    filter_calls();
    std::printf("repl.net host tests: %d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
