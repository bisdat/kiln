// SPDX-License-Identifier: MIT
// kiln.ui: a working copy of one game function, placed elsewhere (menu_footer.cpp; docs/re/game-ui.md "The main
// menu's footer"). Plain logic over byte buffers with Zydis: no Windows, no game memory, so tests/code_clone_test.cpp
// runs it on the build machine, also over the real function from the game's exe when it is installed.
//
// The copy keeps the layout byte for byte: every instruction stays at the same offset with the same length. Only the
// relative fields that point outside the function (rip-relative data, calls and jumps out of it) are rewritten for the
// copy's address, and one call can be sent elsewhere (Kiln's thunk). A jump inside the function keeps its field, since
// source and target moved together. A relative field to the outside that is not 32 bits wide, an indirect jump (a jump
// table could lead back into the original), an undecodable byte or a target out of the copy's reach refuses the copy:
// the caller then runs the original instead.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

// Zydis.h repeats the static-build defines the target already passes on the command line.
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wmacro-redefined"
#endif
#include "Zydis.h"
#if defined(__clang__)
#pragma clang diagnostic pop
#endif

namespace code_clone {

// One relative field of an instruction: where it is and where it points.
struct Rel {
    uint32_t at = 0;      // the instruction's offset in the function
    uint32_t len = 0;     // the instruction's length
    uint32_t field = 0;   // the field's offset in the instruction
    uint32_t bits = 0;    // the field's width
    uint64_t target = 0;  // the absolute address it points at (for the function at its source address)
    bool call = false;    // a near call (E8 rel32)
    bool inside = false;  // the target lies inside the function
};

// Every instruction of the function, decoded. False (and `err`) when a byte does not decode, the last instruction runs
// past `n`, or an indirect jump is found.
inline bool scan(const uint8_t* code, size_t n, uint64_t addr, std::vector<Rel>* rels, std::vector<uint32_t>* starts,
                 std::string* err) {
    ZydisDecoder dec{};
    if (!ZYAN_SUCCESS(ZydisDecoderInit(&dec, ZYDIS_MACHINE_MODE_LONG_64, ZYDIS_STACK_WIDTH_64))) {
        if (err) *err = "the decoder did not start";
        return false;
    }
    size_t o = 0;
    while (o < n) {
        ZydisDecodedInstruction ins{};
        ZydisDecodedOperand ops[ZYDIS_MAX_OPERAND_COUNT];
        if (!ZYAN_SUCCESS(ZydisDecoderDecodeFull(&dec, code + o, n - o, &ins, ops))) {
            if (err) *err = "a byte at +0x" + std::to_string(o) + " does not decode";
            return false;
        }
        if (starts) starts->push_back(static_cast<uint32_t>(o));
        const bool branch = ins.meta.category == ZYDIS_CATEGORY_UNCOND_BR || ins.meta.category == ZYDIS_CATEGORY_COND_BR;
        if (branch && ins.operand_count_visible > 0 && ops[0].type != ZYDIS_OPERAND_TYPE_IMMEDIATE) {
            if (err) *err = "an indirect jump at +0x" + std::to_string(o);
            return false;
        }
        if (ins.attributes & ZYDIS_ATTRIB_IS_RELATIVE) {
            for (uint8_t i = 0; i < ins.operand_count_visible; ++i) {
                const ZydisDecodedOperand& op = ops[i];
                Rel r;
                r.at = static_cast<uint32_t>(o);
                r.len = ins.length;
                if (op.type == ZYDIS_OPERAND_TYPE_MEMORY && op.mem.base == ZYDIS_REGISTER_RIP) {
                    r.field = ins.raw.disp.offset;
                    r.bits = ins.raw.disp.size;
                } else if (op.type == ZYDIS_OPERAND_TYPE_IMMEDIATE && op.imm.is_relative) {
                    const int k = ins.raw.imm[0].is_relative ? 0 : 1;
                    r.field = ins.raw.imm[k].offset;
                    r.bits = ins.raw.imm[k].size;
                } else {
                    continue;
                }
                ZyanU64 t = 0;
                if (!ZYAN_SUCCESS(ZydisCalcAbsoluteAddress(&ins, &op, addr + o, &t))) {
                    if (err) *err = "no target for the instruction at +0x" + std::to_string(o);
                    return false;
                }
                r.target = t;
                r.call = ins.mnemonic == ZYDIS_MNEMONIC_CALL && ins.length == 5 && code[o] == 0xE8;
                r.inside = t >= addr && t < addr + n;
                if (rels) rels->push_back(r);
            }
        }
        o += ins.length;
    }
    if (o != n) {
        if (err) *err = "the last instruction runs past the function's end";
        return false;
    }
    return true;
}

inline bool fits32(int64_t d) { return d >= INT32_MIN && d <= INT32_MAX; }

// Copies the function `src` (n bytes, at `src_addr`) to `dst` (at least n bytes; it will run at `dst_addr`) and
// rewrites its outside references for that address. `redirect_at` (an offset, or UINT32_MAX for none) names a near
// call whose target becomes `redirect_to`. False (and `err`): see the file comment.
inline bool relocate(const uint8_t* src, size_t n, uint64_t src_addr, uint8_t* dst, uint64_t dst_addr,
                     uint32_t redirect_at, uint64_t redirect_to, std::string* err) {
    std::vector<Rel> rels;
    if (!scan(src, n, src_addr, &rels, nullptr, err)) return false;
    std::memcpy(dst, src, n);
    bool redirected = redirect_at == UINT32_MAX;
    for (const Rel& r : rels) {
        uint64_t target = r.target;
        if (r.at == redirect_at) {
            if (!r.call) {
                if (err) *err = "the instruction to send elsewhere is not a near call";
                return false;
            }
            target = redirect_to;
            redirected = true;
        } else if (r.inside) {
            continue;  // moved together with its target: the field stays
        }
        if (r.bits != 32) {
            if (err) *err = "a " + std::to_string(r.bits) + "-bit relative field to the outside at +0x" + std::to_string(r.at);
            return false;
        }
        const int64_t d = static_cast<int64_t>(target - (dst_addr + r.at + r.len));
        if (!fits32(d)) {
            if (err) *err = "a target out of the copy's reach at +0x" + std::to_string(r.at);
            return false;
        }
        const int32_t v = static_cast<int32_t>(d);
        std::memcpy(dst + r.at + r.field, &v, 4);
    }
    if (!redirected) {
        if (err) *err = "no instruction at the offset to send elsewhere";
        return false;
    }
    return true;
}

// The copy checked against the original: the same instructions at the same offsets, and every relative field of the
// copy (at `copy_addr`) points where the original's does (at `src_addr`), except the redirected call, which points at
// `redirect_to`. Kiln runs this on the copy it made before it ever calls it.
inline bool same_meaning(const uint8_t* src, uint64_t src_addr, const uint8_t* copy, uint64_t copy_addr, size_t n,
                         uint32_t redirect_at, uint64_t redirect_to, std::string* err) {
    std::vector<Rel> a, b;
    std::vector<uint32_t> sa, sb;
    if (!scan(src, n, src_addr, &a, &sa, err) || !scan(copy, n, copy_addr, &b, &sb, err)) return false;
    if (sa != sb || a.size() != b.size()) {
        if (err) *err = "the copy's instructions differ from the original's";
        return false;
    }
    for (size_t i = 0; i < a.size(); ++i) {
        const uint64_t want = a[i].at == redirect_at ? redirect_to
                              : a[i].inside            ? a[i].target - src_addr + copy_addr
                                                       : a[i].target;
        if (b[i].at != a[i].at || b[i].target != want) {
            if (err) *err = "the copy's field at +0x" + std::to_string(a[i].at) + " points elsewhere";
            return false;
        }
    }
    // every byte outside the relative fields is the original's
    std::vector<uint8_t> mask(n, 0);
    for (const Rel& r : a)
        for (uint32_t k = 0; k < r.bits / 8; ++k) mask[r.at + r.field + k] = 1;
    for (size_t i = 0; i < n; ++i)
        if (!mask[i] && src[i] != copy[i]) {
            if (err) *err = "the copy's byte at +0x" + std::to_string(i) + " differs";
            return false;
        }
    return true;
}

// Where the live function differs from the file's: each differing instruction of the file's code once.
struct Change {
    uint32_t at = 0, len = 0;
    bool call = false;       // a near call in both, only its target changed (a call-site patch)
    uint64_t live_target = 0;  // that call's live target
};
inline std::vector<Change> changes(const uint8_t* file, const uint8_t* live, size_t n, uint64_t addr) {
    std::vector<Change> out;
    std::vector<uint32_t> starts;
    if (!scan(file, n, addr, nullptr, &starts, nullptr)) return out;
    starts.push_back(static_cast<uint32_t>(n));
    for (size_t i = 0; i + 1 < starts.size(); ++i) {
        const uint32_t a = starts[i], len = starts[i + 1] - a;
        if (std::memcmp(file + a, live + a, len) == 0) continue;
        Change c;
        c.at = a;
        c.len = len;
        c.call = len == 5 && file[a] == 0xE8 && live[a] == 0xE8;
        if (c.call) {
            int32_t rel = 0;
            std::memcpy(&rel, live + a + 1, 4);
            c.live_target = addr + a + 5 + static_cast<int64_t>(rel);
        }
        out.push_back(c);
    }
    return out;
}

}  // namespace code_clone
