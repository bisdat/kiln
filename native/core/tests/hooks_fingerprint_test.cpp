// SPDX-License-Identifier: MIT
// Host tests of the ABI fingerprint pass and the `sig` descriptor (core/src/services/hooks_fingerprint.cpp).
// The "functions" are byte snippets assembled into this binary (Win64 register use; they are never run): the pass
// reads them through a Reader over the process's own memory, with a function table made from the snippet labels.
//   native/core/tests/test-hooks-fingerprint.sh
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <vector>

#include "nlohmann/json.hpp"
#include "services/hooks_fingerprint.h"

namespace fp = kiln::hooks::fp;

#define SNIPPET(name) ".globl " #name "\n.hidden " #name "\n" #name ":\n"
__asm__(
    ".intel_syntax noprefix\n"
    ".text\n"
    // void SetTimeScale(Daytime* d, float scale, s64 now): rcx, xmm1, r8
    SNIPPET(fp_time)
    "  movss dword ptr [rcx+0x10], xmm1\n"
    "  mov qword ptr [rcx+0x18], r8\n"
    "  ret\n"
    // a jump-through: shuffles one register and leaves the rest to another function
    SNIPPET(fp_through)
    "  mov rax, qword ptr [rcx+8]\n"
    "  mov rcx, rax\n"
    "  jmp fp_time\n"
    // an entry that is only a jmp rel32 to the real code (a tail-merged or incremental-link entry)
    SNIPPET(fp_thunk)
    "  .byte 0xE9\n"
    "  .long fp_branches - . - 4\n"
    "  int3\n"
    // a virtual tail call: the arguments' use cannot be followed
    SNIPPET(fp_indirect)
    "  mov rax, qword ptr [rcx]\n"
    "  jmp qword ptr [rax+0x18]\n"
    // an MSVC-style frame: rax copies rsp, rbp is set from it, two stack arguments are read through rbp and rsp
    SNIPPET(fp_stack)
    "  mov rax, rsp\n"
    "  mov qword ptr [rax+8], rbx\n"
    "  push rbp\n"
    "  lea rbp, [rax-0x58]\n"
    "  sub rsp, 0x140\n"
    "  test rdx, rdx\n"
    "  mov rbx, rcx\n"
    "  add r8, r9\n"
    "  mov eax, dword ptr [rbp+0x80]\n"            // entry rsp + 0x28
    "  movzx ecx, byte ptr [rsp+0x178]\n"         // entry rsp + 0x30
    "  add eax, ecx\n"
    "  add rsp, 0x140\n"
    "  pop rbp\n"
    "  ret\n"
    // zeroing idioms and merging writes are writes, not reads
    SNIPPET(fp_idioms)
    "  xor ecx, ecx\n"
    "  xorps xmm1, xmm1\n"
    "  cvtsi2ss xmm2, edx\n"
    "  movss xmm3, xmm0\n"
    "  ret\n"
    // a call: what it has not touched is passed on; afterwards the volatile registers are the callee's
    SNIPPET(fp_call)
    "  sub rsp, 0x28\n"
    "  mov r8, rcx\n"
    "  call fp_time\n"
    "  mov rcx, rdx\n"
    "  movaps xmm1, xmm0\n"
    "  add rsp, 0x28\n"
    "  ret\n"
    // both sides of a branch count
    SNIPPET(fp_branches)
    "  test ecx, ecx\n"
    "  jz 1f\n"
    "  mov eax, edx\n"
    "  ret\n"
    "1:\n"
    "  movd eax, xmm2\n"
    "  ret\n"
    // cmov: the destination is neither read nor written
    SNIPPET(fp_cmov)
    "  cmovne rdx, r8\n"
    "  ret\n"
    // a float return
    SNIPPET(fp_retf)
    "  xorps xmm0, xmm0\n"
    "  ret\n"
    // a loop ends
    SNIPPET(fp_loop)
    "2:\n"
    "  dec rcx\n"
    "  jnz 2b\n"
    "  ret\n"
    // rsp is realigned: stack arguments can no longer be followed
    SNIPPET(fp_blind)
    "  push rbp\n"
    "  mov rbp, rsp\n"
    "  and rsp, -32\n"
    "  mov rax, qword ptr [rcx]\n"
    "  mov rsp, rbp\n"
    "  pop rbp\n"
    "  ret\n"
    // a tail call after the function used one argument: stack arguments go with it
    SNIPPET(fp_tail)
    "  inc dword ptr [rcx]\n"
    "  jmp fp_stack\n"
    // bytes no decoder takes in 64-bit mode
    SNIPPET(fp_bad)
    "  .byte 0x06, 0x06, 0x06, 0x06\n"
    SNIPPET(fp_end)
    "  int3\n"
    ".att_syntax prefix\n");

