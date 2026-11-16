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
    return true;
}

int main() {
    synthetic();
    const bool real = game();
    std::printf("code_clone_test: %s (%s)\n", g_fail ? "FAIL" : "ok", real ? "with the game's footer" : "synthetic only");
    return g_fail ? 1 : 0;
}
