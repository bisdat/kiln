// SPDX-License-Identifier: MIT
// entity_scan.h: for_each_entity's read of the entity map (0.2.0-dev.30.7) gives the same ids, in the same order, as the
// way up to 30.6 (copy every slot's key, test every slot), refuses on a failed read, and with "bench" times both on the
// server world's shape: 1,048,576 slots, 22,866 entities (PepegaJedi's base, find.log "22866 entities checked").
// "bench fresh" makes every large allocation fresh pages as Windows' heap does above 512 KB (glibc's default reuses).
#include <malloc.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <vector>

#include "entity_scan.h"

static int g_fail = 0, g_checks = 0;
static void check(bool ok, const char* what) {
    ++g_checks;
    if (!ok) {
        ++g_fail;
        std::printf("FAIL %s\n", what);
    }
}

namespace {
struct Map {
    uint64_t cap = 0;
    std::vector<uint64_t> bits;
    std::vector<uint32_t> keys;
};
Map make_map(uint64_t cap, size_t count, uint32_t seed) {
    Map m;
    m.cap = cap;
    m.bits.assign(static_cast<size_t>((cap + 63) / 64), 0);
    m.keys.assign(static_cast<size_t>(cap), 0xDEADBEEFu);  // unoccupied slots hold junk: must never be read as ids
    std::mt19937 rng(seed);
    size_t placed = 0;
    while (placed < count && placed < cap) {
        const uint64_t i = rng() % cap;
        if ((m.bits[i >> 6] >> (i & 63)) & 1) continue;
        m.bits[i >> 6] |= uint64_t{1} << (i & 63);
        m.keys[i] = static_cast<uint32_t>(i * 7 + 1);
        ++placed;
    }
    return m;
}
bool plain_copy(void* d, uintptr_t s, size_t n) {
    std::memcpy(d, reinterpret_cast<const void*>(s), n);
    return true;
}
// for_each_entity up to 0.2.0-dev.30.6 (api_core.cpp), the copy and the slot test as they were
bool old_scan(const Map& m, std::vector<uint32_t>& snapshot) {
    const uint64_t cap = m.cap;
    std::vector<uint64_t> words(static_cast<size_t>((cap + 63) / 64));
    std::vector<uint32_t> ids(static_cast<size_t>(cap));
    if (!plain_copy(words.data(), reinterpret_cast<uintptr_t>(m.bits.data()), words.size() * 8) ||
        !plain_copy(ids.data(), reinterpret_cast<uintptr_t>(m.keys.data()), ids.size() * 4))
        return false;
    snapshot.reserve(4096);
    for (uint64_t i = 0; i < cap; ++i)
        if ((words[i >> 6] >> (i & 63)) & 1) snapshot.push_back(ids[i]);
    return true;
}
// for_each_entity from 30.7: a kept bit buffer, a snapshot reserved to the last count
bool new_scan(const Map& m, std::vector<uint32_t>& snapshot) {
    thread_local std::vector<uint64_t> words;
    thread_local size_t last = 0;
    snapshot.reserve(last + 256);
    const bool ok = kemf::scan_entity_keys(m.cap, reinterpret_cast<uintptr_t>(m.bits.data()),
                                           reinterpret_cast<uintptr_t>(m.keys.data()), plain_copy, words, snapshot);
    if (ok) last = snapshot.size();
    return ok;
}

void same_as_old(uint64_t cap, size_t count, const char* what) {
    const Map m = make_map(cap, count, static_cast<uint32_t>(cap * 31 + count));
    std::vector<uint32_t> a, b;
    check(old_scan(m, a), what);
    check(new_scan(m, b), what);
    check(a == b, (std::string(what) + ": the same ids in the same order").c_str());
    check(b.size() == std::min<uint64_t>(count, cap), (std::string(what) + ": one id per entity").c_str());
    check(std::find(b.begin(), b.end(), 0xDEADBEEFu) == b.end(), (std::string(what) + ": no unoccupied slot read").c_str());
}

template <class F>
double ms_per_call(F&& f, int calls) {
    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < calls; ++i) f();
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() / calls;
}
}  // namespace