extern "C" {
extern const uint8_t fp_time[], fp_through[], fp_thunk[], fp_indirect[], fp_stack[], fp_idioms[], fp_call[],
    fp_branches[], fp_cmov[], fp_retf[], fp_loop[], fp_blind[], fp_tail[], fp_bad[], fp_end[];
}

namespace {

int g_fail = 0, g_pass = 0;
void check(bool ok, const char* what, const std::string& detail = "") {
    std::printf("%s %s%s%s\n", ok ? "PASS" : "FAIL", what, detail.empty() ? "" : ": ", detail.c_str());
    ok ? ++g_pass : ++g_fail;
}

std::vector<uint64_t> g_starts;  // snippet starts, sorted: the fake function table
bool g_unwind = false;           // false: every snippet is a leaf (no unwind data); true: each has an entry

size_t read_bytes(void*, uint64_t addr, uint8_t* out, size_t n) {
    const uint64_t lo = reinterpret_cast<uint64_t>(fp_time), hi = reinterpret_cast<uint64_t>(fp_end) + 1;
    if (addr < lo || addr >= hi) return 0;
    n = static_cast<size_t>(std::min<uint64_t>(n, hi - addr));
    std::memcpy(out, reinterpret_cast<const void*>(addr), n);
    return n;
}
bool function_of(void*, uint64_t addr, uint64_t* primary, uint64_t* next) {
    auto it = std::upper_bound(g_starts.begin(), g_starts.end(), addr);
    *next = it == g_starts.end() ? UINT64_MAX : *it;
    if (!g_unwind || it == g_starts.begin()) return false;
    *primary = *(it - 1);
    return true;
}

fp::Fingerprint of(const uint8_t* snippet, bool table = true) {
    fp::Reader r;
    r.bytes = &read_bytes;
    r.function = table ? &function_of : nullptr;
    return fp::compute(r, reinterpret_cast<uint64_t>(snippet));
}

enum : uint8_t { RCX = 1, RDX = 2, R8 = 4, R9 = 8, X0 = 16, X1 = 32, X2 = 64, X3 = 128 };

std::string show(const fp::Fingerprint& f) { return fp::to_json(f); }

fp::Shape shape(std::initializer_list<std::pair<const char*, uint32_t>> args, bool sret = false) {
    static std::vector<KilnSigArg> store;
    store.clear();
    for (auto& [name, kind] : args) store.push_back(KilnSigArg{KilnStr{name, std::strlen(name)}, kind, 0});
    KilnSig s{};
    s.size = sizeof s;
    s.argc = static_cast<uint32_t>(store.size());
    s.args = store.data();
    s.sret = sret;
    s.ret_kind = sret ? KILN_SIG_STRUCT : KILN_SIG_VOID;
    s.ret_size = sret ? 16 : 0;
    return fp::shape_of(s);
}

void test_pass(bool unwind) {
    g_unwind = unwind;
    std::printf("-- the pass (%s)\n", unwind ? "snippets with unwind data" : "leaf snippets");
    {
        const auto f = of(fp_time);
        check(f.ok && f.reads == (RCX | X1 | R8) && !f.passes && !f.indirect && !f.stack && f.rets == fp::kRetNone && !f.tail,
              "a float in position 1 is xmm1: reads rcx, xmm1, r8; returns nothing", show(f));
        check(fp::reads_text(f) == "[rcx, xmm1, r8]", "reads_text", fp::reads_text(f));
    }
    {
        const auto f = of(fp_through);
        check(f.ok && f.reads == RCX && f.passes == (RDX | R8 | R9 | X0 | X1 | X2 | X3) && f.tail && f.rets == fp::kRetCallee &&
                  !f.indirect,
              "a jump-through reads what it uses and passes the rest to the function it jumps to", show(f));
    }
    {
        const auto f = of(fp_thunk);
        const auto g = of(fp_branches);
        check(f.ok && f.thunk && f.entry == reinterpret_cast<uint64_t>(fp_branches) && f.reads == g.reads && f.rets == g.rets,
              "an entry that is a jmp rel32 is followed once: the fingerprint is the real code's", show(f));
    }
    {
        const auto f = of(fp_indirect);
        check(f.ok && f.reads == RCX && f.indirect == (RDX | R8 | R9 | X0 | X1 | X2 | X3) && f.tail,
              "a jump through memory: the untouched registers are recorded as indirect", show(f));
        check(fp::to_json(f).find("\"result\":\"unknown\"") != std::string::npos, "...and the JSON result is unknown");
    }
    {
        const auto f = of(fp_stack);
        check(f.ok && f.reads == (RCX | RDX | R8 | R9) && f.stack == 3u && !f.stack_blind && f.rets == fp::kRetRax,
              "stack slots read through rsp and through a frame pointer set from a copy of rsp", show(f));
        check(fp::reads_text(f) == "[rcx, rdx, r8, r9, stack+0x28, stack+0x30]", "reads_text with stack slots", fp::reads_text(f));
    }
    {
        const auto f = of(fp_idioms);
        check(f.ok && f.reads == (RDX | X0), "xor r,r / xorps x,x / cvtsi2ss / movss x,x destinations are not reads", show(f));
    }
    {
        const auto f = of(fp_call);
        check(f.ok && f.reads == RCX && f.passes == (RDX | R9 | X0 | X1 | X2 | X3) && !f.tail && f.rets == fp::kRetCallee,
              "a call: untouched registers pass, later uses of volatile registers are the callee's values", show(f));
    }
    {
        const auto f = of(fp_branches);
        check(f.ok && f.reads == (RCX | RDX | X2) && f.rets == fp::kRetRax, "both sides of a branch are walked", show(f));
    }
    {
        const auto f = of(fp_cmov);
        check(f.ok && f.reads == R8, "a cmov destination is neither read nor written", show(f));
    }
    {
        const auto f = of(fp_retf);
        check(f.ok && !f.reads && f.rets == fp::kRetXmm0, "xmm0 written last before ret", show(f));
    }
    {
        const auto f = of(fp_loop);
        check(f.ok && f.reads == RCX && f.insns <= 6 && !f.truncated, "a loop is walked once", show(f));
    }
    {
        const auto f = of(fp_blind);
        check(f.ok && f.stack_blind && f.reads == RCX, "rsp realigned: the fingerprint says it cannot follow stack arguments", show(f));
    }
    {
        const auto f = of(fp_bad);
        check(!f.ok && !f.why.empty(), "bytes that do not decode: no fingerprint", f.why);
    }
    {
        fp::Reader r;
        r.bytes = +[](void*, uint64_t, uint8_t*, size_t) -> size_t { return 0; };
        const auto f = fp::compute(r, reinterpret_cast<uint64_t>(fp_time));
        check(!f.ok && f.why.find("not available") != std::string::npos, "no file bytes: no fingerprint", f.why);
    }
}

void test_window() {
    std::puts("-- without a function table the 4 KiB window decides");
    const auto f = of(fp_through, false);
    // fp_time is before fp_through: a backward target is outside [entry, entry + 4 KiB) -> a tail call
    check(f.ok && f.tail && f.reads == RCX, "a jump before the entry is a tail call", show(f));
}

void test_check() {
    g_unwind = false;
    std::puts("-- descriptor against fingerprint");
    std::string t;
    const auto time = of(fp_time);
    check(fp::check(shape({{"daytime", KILN_SIG_PTR}, {"scale", KILN_SIG_F32}, {"ecsNow", KILN_SIG_S64}}), time, &t) ==
                  fp::Verdict::Match &&
              t == "3 arguments (rcx, xmm1, r8), the function reads [rcx, xmm1, r8]",
          "the right descriptor matches", t);
    check(fp::check(shape({{"daytime", KILN_SIG_PTR}, {"ecsNow", KILN_SIG_S64}}), time, &t) == fp::Verdict::Mismatch &&
              t == "descriptor arg 2 'ecsNow' is rdx, the function reads [rcx, xmm1, r8]",
          "an integer where the function takes a float is refused naming the argument", t);
    check(fp::check(shape({{"daytime", KILN_SIG_PTR}, {"scale", KILN_SIG_F32}}), time, &t) == fp::Verdict::Mismatch &&
              t.find("the function reads r8, the descriptor has 2 arguments") == 0,
          "a read the descriptor lacks is refused", t);
    check(fp::check(shape({{"daytime", KILN_SIG_PTR}, {"scale", KILN_SIG_F32}, {"ecsNow", KILN_SIG_S64}, {"extra", KILN_SIG_U32}}),
                    time, &t) == fp::Verdict::Mismatch &&
              t.find("descriptor arg 4 'extra' is r9") == 0,
          "a declared argument the function neither reads nor passes is refused", t);
    check(fp::check(shape({{"a", KILN_SIG_PTR}, {"b", KILN_SIG_F32}, {"c", KILN_SIG_S64}, {"d", KILN_SIG_U32}, {"e", KILN_SIG_U8}}),
                    time, &t) == fp::Verdict::Mismatch,
          "...also on the stack", t);
    // pass-through: declared arguments it only passes are fine
    check(fp::check(shape({{"out", KILN_SIG_PTR}, {"s", KILN_SIG_PTR}, {"acc", KILN_SIG_PTR}, {"e", KILN_SIG_U32}}), of(fp_through), &t) ==
              fp::Verdict::Match,
          "a jump-through function matches a descriptor whose arguments it passes on", t);
    // sret: the buffer is position 0, the declared arguments shift by one
    check(fp::check(shape({{"item", KILN_SIG_PTR}}, true), of(fp_branches), &t) == fp::Verdict::Mismatch &&
              t.find("xmm2") != std::string::npos,
          "sret shifts the declared arguments (here the function also reads xmm2: refused)", t);
    {
        // fp_stack: rcx rdx r8 r9 + two stack slots
        const auto st = of(fp_stack);
        check(fp::check(shape({{"a", KILN_SIG_PTR}, {"b", KILN_SIG_PTR}, {"c", KILN_SIG_U64}, {"d", KILN_SIG_U64},
                               {"e", KILN_SIG_U32}, {"f", KILN_SIG_U8}}), st, &t) == fp::Verdict::Match,
              "six positions, two on the stack", t);
        check(fp::check(shape({{"a", KILN_SIG_PTR}, {"b", KILN_SIG_PTR}, {"c", KILN_SIG_U64}, {"d", KILN_SIG_U64}, {"e", KILN_SIG_U32}}),
                        st, &t) == fp::Verdict::Mismatch &&
                  t.find("the function reads stack+0x30, the descriptor has 5 arguments") == 0,
              "a stack read the descriptor lacks is refused", t);
        check(fp::check(shape({{"buf", KILN_SIG_PTR}, {"b", KILN_SIG_U64}, {"c", KILN_SIG_U64}, {"e", KILN_SIG_U32}, {"f", KILN_SIG_U8}},
                              true), st, &t) == fp::Verdict::Match,
              "the same function described with a hidden return buffer and five arguments", t);
    }
    check(fp::check(shape({{"self", KILN_SIG_PTR}, {"x", KILN_SIG_U64}}), of(fp_indirect), &t) == fp::Verdict::Unknown,
          "an argument lost behind an indirect jump is unknown", t);
    check(fp::check(shape({{"self", KILN_SIG_PTR}}), of(fp_indirect), &t) == fp::Verdict::Match,
          "...a descriptor that declares only what was read still matches", t);
    check(fp::check(shape({{"p", KILN_SIG_PTR}, {"b", KILN_SIG_U64}, {"c", KILN_SIG_U64}, {"d", KILN_SIG_U64}, {"e", KILN_SIG_U64}}),
                    of(fp_tail), &t) == fp::Verdict::Match,
          "a tail call takes the stack arguments with it", t);
    check(fp::check(shape({{"p", KILN_SIG_PTR}, {"b", KILN_SIG_U64}, {"c", KILN_SIG_U64}, {"d", KILN_SIG_U64}, {"e", KILN_SIG_U64}}),
                    of(fp_blind), &t) == fp::Verdict::Mismatch,
          "stack blind does not excuse unread registers", t);
    check(fp::check(shape({{"x", KILN_SIG_U64}}), of(fp_bad), &t) == fp::Verdict::Unknown, "no fingerprint is unknown", t);
}

void test_parse() {
    std::puts("-- the sig block");
    fp::Structs structs;
    std::string err;
    check(fp::parse_structs(R"({"OptS32": {"size": 8, "align": 4, "fields": [{"name": "value", "type": "s32", "offset": 0},
                                {"name": "has", "type": "u8", "offset": 4}]},
                                "WeatherSample": {"size": 16, "align": 4}, "Tiny": {"size": 4, "align": 4}})",
                            &structs, &err) && structs.size() == 3 && structs.find("OptS32")->second.fields.size() == 2,
          "structs parse", err);
    check(!fp::parse_structs(R"({"OptS32": {"size": 8}})", &structs, &err) && err.find("twice") != std::string::npos,
          "a struct declared twice is refused", err);
    fp::Structs scratch;
    check(!fp::parse_structs(R"({"X": {"size": 4, "fields": [{"name": "a", "type": "u64", "offset": 0}]}})", &scratch, &err),
          "a field behind the struct's size is refused", err);
    check(!fp::parse_structs(R"({"X": {"align": 4}})", &scratch, &err), "a struct without a size is refused", err);
    err.clear();

    fp::Sig s;
    const char* time = R"({"conv": "win64", "ret": "void",
        "args": [{"name": "daytime", "type": "keen::Daytime*"}, {"name": "scale", "type": "f32"}, {"name": "ecsNow", "type": "s64"}],
        "abi": {"reads": ["rcx", "xmm1", "r8"], "stack": [], "ret": "none"}, "invoke": "tick", "since": "1287369"})";
    check(fp::parse_sig(time, structs, false, &s, &err) && s.args.size() == 3 && s.args[0].kind == KILN_SIG_PTR &&
              s.args[1].kind == KILN_SIG_F32 && s.args[2].kind == KILN_SIG_S64 && s.ret_kind == KILN_SIG_VOID && !s.sret &&
              s.has_abi && s.abi_reads == (RCX | X1 | R8) && s.abi_stack == 0 && s.abi_ret == "none" && s.invoke == "tick",
          "the chapter's SetTimeScale block parses", err);
    std::string t;
    check(fp::check_sig(s, of(fp_time), &t) == fp::Verdict::Match, "...and matches its function", t);
    check(fp::check_sig(s, of(fp_branches), &t) == fp::Verdict::Mismatch && t.find("review the descriptor") != std::string::npos,
          "...and is refused on a function with another fingerprint", t);
    check(fp::to_json(of(fp_time)).find(R"("reads":["rcx","xmm1","r8"])") != std::string::npos, "JSON lists registers in position order",
          fp::to_json(of(fp_time)));
    const char* vol = R"({"sret": "OptS32", "args": [{"name": "item", "type": "keen::ItemInfo*"}], "invoke": "any"})";
    check(fp::parse_sig(vol, structs, false, &s, &err) && s.sret && s.ret_kind == KILN_SIG_STRUCT && s.ret_size == 8 &&
              s.ret_type == "OptS32" && s.positions() == 2 && fp::shape_of(s).names[0] == "(return buffer)",
          "sret: the buffer is not in args, positions count it", err);
    const char* stack = R"({"ret": "bool", "args": [{"name": "r", "type": "ptr"}, {"name": "hash", "type": "u32"},
        {"name": "fn", "type": "ptr"}, {"name": "name", "type": "ptr"}, {"name": "argc", "type": "u8"}],
        "abi": {"reads": ["rcx", "rdx", "r8", "r9"], "stack": ["0x28"], "ret": "rax"}})";
    check(fp::parse_sig(stack, structs, false, &s, &err) && s.abi_stack == 1 && s.ret_kind == KILN_SIG_BOOL && s.invoke == "none",
          "a stack argument and its abi slot", err);
    const char* byval = R"({"ret": "Tiny", "args": [{"name": "w", "type": "WeatherSample"}, {"name": "t", "type": "Tiny"}]})";
    check(fp::parse_sig(byval, structs, false, &s, &err) && s.args[0].kind == KILN_SIG_BYVAL && s.args[0].size == 16 &&
              s.args[1].size == 4 && s.ret_kind == KILN_SIG_STRUCT && s.ret_size == 4 && !s.sret,
          "structs by value: sizes from the struct table; a 4-byte struct returns in rax", err);
    auto bad = [&](const char* text, const char* needle, bool trust = false) {
        fp::Sig x;
        std::string e;
        const bool ok = fp::parse_sig(text, structs, trust, &x, &e);
        return !ok && e.find(needle) != std::string::npos;
    };
    check(bad(R"({"args": [{"name": "a", "type": "Vec3"}]})", "Vec3"), "an unknown type is refused by name");
    check(bad(R"({"ret": "WeatherSample"})", "sret"), "a 16-byte struct return must be declared as sret");
    check(bad(R"({"sret": "Nope"})", "Nope"), "an unknown sret struct");
    check(bad(R"({"sret": "OptS32", "ret": "u32"})", "disagree"), "sret with another return");
    check(bad(R"({"args": [{"name": "out", "type": "sret"}]})", "never in args"), "sret listed in args");
    check(bad(R"({"conv": "sysv"})", "win64"), "another convention");
    check(bad(R"({"varargs": true})", "varargs"), "varargs");
    check(bad(R"({"invoke": "always"})", "invoke"), "a bad invoke value");
    check(bad(R"({"abi": "trust"})", "only in a mod's own fragment"), "trust in a core descriptor");
    check(bad(R"({"abi": {"reads": ["rax"]}})", "rax"), "a register that is not an argument register");
    check(bad(R"({"abi": {"stack": ["0x20"]}})", "0x28"), "a stack offset inside the home space");
    check(bad(R"({"args": [{"name": "a", "type": "u8"}, {"name": "a", "type": "u8"}]})", "twice"), "an argument name twice");
    check(bad("[1,2]", "object") && bad("{", "sig:"), "not an object, not JSON");
    {
        fp::Sig x;
        check(fp::parse_sig(R"({"args": [{"name": "self", "type": "ptr"}, {"name": "x", "type": "u64"}], "abi": "trust"})",
                            structs, true, &x, &err) && x.trust && !x.has_abi,
              "a fragment may say trust", err);
        check(fp::check_sig(x, of(fp_indirect), &t) == fp::Verdict::Match && t.find("trusted") != std::string::npos,
              "...which turns unknown into match", t);
        check(fp::check_sig(x, of(fp_time), &t) == fp::Verdict::Mismatch, "...but never a mismatch", t);
    }
}

