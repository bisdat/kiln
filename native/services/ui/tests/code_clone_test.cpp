// SPDX-License-Identifier: MIT
// code_clone.h: a function copied elsewhere keeps its meaning (the main menu's footer, menu_footer.cpp). Synthetic code,
// and the game's own footer function when the client exe is installed (read only; skipped otherwise).
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

#include "../code_clone.h"

static int g_fail = 0;
#define CHECK(c)                                                         \
    do {                                                                 \
        if (!(c)) {                                                      \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);     \
            ++g_fail;                                                    \
        }                                                                \
    } while (0)

static int32_t rd32(const uint8_t* p) {
    int32_t v;
    std::memcpy(&v, p, 4);
    return v;
}

// lea rax,[rip+0x10] (inside) / call +0x1000 (outside) / je +5 (inside) / mov rax,[rip+0x100000] (outside) /
// jmp short +2 (inside) / nop nop / ret
static const uint8_t kCode[] = {
    0x48, 0x8D, 0x05, 0x10, 0x00, 0x00, 0x00,        // 0x00 lea rax,[rip+0x10]  -> 0x17
    0xE8, 0x00, 0x10, 0x00, 0x00,                    // 0x07 call -> 0x100c
    0x0F, 0x84, 0x05, 0x00, 0x00, 0x00,              // 0x0c je -> 0x17
    0x48, 0x8B, 0x05, 0x00, 0x00, 0x10, 0x00,        // 0x12 mov rax,[rip+0x100000] -> 0x100019
    0xEB, 0x02,                                      // 0x19 jmp short -> 0x1d
    0x90, 0x90,                                      // 0x1b
    0xC3,                                            // 0x1d ret
};

static void synthetic() {
    const size_t n = sizeof kCode;
    const uint64_t src = 0x140000000ull, dst = 0x13f000000ull;
    std::vector<uint8_t> out(n);
    std::string err;
    CHECK(code_clone::relocate(kCode, n, src, out.data(), dst, UINT32_MAX, 0, &err));
    // inside references keep their fields
    CHECK(rd32(&out[3]) == 0x10);
    CHECK(rd32(&out[0x0e]) == 5);
    CHECK(out[0x1a] == 0x02);
    // outside references point where they pointed
    CHECK(dst + 0x0c + rd32(&out[0x08]) == src + 0x100c);
    CHECK(dst + 0x19 + rd32(&out[0x15]) == src + 0x100019);
    CHECK(code_clone::same_meaning(kCode, src, out.data(), dst, n, UINT32_MAX, 0, &err));
    // a call sent elsewhere
    const uint64_t thunk = dst + 0x800;
    CHECK(code_clone::relocate(kCode, n, src, out.data(), dst, 0x07, thunk, &err));
    CHECK(dst + 0x0c + rd32(&out[0x08]) == thunk);
    CHECK(code_clone::same_meaning(kCode, src, out.data(), dst, n, 0x07, thunk, &err));
    CHECK(!code_clone::same_meaning(kCode, src, out.data(), dst, n, UINT32_MAX, 0, &err));  // not the original's call
    // a tampered copy is caught
    out[0x1b] = 0xCC;
    CHECK(!code_clone::same_meaning(kCode, src, out.data(), dst, n, 0x07, thunk, &err));
    // refusals: a redirect that is not a call, a target out of reach, an outside rel8, an indirect jump, a cut instruction
    CHECK(!code_clone::relocate(kCode, n, src, out.data(), dst, 0x0c, thunk, &err));
    CHECK(!code_clone::relocate(kCode, n, src, out.data(), dst, 0x08, thunk, &err));  // no instruction starts there
    CHECK(!code_clone::relocate(kCode, n, src, out.data(), src + 0x100000000ull, UINT32_MAX, 0, &err));
    const uint8_t short_out[] = {0xEB, 0x10, 0xC3};
    std::vector<uint8_t> o2(sizeof short_out);
    CHECK(!code_clone::relocate(short_out, sizeof short_out, src, o2.data(), dst, UINT32_MAX, 0, &err));
    CHECK(!code_clone::relocate(short_out, sizeof short_out, src, o2.data(), src, UINT32_MAX, 0, &err));  // also in place
    const uint8_t indirect[] = {0xFF, 0xE0, 0xC3};  // jmp rax
    CHECK(!code_clone::relocate(indirect, sizeof indirect, src, o2.data(), dst, UINT32_MAX, 0, &err));
    const uint8_t cut[] = {0x90, 0xE8, 0x00};
    CHECK(!code_clone::relocate(cut, sizeof cut, src, o2.data(), dst, UINT32_MAX, 0, &err));
    // live against file: a call-site patch is told apart from other changes
    std::vector<uint8_t> live(kCode, kCode + n);
    live[0x08] = 0x34;  // the call's target
    live[0x1b] = 0xCC;  // a nop
    const auto ch = code_clone::changes(kCode, live.data(), n, src);
    CHECK(ch.size() == 2);
    if (ch.size() == 2) {
        CHECK(ch[0].at == 0x07 && ch[0].call && ch[0].live_target == src + 0x0c + rd32(&live[0x08]));
        CHECK(ch[1].at == 0x1b && !ch[1].call);
    }
}