int main(int argc, char** argv) {
    const bool bench = argc > 1 && std::string(argv[1]) == "bench";
    const bool fresh = argc > 2 && std::string(argv[2]) == "fresh";

    same_as_old(1u << 20, 22866, "server shape (1M slots, 22866 entities)");
    same_as_old(1u << 20, 0, "an empty world");
    same_as_old(1u << 12, 4096, "every slot occupied (block copies only)");
    same_as_old(1u << 12, 300, "sparse (single reads only)");
    same_as_old(1u << 10, 600, "mixed blocks");
    same_as_old(1, 1, "one slot");
    same_as_old(8, 5, "a map smaller than a bit word");
    same_as_old(64, 64, "one full bit word");

    {  // a failed read: false, and the caller calls nothing
        const Map m = make_map(1u << 12, 2000, 7);
        const uintptr_t keys = reinterpret_cast<uintptr_t>(m.keys.data());
        std::vector<uint64_t> words;
        std::vector<uint32_t> out;
        auto bad_keys = [&](void* d, uintptr_t s, size_t n) {
            if (s >= keys + 2048 * 4 && s < keys + m.cap * 4) return false;  // the second half of the keys faults
            std::memcpy(d, reinterpret_cast<const void*>(s), n);
            return true;
        };
        check(!kemf::scan_entity_keys(m.cap, reinterpret_cast<uintptr_t>(m.bits.data()), keys, bad_keys, words, out),
              "a faulting key read refuses");
        auto bad_bits = [](void*, uintptr_t, size_t) { return false; };
        out.clear();
        check(!kemf::scan_entity_keys(m.cap, reinterpret_cast<uintptr_t>(m.bits.data()), keys, bad_bits, words, out) && out.empty(),
              "a faulting bit read refuses before any id");
        check(!kemf::scan_entity_keys(0, 1, 1, plain_copy, words, out), "no slots: refused");
        check(!kemf::scan_entity_keys(64, 0, 1, plain_copy, words, out), "no bits: refused");
    }
    {  // the reads it makes on the server shape: the bit words once, then about one small read per entity
        const Map m = make_map(1u << 20, 22866, 11);
        std::vector<uint64_t> words;
        std::vector<uint32_t> out;
        size_t bytes = 0, reads = 0;
        auto counting = [&](void* d, uintptr_t s, size_t n) {
            bytes += n;
            ++reads;
            std::memcpy(d, reinterpret_cast<const void*>(s), n);
            return true;
        };
        check(kemf::scan_entity_keys(m.cap, reinterpret_cast<uintptr_t>(m.bits.data()), reinterpret_cast<uintptr_t>(m.keys.data()),
                                     counting, words, out), "server shape: counted scan");
        std::printf("server shape: %zu ids, %zu reads, %.0f KB read (the old way: 2 reads, %.0f KB)\n", out.size(), reads,
                    bytes / 1024.0, (m.cap / 8 + m.cap * 4) / 1024.0);
        check(bytes < 300 * 1024, "server shape: under 300 KB read (the old way read 4.1 MB)");
    }

    if (bench) {
        if (fresh) mallopt(M_MMAP_THRESHOLD, 512 * 1024);  // fixed: every 4 MB vector is fresh pages, as on Windows
        const Map m = make_map(1u << 20, 22866, 3);
        volatile size_t sink = 0;
        auto old_call = [&] { std::vector<uint32_t> s; old_scan(m, s); sink = sink + s.size(); };
        auto new_call = [&] { std::vector<uint32_t> s; new_scan(m, s); sink = sink + s.size(); };
        old_call();
        new_call();
        double old_ms = 1e9, new_ms = 1e9;
        for (int round = 0; round < 5; ++round) {  // the best of 5 rounds of 100 calls
            old_ms = std::min(old_ms, ms_per_call(old_call, 100));
            new_ms = std::min(new_ms, ms_per_call(new_call, 100));
        }
        std::printf("BENCH for_each_entity scan, 1M slots, 22866 entities%s: old %.3f ms, new %.3f ms per call (%.1fx)\n",
                    fresh ? " (fresh pages per call, as Windows)" : "", old_ms, new_ms, old_ms / new_ms);
        check(new_ms * 3 < old_ms, "the new scan is at least 3x faster");
    }
    std::printf("ENTITY SCAN %s (%d checks)\n", g_fail ? "FAIL" : "PASS", g_checks);
    return g_fail ? EXIT_FAILURE : EXIT_SUCCESS;
}