void put32(std::vector<uint8_t>& v, size_t at, uint32_t x) { std::memcpy(v.data() + at, &x, 4); }
void put16(std::vector<uint8_t>& v, size_t at, uint16_t x) { std::memcpy(v.data() + at, &x, 2); }

void test_pdata() {
    std::puts("-- the exception table of a mapped image");
    std::vector<uint8_t> img(0x3000, 0);
    put16(img, 0, 0x5A4D);
    put32(img, 0x3C, 0x80);
    put32(img, 0x80, 0x00004550);
    const size_t opt = 0x80 + 24;
    put16(img, opt, 0x20B);
    put32(img, opt + 108, 16);
    put32(img, opt + 112 + 3 * 8, 0x1000);       // exception directory
    put32(img, opt + 112 + 3 * 8 + 4, 4 * 12);
    // four entries: A [0x2000,0x2040), A's chained part [0x2040,0x2080), a gap, B [0x2100,0x2110), C [0x2200,0x2210)
    const uint32_t e[4][3] = {{0x2000, 0x2040, 0x1100}, {0x2040, 0x2080, 0x1110}, {0x2100, 0x2110, 0x1100}, {0x2200, 0x2210, 0x1100}};
    for (int i = 0; i < 4; ++i)
        for (int k = 0; k < 3; ++k) put32(img, 0x1000 + i * 12 + k * 4, e[i][k]);
    img[0x1100] = 1;                              // version 1, no flags, no codes
    img[0x1110] = 1 | (4 << 3);                   // UNW_FLAG_CHAININFO
    img[0x1112] = 1;                              // one unwind code (rounded up to two slots)
    put32(img, 0x1110 + 4 + 4, 0x2000);           // the parent RUNTIME_FUNCTION
    put32(img, 0x1110 + 4 + 8, 0x2040);
    put32(img, 0x1110 + 4 + 12, 0x1100);
    fp::Pdata p;
    check(fp::pdata_init(&p, img.data(), img.size()) && p.count == 4, "the directory is found");
    uint64_t primary = 0, next = 0;
    check(fp::pdata_lookup(p, 0x2010, &primary, &next) && primary == 0x2000 && next == 0x2040, "inside the first part");
    check(fp::pdata_lookup(p, 0x2050, &primary, &next) && primary == 0x2000 && next == 0x2100, "a chained part belongs to its first part");
    check(!fp::pdata_lookup(p, 0x2090, &primary, &next) && next == 0x2100, "a gap is in no function; next is the one behind it");
    check(fp::pdata_lookup(p, 0x2100, &primary, &next) && primary == 0x2100 && next == 0x2200, "the start of an entry");
    check(!fp::pdata_lookup(p, 0x1FFF, &primary, &next) && next == 0x2000, "before the first entry");
    check(!fp::pdata_lookup(p, 0x2210, &primary, &next) && next == UINT64_MAX, "behind the last entry");
    fp::Pdata q;
    img[0] = 0;
    check(!fp::pdata_init(&q, img.data(), img.size()) && !fp::pdata_init(&q, nullptr, 0), "not a PE image: no table");
}

}  // namespace

