// SPDX-License-Identifier: MIT
// kiln.routines: the wire document's reader (strict: format 1 and nothing else), the canonical document, SHA-256, and
// which window a minute takes. See plan.h and native/core/abi/services/kiln_routines.h.
#include "plan.h"

#include <cstring>

#include "services/kiln_routines.h"

namespace kroutines {

namespace {
const char* const kKinds[] = {"vanilla", "idle", "roam", "sleep", "sit", "inspect"};

// ---- SHA-256 (FIPS 180-4) ----
constexpr uint32_t kK[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be,
    0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa,
    0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85,
    0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
    0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f,
    0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

inline uint32_t rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

void sha_block(uint32_t h[8], const uint8_t* p) {
    uint32_t w[64];
    for (int i = 0; i < 16; ++i)
        w[i] = (uint32_t{p[i * 4]} << 24) | (uint32_t{p[i * 4 + 1]} << 16) | (uint32_t{p[i * 4 + 2]} << 8) | uint32_t{p[i * 4 + 3]};
    for (int i = 16; i < 64; ++i) {
        const uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        const uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
    for (int i = 0; i < 64; ++i) {
        const uint32_t t1 = hh + (rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)) + ((e & f) ^ (~e & g)) + kK[i] + w[i];
        const uint32_t t2 = (rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        hh = g, g = f, f = e, e = d + t1, d = c, c = b, b = a, a = t1 + t2;
    }
    h[0] += a, h[1] += b, h[2] += c, h[3] += d, h[4] += e, h[5] += f, h[6] += g, h[7] += hh;
}

// ---- a reader for exactly the JSON the wire document uses: objects, arrays, strings, integers, true and false ----
struct Reader {
    const char* p;
    const char* end;
    std::string* problem;
    int depth = 0;

    bool fail(const std::string& why) {
        if (problem->empty()) *problem = why;
        return false;
    }
    void ws() {
        while (p < end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) ++p;
    }
    bool lit(char c) {
        ws();
        if (p < end && *p == c) return ++p, true;
        return false;
    }
    bool expect(char c) { return lit(c) || fail(std::string("'") + c + "' expected"); }
    // A string without escapes beyond \" \\ \/ : keys, task kinds, the plan's key and the hash need no more, and a
    // document that uses another escape is refused rather than read loosely.
    bool str(std::string& out) {
        ws();
        if (p >= end || *p != '"') return fail("a string expected");
        ++p;
        out.clear();
        while (p < end && *p != '"') {
            const unsigned char c = static_cast<unsigned char>(*p);
            if (c < 0x20) return fail("a control character in a string");
            if (c == '\\') {
                if (p + 1 >= end || (p[1] != '"' && p[1] != '\\' && p[1] != '/')) return fail("an escape a plan does not use");
                ++p;
            }
            out.push_back(*p++);
            if (out.size() > 512) return fail("a string of more than 512 bytes");
        }
        if (p >= end) return fail("a string that does not end");
        ++p;
        return true;
    }
    // A whole number: no fraction, no exponent, no leading zeros.
    bool integer(int64_t& out, const char* what) {
        ws();
        const char* s = p;
        const bool neg = p < end && *p == '-';
        if (neg) ++p;
        if (p >= end || *p < '0' || *p > '9') return fail(std::string(what) + " is not a whole number");
        if (*p == '0' && p + 1 < end && p[1] >= '0' && p[1] <= '9') return fail(std::string(what) + " has a leading zero");
        uint64_t v = 0;
        int digits = 0;
        while (p < end && *p >= '0' && *p <= '9') {
            if (++digits > 18) return fail(std::string(what) + " is too large");
            v = v * 10 + static_cast<uint64_t>(*p++ - '0');
        }
        if (p < end && (*p == '.' || *p == 'e' || *p == 'E')) return p = s, fail(std::string(what) + " is not a whole number");
        out = neg ? -static_cast<int64_t>(v) : static_cast<int64_t>(v);
        return true;
    }
    bool boolean(bool& out, const char* what) {
        ws();
        if (end - p >= 4 && !std::memcmp(p, "true", 4)) return p += 4, out = true, true;
        if (end - p >= 5 && !std::memcmp(p, "false", 5)) return p += 5, out = false, true;
        return fail(std::string(what) + " is not true or false");
    }
};

bool in_i32(int64_t v) { return v >= INT32_MIN && v <= INT32_MAX; }

bool is_local(std::string_view s) {
    if (s.empty()) return false;
    for (char c : s)
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.')) return false;
    return true;
}

// { "kind": "<name>" } and nothing else
bool task(Reader& r, Kind& out, const std::string& what) {
    if (!r.expect('{')) return false;
    std::string name, value;
    if (!r.str(name) || !r.expect(':')) return false;
    if (name != "kind") return r.fail(what + ": \"" + name + "\" is not part of a task (it takes kind)");
    if (!r.str(value)) return false;
    r.ws();
    if (r.p < r.end && *r.p == ',') return r.fail(what + ": a task takes kind and nothing else");
    if (!r.expect('}')) return false;
    for (size_t i = 0; i < 6; ++i)
        if (value == kKinds[i]) return out = static_cast<Kind>(i), true;
    return r.fail(what + ": \"" + value + "\" is not a task of this version");
}

bool window(Reader& r, Window& w, size_t index) {
    const std::string at = "windows[" + std::to_string(index) + "]";
    if (!r.expect('{')) return false;
    unsigned seen = 0;
    std::string name;
    do {
        if (!r.str(name) || !r.expect(':')) return false;
        unsigned bit;
        int64_t v = 0;
        if (name == "startMinute") {
            bit = 1;
            if (!r.integer(v, "startMinute") || !in_i32(v)) return r.fail(at + ".startMinute is not a whole number of 32 bits");
            w.start = static_cast<int32_t>(v);
        } else if (name == "endMinute") {
            bit = 2;
            if (!r.integer(v, "endMinute") || !in_i32(v)) return r.fail(at + ".endMinute is not a whole number of 32 bits");
            w.end = static_cast<int32_t>(v);
        } else if (name == "priority") {
            bit = 4;
            if (!r.integer(v, "priority") || !in_i32(v)) return r.fail(at + ".priority is not a whole number of 32 bits");
            w.priority = static_cast<int32_t>(v);
        } else if (name == "allDay") {
            bit = 8;
            if (!r.boolean(w.all_day, "allDay")) return false;
        } else if (name == "task") {
            bit = 16;
            if (!task(r, w.task, at + ".task")) return false;
        } else {
            return r.fail(at + ": \"" + name + "\" is not part of a window");
        }
        if (seen & bit) return r.fail(at + "." + name + " is there twice");
        seen |= bit;
    } while (r.lit(','));
    if (!r.expect('}')) return false;
    if (seen != 31) return r.fail(at + ": startMinute, endMinute, allDay, priority and task are all needed");
    return true;
}

// The one rule set for a plan's numbers (the same as Kiln.Blocks' RoutinePlan.Check).
bool check(uint32_t version, const std::vector<Window>& windows, Kind fallback, std::string& problem) {
    if (version == 0) return problem = "planVersion is 0: a plan's version is 1 or more", false;
    if (windows.size() > KILN_ROUTINES_MAX_WINDOWS) return problem = "more than 64 windows", false;
    for (size_t i = 0; i < windows.size(); ++i) {
        const Window& w = windows[i];
        const std::string at = "windows[" + std::to_string(i) + "]";
        if (w.all_day) {
            if (w.start != 0 || w.end != 0) return problem = at + ": a window of the whole day has startMinute 0 and endMinute 0", false;
            continue;
        }
        if (w.start < 0 || w.start >= kMinutesPerDay) return problem = at + ".startMinute is outside 0..1439", false;
        if (w.end < 0 || w.end >= kMinutesPerDay) return problem = at + ".endMinute is outside 0..1439", false;
        if (w.start == w.end) return problem = at + " starts and ends at the same minute", false;
    }
    if (fallback != Kind::Vanilla && fallback != Kind::Idle && fallback != Kind::Roam)
        return problem = "the fallback is vanilla, idle or roam", false;
    return true;
}
}  // namespace

const char* kind_name(Kind k) { return static_cast<size_t>(k) < 6 ? kKinds[static_cast<size_t>(k)] : "none"; }

std::string sha256_hex(std::string_view bytes) {
    uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    const auto* p = reinterpret_cast<const uint8_t*>(bytes.data());
    size_t n = bytes.size();
    for (; n >= 64; p += 64, n -= 64) sha_block(h, p);
    uint8_t tail[128] = {};
    std::memcpy(tail, p, n);
    tail[n] = 0x80;
    const size_t total = n + 9 <= 64 ? 64 : 128;
    const uint64_t bits = static_cast<uint64_t>(bytes.size()) * 8;
    for (int i = 0; i < 8; ++i) tail[total - 1 - static_cast<size_t>(i)] = static_cast<uint8_t>(bits >> (8 * i));
    sha_block(h, tail);
    if (total == 128) sha_block(h, tail + 64);
    static const char hex[] = "0123456789abcdef";
    std::string out(64, '0');
    for (int i = 0; i < 8; ++i)
        for (int j = 0; j < 8; ++j) out[static_cast<size_t>(i * 8 + j)] = hex[(h[i] >> (28 - 4 * j)) & 15];
    return out;
}

std::string canonical(uint32_t version, const std::vector<Window>& windows, Kind fallback) {
    std::string s = "{\"fallback\":{\"kind\":\"";
    s += kind_name(fallback);
    s += "\"},\"format\":1,\"planVersion\":" + std::to_string(version) + ",\"windows\":[";
    for (size_t i = 0; i < windows.size(); ++i) {
        const Window& w = windows[i];
        if (i) s += ',';
        s += "{\"allDay\":";
        s += w.all_day ? "true" : "false";
        s += ",\"endMinute\":" + std::to_string(w.end) + ",\"priority\":" + std::to_string(w.priority) + ",\"startMinute\":" +
             std::to_string(w.start) + ",\"task\":{\"kind\":\"";
        s += kind_name(w.task);
        s += "\"}}";
    }
    s += "]}";
    return s;
}

int32_t minute_of(int64_t now_ns) noexcept {
    int64_t minutes = now_ns / kNsPerMinute;
    if (now_ns % kNsPerMinute < 0) --minutes;
    const int64_t m = minutes % kMinutesPerDay;
    return static_cast<int32_t>(m < 0 ? m + kMinutesPerDay : m);
}

bool covers(const Window& w, int32_t minute) noexcept {
    if (w.all_day) return true;
    return w.start < w.end ? minute >= w.start && minute < w.end : minute >= w.start || minute < w.end;
}

int32_t Plan::select(int32_t minute) const noexcept {
    int32_t best = -1;
    for (size_t i = 0; i < windows.size(); ++i)
        if (covers(windows[i], minute) && (best < 0 || windows[i].priority > windows[static_cast<size_t>(best)].priority))
            best = static_cast<int32_t>(i);
    return best;
}

bool parse_plan(std::string_view json, Plan& out, std::string& problem) {
    problem.clear();
    if (json.size() > KILN_ROUTINES_MAX_PLAN_BYTES) return problem = "a plan of more than 16384 bytes", false;
    Reader r{json.data(), json.data() + json.size(), &problem};
    Plan plan;
    std::string hash, name;
    unsigned seen = 0;
    if (!r.expect('{')) return false;
    do {
        if (!r.str(name) || !r.expect(':')) return false;
        unsigned bit;
        int64_t v = 0;
        if (name == "format") {
            bit = 1;
            if (!r.integer(v, "format")) return false;
            if (v != 1) return r.fail("format " + std::to_string(v) + ": this Kiln reads format 1");
        } else if (name == "key") {
            bit = 2;
            if (!r.str(plan.key)) return false;
        } else if (name == "planVersion") {
            bit = 4;
            if (!r.integer(v, "planVersion") || v < 0 || v > UINT32_MAX) return r.fail("planVersion is not a whole number of 32 bits");
            plan.version = static_cast<uint32_t>(v);
        } else if (name == "contentHash") {
            bit = 8;
            if (!r.str(hash)) return false;
        } else if (name == "windows") {
            bit = 16;
            if (!r.expect('[')) return false;
            if (!r.lit(']')) {
                do {
                    if (plan.windows.size() >= KILN_ROUTINES_MAX_WINDOWS) return r.fail("more than 64 windows");
                    Window w{};
                    if (!window(r, w, plan.windows.size())) return false;
                    plan.windows.push_back(w);
                } while (r.lit(','));
                if (!r.expect(']')) return false;
            }
        } else if (name == "fallback") {
            bit = 32;
            if (!task(r, plan.fallback, "fallback")) return false;
        } else {
            return r.fail("\"" + name + "\" is not part of a routine plan");
        }
        if (seen & bit) return r.fail(name + " is there twice");
        seen |= bit;
    } while (r.lit(','));
    if (!r.expect('}')) return false;
    r.ws();
    if (r.p != r.end) return r.fail("text after the plan");
    if (seen != 63) return r.fail("format, key, planVersion, contentHash, windows and fallback are all needed");
    const size_t colon = plan.key.find(':');
    if (colon == std::string::npos || colon == 0 || !is_local(std::string_view(plan.key).substr(colon + 1)) ||
        plan.key.find_first_of(" \t\r\n\"\\/") < colon)
        return r.fail("key \"" + plan.key + "\" is not <mod>:<name>");
    plan.mod = plan.key.substr(0, colon);
    if (!check(plan.version, plan.windows, plan.fallback, problem)) return false;
    plan.hash = sha256_hex(canonical(plan.version, plan.windows, plan.fallback));
    if (plan.hash != hash) return r.fail("contentHash is not the hash of the plan's content");
    out = std::move(plan);
    return true;
}

}  // namespace kroutines