// The game's footer function (client build 26430625, sha256 5d9ef2f1...): RVA 0xc189e0, 0x3f5 bytes, build-number call
// at +0x234. Copied 300 MB below the image: every outside reference still points where the game's does.
static bool game() {
    const char* home = std::getenv("HOME");
    std::string path = std::getenv("KILN_CLIENT_EXE") ? std::getenv("KILN_CLIENT_EXE")
                                                      : std::string(home ? home : "") +
                                                            "/.local/share/Steam/steamapps/common/Enshrouded/enshrouded.exe";
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    // PE: section of RVA 0xc189e0
    auto u16 = [&](uint64_t off) { uint16_t v = 0; f.seekg(off); f.read(reinterpret_cast<char*>(&v), 2); return v; };
    auto u32 = [&](uint64_t off) { uint32_t v = 0; f.seekg(off); f.read(reinterpret_cast<char*>(&v), 4); return v; };
    const uint32_t pe = u32(0x3c), nsec = u16(pe + 6), opt = u16(pe + 20);
    const uint32_t rva = 0xc189e0, n = 0x3f5, site = 0x234;
    uint64_t at = 0;
    for (uint32_t i = 0; i < nsec; ++i) {
        const uint64_t s = pe + 24 + opt + i * 40;
        const uint32_t va = u32(s + 12), raw = u32(s + 16), ptr = u32(s + 20);
        if (rva >= va && rva + n <= va + raw) at = ptr + (rva - va);
    }
    if (!at) return false;
    std::vector<uint8_t> code(n);
    f.seekg(at);
    f.read(reinterpret_cast<char*>(code.data()), n);
    static const uint8_t kHead[] = {0x48, 0x89, 0x5C, 0x24, 0x18, 0x48, 0x89, 0x7C, 0x24, 0x20, 0x55};
    if (!f || std::memcmp(code.data(), kHead, sizeof kHead) != 0 || code[site] != 0xE8) {
        std::printf("code_clone: %s is another game build; the footer copy test is skipped\n", path.c_str());
        return false;
    }
    const uint64_t base = 0x140000000ull, src = base + rva, dst = base - 300ull * 1024 * 1024, thunk = dst + 0x1000;
    std::vector<uint8_t> out(n);
    std::string err;
    const bool ok = code_clone::relocate(code.data(), n, src, out.data(), dst, site, thunk, &err);
    if (!ok) std::printf("relocate: %s\n", err.c_str());
    CHECK(ok);
    CHECK(code_clone::same_meaning(code.data(), src, out.data(), dst, n, site, thunk, &err));
    std::vector<code_clone::Rel> rels;
    CHECK(code_clone::scan(code.data(), n, src, &rels, nullptr, &err));
    size_t outside = 0, calls = 0;
    for (const auto& r : rels) {
        outside += !r.inside;
        calls += r.call;
    }
    std::printf("code_clone: the game's footer: %zu relative fields, %zu to the outside, %zu calls\n", rels.size(), outside, calls);
    CHECK(calls >= 20 && outside >= 25);
    // the window call (menu_footer.h kWindowCallAt, +0x4b) and the build-number call, both sent elsewhere
    bool window_call = false;
    for (const auto& r : rels) window_call |= r.at == 0x4b && r.call;
    CHECK(window_call);
    const code_clone::Redirect both[] = {{0x4b, thunk + 0x10}, {site, thunk}};
    CHECK(code_clone::relocate(code.data(), n, src, out.data(), dst, both, 2, &err));
    CHECK(code_clone::same_meaning(code.data(), src, out.data(), dst, n, both, 2, &err));
    return true;
}