// ---- the vector file shared with the Python twin (tools/sigport/tests/fingerprint-vectors.json) ----
// Snippets are bytes at a base address with no exception table; each has the fingerprint the core's pass gives it, and
// descriptor cases the verdict and text of check_sig. tools/sigport/tests/test_abifp.py checks the same file with
// abifp.py (and the exe entries, by RVA, with both passes).
struct Blob {
    uint64_t base = 0;
    std::vector<uint8_t> code;
};
size_t blob_bytes(void* ctx, uint64_t addr, uint8_t* out, size_t n) {
    const Blob& b = *static_cast<const Blob*>(ctx);
    if (addr < b.base || addr >= b.base + b.code.size()) return 0;
    n = static_cast<size_t>(std::min<uint64_t>(n, b.base + b.code.size() - addr));
    std::memcpy(out, b.code.data() + (addr - b.base), n);
    return n;
}
Blob blob_of(const nlohmann::json& v) {
    Blob b;
    b.base = std::strtoull(v.at("base").get<std::string>().c_str(), nullptr, 0);
    const std::string hex = v.at("hex").get<std::string>();
    for (size_t i = 0; i + 1 < hex.size(); i += 2) b.code.push_back(static_cast<uint8_t>(std::strtoul(hex.substr(i, 2).c_str(), nullptr, 16)));
    return b;
}
fp::Fingerprint blob_fp(const Blob& b, const nlohmann::json& v) {
    fp::Reader r;
    r.ctx = const_cast<Blob*>(&b);
    r.bytes = &blob_bytes;
    const uint64_t entry = v.contains("entry") ? std::strtoull(v["entry"].get<std::string>().c_str(), nullptr, 0) : b.base;
    return fp::compute(r, entry);
}

