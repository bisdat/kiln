// SPDX-License-Identifier: MIT
// kiln.repl.net's snapshot step as pure functions (no game call, no global state), shared by filter.cpp, remap.cpp and
// the host tests (tests/repl_net_test.cpp): the item remap over one component's sites, the list-1 mask test, one
// filter call under its own guard. Not an ABI.
#pragma once
#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <cstring>

#include "services/kiln_repl.h"

namespace repl {

struct RemapSite {
    uint16_t offset, count, stride;
};

// The ids at one component's sites (data: size bytes): 0 and a game id (game: sorted) stay; a Kiln-only id in map
// (sorted by from) becomes its look-alike when that is a game id; anything else becomes 0. Never reads or writes past
// size. remapped / zeroed count the ids replaced.
inline void remap_ids(const RemapSite* sites, uint32_t n, const uint32_t* game, size_t ngame, const KilnReplItemMap* map,
                      size_t nmap, uint8_t* data, uint32_t size, uint64_t* remapped, uint64_t* zeroed) {
    const auto is_game = [&](uint32_t id) { return std::binary_search(game, game + ngame, id); };
    for (uint32_t k = 0; k < n; ++k) {
        const RemapSite& s = sites[k];
        const uint32_t count = s.count ? s.count : 1;
        for (uint32_t j = 0; j < count; ++j) {
            const uint64_t off = uint64_t{s.offset} + uint64_t{j} * s.stride;
            if (off + 4 > size) break;
            uint32_t id;
            std::memcpy(&id, data + off, 4);
            if (!id || is_game(id)) continue;
            const KilnReplItemMap* it =
                std::lower_bound(map, map + nmap, id, [](const KilnReplItemMap& m, uint32_t v) { return m.from < v; });
            const uint32_t to = it != map + nmap && it->from == id && it->to && is_game(it->to) ? it->to : 0;
            std::memcpy(data + off, &to, 4);
            ++(to ? *remapped : *zeroed);
        }
    }
}

// Every id at the sites becomes 0 (the remap's fail-closed path).
inline void zero_ids(const RemapSite* sites, uint32_t n, uint8_t* data, uint32_t size) {
    for (uint32_t k = 0; k < n; ++k) {
        const RemapSite& s = sites[k];
        const uint32_t count = s.count ? s.count : 1;
        for (uint32_t j = 0; j < count; ++j) {
            const uint64_t off = uint64_t{s.offset} + uint64_t{j} * s.stride;
            if (off + 4 > size) break;
            std::memset(data + off, 0, 4);
        }
    }
}

// The snapshot hook's first test: 1 when a component of the state's list 1 has its bit in mask, 0 when none, -1 when
// the list is malformed (an index past reg_count, or past the blob). versioned(i): a version byte follows index i.
template <class Versioned>
inline int list1_match(const uint64_t* mask, const uint8_t* s, uint32_t reg_count, Versioned&& versioned) {
    uint16_t total, count;
    std::memcpy(&total, s, 2);
    std::memcpy(&count, s + 2, 2);
    uint32_t o = 8;
    for (uint32_t k = 0; k < count; ++k) {
        if (o + 2 > total) return -1;
        uint16_t i;
        std::memcpy(&i, s + o, 2);
        o += 2;
        if (i >= reg_count) return -1;
        if (versioned(i)) o += 1;
        if ((mask[i >> 6] >> (i & 63)) & 1) return 1;
    }
    return 0;
}

namespace snap_detail {
inline int note(EXCEPTION_POINTERS* e, uintptr_t* where) {
    *where = reinterpret_cast<uintptr_t>(e->ExceptionRecord->ExceptionAddress);
    return EXCEPTION_EXECUTE_HANDLER;
}
}  // namespace snap_detail

// One filter under its own guard: true when it returned normally and did not set *ctx->failed. A fault sets *where.
inline bool call_filter(KilnReplFilterFn fn, uint8_t* scratch, uint32_t size, KilnReplFilterCtx* ctx, void* user, uintptr_t* where) {
    uint32_t failed = 0;
    ctx->failed = &failed;
    __try {
        fn(scratch, size, ctx, user);
    } __except (snap_detail::note(GetExceptionInformation(), where)) {
        if (!*where) *where = 1;
        ctx->failed = nullptr;
        return false;
    }
    ctx->failed = nullptr;
    return failed == 0;
}

}  // namespace repl