// Two calls sent elsewhere (the footer's window call and its build-number call), and the calls a function makes from its
// start (XHL-Core's footer detour: its state getter is the first one).
static void two_redirects() {
    // call +0x100 / call +0x200 / ret
    static const uint8_t code[] = {0xE8, 0x00, 0x01, 0x00, 0x00, 0xE8, 0x00, 0x02, 0x00, 0x00, 0xC3};
    const size_t n = sizeof code;
    const uint64_t src = 0x140000000ull, dst = 0x13f000000ull, a = dst + 0x800, b = dst + 0x810;
    std::vector<uint8_t> out(n);
    std::string err;
    const code_clone::Redirect both[] = {{0x00, a}, {0x05, b}};
    CHECK(code_clone::relocate(code, n, src, out.data(), dst, both, 2, &err));
    CHECK(dst + 0x05 + rd32(&out[0x01]) == a);
    CHECK(dst + 0x0a + rd32(&out[0x06]) == b);
    CHECK(code_clone::same_meaning(code, src, out.data(), dst, n, both, 2, &err));
    const code_clone::Redirect one[] = {{0x05, b}};
    CHECK(!code_clone::same_meaning(code, src, out.data(), dst, n, one, 1, &err));  // the first call is not the original's
    const code_clone::Redirect bad[] = {{0x00, a}, {0x0a, b}};                        // a ret is not a call
    CHECK(!code_clone::relocate(code, n, src, out.data(), dst, bad, 2, &err));
    const code_clone::Redirect missing[] = {{0x00, a}, {0x03, b}};                    // no instruction starts there
    CHECK(!code_clone::relocate(code, n, src, out.data(), dst, missing, 2, &err));

    // XHL-Core 2.1.0's footer detour (dinput8.dll RVA 0x596a0), its first 0x90 bytes: two returns, a jmp rax, then the
    // call of its state getter (RVA 0x43af0)
    static const uint8_t detour[] = {
        0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x56, 0x57, 0x55, 0x53, 0x48, 0x81, 0xEC, 0x38, 0x02, 0x00, 0x00, 0x0F,
        0x29, 0xB4, 0x24, 0x20, 0x02, 0x00, 0x00, 0x48, 0x89, 0xD7, 0x31, 0xD2, 0x31, 0xC0, 0xF0, 0x48, 0x0F, 0xB1, 0x15, 0xF5,
        0xFD, 0x0A, 0x00, 0x75, 0x1F, 0x48, 0x89, 0xD0, 0x0F, 0x28, 0xB4, 0x24, 0x20, 0x02, 0x00, 0x00, 0x48, 0x81, 0xC4, 0x38,
        0x02, 0x00, 0x00, 0x5B, 0x5D, 0x5F, 0x5E, 0x41, 0x5C, 0x41, 0x5D, 0x41, 0x5E, 0x41, 0x5F, 0xC3, 0x0F, 0xB6, 0x15, 0xB5,
        0xFB, 0x0A, 0x00, 0x80, 0xFA, 0x01, 0x75, 0x21, 0x48, 0x89, 0xFA, 0x0F, 0x28, 0xB4, 0x24, 0x20, 0x02, 0x00, 0x00, 0x48,
        0x81, 0xC4, 0x38, 0x02, 0x00, 0x00, 0x5B, 0x5D, 0x5F, 0x5E, 0x41, 0x5C, 0x41, 0x5D, 0x41, 0x5E, 0x41, 0x5F, 0x48, 0xFF,
        0xE0, 0x49, 0x89, 0xC4, 0x49, 0x89, 0xCE, 0x4C, 0x89, 0xC6, 0x44, 0x89, 0xCB, 0xE8, 0xC6, 0xA3, 0xFE, 0xFF, 0x89, 0xC5,
        0x85, 0xC0, 0x74, 0x09};
    const auto calls = code_clone::calls_in(detour, sizeof detour, 0x1800596a0ull);
    CHECK(!calls.empty() && calls[0] == 0x180043af0ull);
    CHECK(code_clone::calls_in(detour, 0x80, 0x1800596a0ull).empty());  // cut before the call: none
    const uint8_t junk[] = {0x06, 0xE8, 0, 0, 0, 0};                      // an undecodable start (push es): nothing past it
    CHECK(code_clone::calls_in(junk, sizeof junk, 0x1000).empty());
}

int main() {
    synthetic();
    two_redirects();
    const bool real = game();
    std::printf("code_clone_test: %s (%s)\n", g_fail ? "FAIL" : "ok", real ? "with the game's footer" : "synthetic only");
    return g_fail ? 1 : 0;
}