// print: the core's answers as JSON (tools/sigport/tests/make_vectors.py records them); else: compare with them
int test_vectors(const char* path, bool print) {
    nlohmann::ordered_json doc;
    try {
        std::ifstream in(path);
        doc = nlohmann::ordered_json::parse(in);
    } catch (const std::exception& e) {
        check(false, "the shared vector file", std::string(path) + ": " + e.what());
        return 1;
    }
    nlohmann::ordered_json out = {{"snippets", nlohmann::ordered_json::object()}, {"sigs", nlohmann::ordered_json::object()}};
    std::map<std::string, Blob> blobs;
    if (!print) std::printf("-- the shared vector file (%zu snippets, %zu descriptor cases)\n", doc["snippets"].size(), doc["sigs"].size());
    for (const auto& v : doc["snippets"]) {
        const std::string name = v.at("name");
        blobs[name] = blob_of(v);
        const auto got = nlohmann::ordered_json::parse(fp::to_json(blob_fp(blobs[name], v)));
        if (print) out["snippets"][name] = got;
        else check(got == v.at("expect"), ("vector " + name).c_str(), got == v.at("expect") ? "" : got.dump() + " expected " + v.at("expect").dump());
    }
    fp::Structs structs;
    std::string err;
    if (doc.contains("structs") && !fp::parse_structs(doc["structs"].dump(), &structs, &err)) {
        check(false, "the shared vector file's structs", err);
        return 1;
    }
    if (!print) check(doc["snippets"].size() >= 30 && doc["sigs"].size() >= 10, "the shared vector file has its cases");
    for (const auto& v : doc["sigs"]) {
        const std::string name = v.at("name");
        const auto& snip = doc["snippets"];
        auto it = std::find_if(snip.begin(), snip.end(), [&](const auto& x) { return x.at("name") == v.at("snippet"); });
        if (it == snip.end()) {
            check(false, ("vector sig " + name).c_str(), "no snippet " + v.at("snippet").get<std::string>());
            continue;
        }
        fp::Sig sig;
        nlohmann::ordered_json got;
        if (!fp::parse_sig(v.at("sig").dump(), structs, v.value("trust_allowed", false), &sig, &err)) {
            got = {{"verdict", "bad"}, {"text", err}};
        } else {
            std::string text;
            const fp::Verdict verdict = fp::check_sig(sig, blob_fp(blobs[it->at("name")], *it), &text);
            got = {{"verdict", fp::verdict_name(verdict)}, {"text", text}};
        }
        if (print) out["sigs"][name] = got;
        else check(got == v.at("expect"), ("vector sig " + name).c_str(), got == v.at("expect") ? "" : got.dump() + " expected " + v.at("expect").dump());
    }
    if (print) std::printf("%s\n", out.dump(2).c_str());
    return 0;
}

