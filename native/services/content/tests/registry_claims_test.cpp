// SPDX-License-Identifier: MIT
// Host test of the registry stage's claim index (registry/claims.cpp): every answer equals what today's full scans in
// gamedata_bootpatch.cpp's prepare give (same_place and through over claims kept in insertion order: the first match),
// on random claim sets; then the cost of 4,096 claims and 50,000 questions.
#include <chrono>
#include <cstdio>
#include <algorithm>
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
// today's segments() + normalised() (gamedata_bootpatch.cpp), the template fold included
static bool segments(const std::string& path, std::vector<Seg>* out) {
    out->clear();
    std::string cur;
    auto flush = [&]() {
        if (!cur.empty()) out->push_back({cur, false});
        cur.clear();
    };
    for (size_t i = 0; i < path.size(); ++i) {
        const char c = path[i];
        if (c == '.') {
            if (cur.empty() && (out->empty() || !out->back().index)) return false;
            flush();
        } else if (c == '[') {
            flush();
            const size_t end = path.find(']', i);
            if (end == std::string::npos) return false;
            const std::string in = path.substr(i + 1, end - i - 1);
            if (in != "*" && (in.empty() || in.size() > 9 || !std::all_of(in.begin(), in.end(), [](char d) { return d >= '0' && d <= '9'; }))) return false;
            out->push_back({in, true});
            i = end;
        } else {
            cur += c;
        }
    }
    flush();
    return !out->empty();
}
static std::string folded(std::string s) {
    for (char& c : s)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return s;
}
static std::vector<Seg> normalised(std::vector<Seg> segs, bool tmpl) {
    for (Seg& s : segs) {
        if (!s.index) s.s = folded(s.s);
        else if (s.s != "*") s.s = std::to_string(std::stoul(s.s));
    }
    if (tmpl && !segs.empty() && !segs[0].index) {
        std::string& f = segs[0].s;
        const size_t h = f.find('#');
        const std::string k = h == std::string::npos ? "" : f.substr(h + 1);
        if (!k.empty() && k.size() <= 9 && std::all_of(k.begin(), k.end(), [](char d) { return d >= '0' && d <= '9'; })) {
            const unsigned long nth = std::stoul(k);
            f = f.substr(0, h) + (nth ? "#" + std::to_string(nth) : "");
        }
    }
    return segs;
}
static std::string text_of(const std::vector<Seg>& segs) {
    std::string t;
    for (const Seg& s : segs) t += s.index ? "[" + s.s + "]" : (t.empty() ? "" : ".") + s.s;
    return t;
}
static std::string base_of(const std::string& path) {
    std::string b;
    for (size_t i = 0; i < path.size(); ++i) {
        if (path[i] == '[') {
            const size_t e = path.find(']', i);
            if (e == std::string::npos) break;
            i = e;
        } else {
            b += path[i];
        }
    }
    return b;
}
static std::string index_of(const std::string& path) {
    std::string x;
    for (size_t i = 0; i < path.size(); ++i)
        if (path[i] == '[') {
            const size_t e = path.find(']', i);
            if (e == std::string::npos) break;
            x += path.substr(i, e - i + 1);
            i = e;
        }
    return x;
}
static bool overlap(const std::string& a, const std::string& b) { return a == b || a.find('*') != std::string::npos || b.find('*') != std::string::npos; }
// today's "fields" check: the same members, and the same indices or a "*" anywhere in either
static bool fields_overlap(const std::vector<Seg>& a, const std::vector<Seg>& b) {
    const std::string ta = text_of(a), tb = text_of(b);
    return base_of(ta) == base_of(tb) && overlap(index_of(ta), index_of(tb));
}

