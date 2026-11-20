// SPDX-License-Identifier: MIT
// Host test of the registry stage's claim index (registry/claims.cpp): every answer equals what today's full scans in
// gamedata_bootpatch.cpp's prepare give (same_place and through over claims kept in insertion order: the first match),
// on random claim sets; then the cost of 4,096 claims and 50,000 questions.
#include <chrono>
#include <cstdio>
#include <random>
#include <string>

#include "../registry/claims.h"

using namespace kc::reg;

static int g_fail = 0, g_pass = 0;
#define CHECK(cond, what)                                              \
    do {                                                               \
        if (cond) ++g_pass;                                            \
        else {                                                         \
            ++g_fail;                                                  \
            if (g_fail < 20) std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, what); \
        }                                                              \
    } while (0)

// The oracle: today's helpers, as gamedata_bootpatch.cpp has them (same_place, through), over a vector in insertion order.
static bool same_place(const std::vector<Seg>& a, const std::vector<Seg>& b, size_t n = SIZE_MAX) {
    if (n == SIZE_MAX) {
        if (a.size() != b.size()) return false;
        n = a.size();
    }
    if (a.size() < n || b.size() < n) return false;
    for (size_t i = 0; i < n; ++i)
        if (a[i].index != b[i].index || (a[i].s != b[i].s && !(a[i].index && (a[i].s == "*" || b[i].s == "*")))) return false;
    return true;
}
static bool through(const std::vector<Seg>& list, const std::vector<Seg>& path) {
    return path.size() > list.size() && path[list.size()].index && same_place(list, path, list.size());
}

struct Kept {
    std::string res;
    std::vector<Seg> place;
    Claim c;
};

static std::vector<Seg> random_path(std::mt19937& rng) {
    static const char* const names[] = {"a", "b", "c"};
    static const char* const idx[] = {"0", "1", "2", "*"};
    std::vector<Seg> p;
    const int n = 1 + static_cast<int>(rng() % 4);
    for (int i = 0; i < n; ++i) {
        if (i > 0 && rng() % 2) p.push_back({idx[rng() % 4], true});
        else p.push_back({names[rng() % 3], false});
    }
    return p;
}

int main() {
    // parse_path: today's spelling rules
    {
        std::vector<Seg> s;
        std::string err;
        CHECK(parse_path("A.b[03].C[*]", &s, &err) && s.size() == 5 && s[0].s == "a" && s[2].s == "3" && s[2].index && s[3].s == "c" && s[4].s == "*",
              "parse_path: names lower-case, indices plain numbers, * kept");
        CHECK(!parse_path("a[x]", &s, &err) && !parse_path("a..b", &s, &err) && !parse_path("a[1", &s, &err) && !parse_path("", &s, &err), "parse_path: bad paths refused");
    }
    std::mt19937 rng(20261120);
    for (int round = 0; round < 200; ++round) {
        ClaimIndex ix;
        std::vector<Kept> kept;
        const int n = 1 + static_cast<int>(rng() % 60);
        for (int i = 0; i < n; ++i) {
            Kept k{rng() % 4 ? "r1" : "r2", random_path(rng), {}};
            k.c.mod = "m" + std::to_string(rng() % 4);
            k.c.kind = static_cast<ClaimKind>(rng() % 5);
            k.c.value = std::to_string(rng() % 3);
            k.c.seq = ix.add(k.res, k.place, k.c);
            kept.push_back(k);
        }
        for (int q = 0; q < 100; ++q) {
            const std::string res = rng() % 4 ? "r1" : "r2";
            const std::vector<Seg> path = random_path(rng);
            const std::string mod = "m" + std::to_string(rng() % 4);
            const ClaimKind kind = static_cast<ClaimKind>(rng() % 5);
            auto pred = [&](const Claim& c) { return c.mod != mod && c.kind == kind; };
            auto oracle = [&](auto&& test) -> uint64_t {
                for (const Kept& k : kept)
                    if (k.res == res && pred(k.c) && test(k.place)) return k.c.seq;
                return 0;
            };
            const Claim* a = ix.same_place(res, path, pred);
            CHECK((a ? a->seq : 0) == oracle([&](const std::vector<Seg>& p) { return same_place(p, path); }), "same_place equals the scan");
            const Claim* b = ix.list_through(res, path, pred);
            CHECK((b ? b->seq : 0) == oracle([&](const std::vector<Seg>& p) { return through(p, path); }), "list_through equals through(claim, path)");
            const Claim* c = ix.below_list(res, path, pred);
            CHECK((c ? c->seq : 0) == oracle([&](const std::vector<Seg>& p) { return through(path, p); }), "below_list equals through(path, claim)");
        }
    }
    // cost: 4,096 claims (today's per-generation cap) on 512 resources, 50,000 questions of each kind
    {
        ClaimIndex ix;
        std::vector<std::pair<std::string, std::vector<Seg>>> places;
        const auto t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < 4096; ++i) {
            const std::string res = "r" + std::to_string(i % 512);
            std::vector<Seg> p = random_path(rng);
            Claim c;
            c.mod = "m" + std::to_string(i % 37);
            ix.add(res, p, c);
            places.emplace_back(res, p);
        }
        const auto t1 = std::chrono::steady_clock::now();
        size_t hits = 0;
        auto pred = [](const Claim& c) { return c.mod != "m0"; };
        for (int q = 0; q < 50000; ++q) {
            const auto& [res, p] = places[q % places.size()];
            hits += ix.same_place(res, p, pred) != nullptr;
            hits += ix.list_through(res, p, pred) != nullptr;
            hits += ix.below_list(res, p, pred) != nullptr;
        }
        const auto t2 = std::chrono::steady_clock::now();
        const double add_ms = std::chrono::duration<double, std::milli>(t1 - t0).count(), q_ms = std::chrono::duration<double, std::milli>(t2 - t1).count();
        std::printf("registry-claims-test: 4096 claims added in %.2f ms; 150000 questions in %.2f ms (%zu answers)\n", add_ms, q_ms, hits);
        CHECK(ix.size() == 4096, "every claim kept");
    }
    std::printf("registry-claims-test: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