int main(int argc, char** argv) {
    // hooks_fingerprint_test [--print-vectors] [<vector file>]
    const bool print = argc > 1 && std::strcmp(argv[1], "--print-vectors") == 0;
    const char* vectors = argc > (print ? 2 : 1) ? argv[print ? 2 : 1] : nullptr;
    if (print) return vectors ? test_vectors(vectors, true) : 2;
    g_starts = {reinterpret_cast<uint64_t>(fp_time),    reinterpret_cast<uint64_t>(fp_through), reinterpret_cast<uint64_t>(fp_thunk),
                reinterpret_cast<uint64_t>(fp_indirect), reinterpret_cast<uint64_t>(fp_stack),   reinterpret_cast<uint64_t>(fp_idioms),
                reinterpret_cast<uint64_t>(fp_call),     reinterpret_cast<uint64_t>(fp_branches), reinterpret_cast<uint64_t>(fp_cmov),
                reinterpret_cast<uint64_t>(fp_retf),     reinterpret_cast<uint64_t>(fp_loop),    reinterpret_cast<uint64_t>(fp_blind),
                reinterpret_cast<uint64_t>(fp_tail),     reinterpret_cast<uint64_t>(fp_bad),     reinterpret_cast<uint64_t>(fp_end)};
    std::sort(g_starts.begin(), g_starts.end());
    test_pass(false);
    test_pass(true);
    test_window();
    {
        fp::Sig loaded;
        loaded.ret_kind = KILN_SIG_F32; loaded.ret_size = 4;
        loaded.args.push_back({"x", "f32", KILN_SIG_F32, 4});
        KilnSigArg arg{{nullptr, 0}, KILN_SIG_F32, 4};
        KilnSig c{}; c.size = sizeof c; c.argc = 1; c.args = &arg;
        c.ret_kind = KILN_SIG_F32; c.ret_size = 4;
        std::string reason;
        check(fp::compiled_matches(c, loaded, &reason), "compiled descriptor equals resolver descriptor");
        arg.kind = KILN_SIG_S32;
        check(!fp::compiled_matches(c, loaded, &reason), "compiled integer against shipped float refused");
        arg.kind = KILN_SIG_F32; c.argc = 0;
        check(!fp::compiled_matches(c, loaded, &reason), "compiled stack/argument count differs");
        c.argc = 1; c.ret_kind = KILN_SIG_S32;
        check(!fp::compiled_matches(c, loaded, &reason), "compiled return class differs");
        // kiln_hooks.h: a scalar's size is "the kind's own size or 0", so 0 is the same descriptor
        c.ret_kind = KILN_SIG_F32; c.ret_size = 0; arg.size = 0;
        check(fp::compiled_matches(c, loaded, &reason), "a scalar argument and return of size 0 equal their kind's size", reason);
        arg.size = 8;
        check(!fp::compiled_matches(c, loaded, &reason), "a scalar argument of another non-zero size is refused");
        arg.size = 4; c.ret_size = 2;
        check(!fp::compiled_matches(c, loaded, &reason), "a scalar return of another non-zero size is refused");
        // a by-value struct's size is the struct's: never implied
        fp::Sig byval;
        byval.args.push_back({"v", "Vec3", KILN_SIG_BYVAL, 12});
        KilnSigArg bv{{nullptr, 0}, KILN_SIG_BYVAL, 12};
        KilnSig cb{}; cb.size = sizeof cb; cb.argc = 1; cb.args = &bv;
        check(fp::compiled_matches(cb, byval, &reason), "a by-value struct of the loaded size matches");
        bv.size = 16;
        check(!fp::compiled_matches(cb, byval, &reason), "a by-value struct of another size is refused");
    }
    test_check();
    test_parse();
    test_pdata();
    if (vectors) test_vectors(vectors, false);
    else check(false, "the shared vector file", "not given (test-hooks-fingerprint.sh passes it)");
    std::printf("%d passed, %d failed\n", g_pass, g_fail);
    std::puts(g_fail ? "FAILED" : "all passed");
    return g_fail ? 1 : 0;
}