static std::string random_text(std::mt19937& rng, bool tmpl) {
    static const char* const first[] = {"Health", "health#0", "Health#1", "HEALTH#01", "Mana#2", "mana", "Health#", "Health#x"};
    static const char* const names[] = {"a", "B", "c"};
    static const char* const idx[] = {"[0]", "[01]", "[2]", "[*]"};
    std::string p = tmpl ? first[rng() % 8] : names[rng() % 3];
    const int n = static_cast<int>(rng() % 4);
    for (int i = 0; i < n; ++i) p += rng() % 2 ? std::string(idx[rng() % 4]) : "." + std::string(names[rng() % 3]);
    return p;
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
    // templates: "#k" of the first segment names the k-th component of its type ("#0" is none, "#01" is "#1"); a cap on segments
    {
        std::vector<Seg> a, b;
        std::string err;
        CHECK(parse_path("Health#0.max", &a, &err, true) && parse_path("Health.max", &b, &err, true) && a.size() == 2 && a[0].s == "health" && b[0].s == "health",
              "template: Health#0 is Health");
        CHECK(parse_path("Health#01.max", &a, &err, true) && a[0].s == "health#1", "template: #01 is #1");
        CHECK(parse_path("Health#0.max", &a, &err) && a[0].s == "health#0", "no template: #0 stays");
        ClaimIndex ix;
        Claim c;
        c.mod = "m1";
        parse_path("Health#0.max", &a, &err, true);
        parse_path("Health.max", &b, &err, true);
        ix.add("r", a, c);
        CHECK(ix.same_place("r", b, [](const Claim&) { return true; }) != nullptr, "template: a claim on Health#0.max meets Health.max");
        std::mt19937 rng(7);
        for (int i = 0; i < 20000; ++i) {
            const bool tmpl = rng() % 2;
            const std::string p = random_text(rng, tmpl);
            std::vector<Seg> want, got;
            const bool ok = segments(p, &want);
            const bool mine = parse_path(p, &got, &err, tmpl);
            bool same = ok == mine;
            if (ok && mine) {
                want = normalised(want, tmpl);
                same = want.size() == got.size();
                for (size_t k = 0; same && k < want.size(); ++k) same = want[k].s == got[k].s && want[k].index == got[k].index;
            }
            CHECK(same, ("parse_path equals segments() + normalised() for \"" + p + "\"").c_str());
        }
        std::string p = "a";
        for (int i = 1; i < 64; ++i) p += ".a";
        CHECK(parse_path(p, &a, &err) && a.size() == 64, "64 segments are taken");
        CHECK(!parse_path(p + ".a", &a, &err) && err.find("64") != std::string::npos, "65 segments are refused");
        std::string deep = "a";
        for (int i = 0; i < 100000; ++i) deep += "[1]";
        CHECK(!parse_path(deep, &a, &err), "a path of 100,001 segments is refused (and nothing recursed on it)");
        std::vector<Seg> long_place(65, Seg{"a", false});
        ClaimIndex ix2;
        CHECK(ix2.add("r", long_place, c) == 0 && ix2.size() == 0 && ix2.same_place("r", long_place, [](const Claim&) { return true; }) == nullptr,
              "a place of more than 64 segments is not taken by the index");
    }
    // the fields check: "*" anywhere in either path's indices overlaps (not per position)
    {
        std::vector<Seg> a, b;
        std::string err;
        parse_path("a[*].b[1]", &a, &err);
        parse_path("a[2].b[3]", &b, &err);
        ClaimIndex ix;
        Claim c;
        c.mod = "m1";
        ix.add("r", a, c);
        CHECK(ix.overlap("r", b, [](const Claim&) { return true; }) != nullptr, "overlap: a[*].b[1] and a[2].b[3] overlap, as today");
        CHECK(ix.same_place("r", b, [](const Claim&) { return true; }) == nullptr, "same_place stays per position");
        parse_path("a.b[1]", &b, &err);
        std::vector<Seg> a2;
        parse_path("a[1].b", &a2, &err);
        ClaimIndex ix3;
        ix3.add("r", a2, c);
        CHECK(ix3.overlap("r", b, [](const Claim&) { return true; }) != nullptr, "overlap: a[1].b and a.b[1] are the same members and indices, as today");
    }
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
        const bool tmpl = round % 2;  // every other round: template paths ("Health#0.max" and "Health.max" are one place)
        auto any_path = [&]() {
            if (!tmpl) return random_path(rng);
            std::vector<Seg> p;
            std::string err;
            while (!parse_path(random_text(rng, true), &p, &err, true)) {
            }
            return p;
        };
        for (int i = 0; i < n; ++i) {
            Kept k{rng() % 4 ? "r1" : "r2", any_path(), {}};
            k.c.mod = "m" + std::to_string(rng() % 4);
            k.c.kind = static_cast<ClaimKind>(rng() % 5);
            k.c.value = std::to_string(rng() % 3);
            k.c.seq = ix.add(k.res, k.place, k.c);
            kept.push_back(k);
        }
        for (int q = 0; q < 100; ++q) {
            const std::string res = rng() % 4 ? "r1" : "r2";
            const std::vector<Seg> path = any_path();
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
            const Claim* o = ix.overlap(res, path, pred);
            CHECK((o ? o->seq : 0) == oracle([&](const std::vector<Seg>& p) { return fields_overlap(p, path); }), "overlap equals today's fields check");
            // a replace or clear asks the appended lists first, then every set's place (today's loop order), each the first match
            auto kind_first = [&](ClaimKind k) -> uint64_t {
                for (const Kept& x : kept)
                    if (x.res == res && x.c.mod != mod && x.c.kind == k && (same_place(x.place, path) || through(path, x.place))) return x.c.seq;
                return 0;
            };
            auto indexed = [&](ClaimKind k) -> uint64_t {
                auto pk = [&](const Claim& x) { return x.mod != mod && x.kind == k; };
                const Claim* s1 = ix.same_place(res, path, pk);
                const Claim* s2 = ix.below_list(res, path, pk);
                const Claim* best = !s1 ? s2 : !s2 ? s1 : (s1->seq < s2->seq ? s1 : s2);
                return best ? best->seq : 0;
            };
            const uint64_t want = kind_first(ClaimKind::Append) ? kind_first(ClaimKind::Append) : kind_first(ClaimKind::Set);
            const uint64_t got = indexed(ClaimKind::Append) ? indexed(ClaimKind::Append) : indexed(ClaimKind::Set);
            CHECK(want == got, "per kind in today's order (appended, then set places): the same claim");
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
