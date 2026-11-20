#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""abifp: the ABI fingerprint of a game function, the Python twin of the core's pass (capstone here, Zydis there).

The contract is the header comment of native/core/src/services/hooks_fingerprint.h; this file follows it rule for rule,
in the same order, so sigport can check a ported symbol's `sig` descriptor on a new game build without the core. A shared
vector file (tools/sigport/tests/fingerprint-vectors.json) holds expected fingerprints that both passes are tested against:
synthetic snippets (bytes of our own) by hooks_fingerprint_test.cpp and test_abifp.py, functions of the game exes (by RVA,
never their bytes) by test_abifp.py against this pass and kiln-fptool (the core's).

    python3 tools/sigport/abifp.py <exe> <rva> [<rva> ...]      one fingerprint (JSON, the core's text) per line
"""
import bisect
import json
import struct
import sys
from pathlib import Path

import capstone
from capstone import x86

ARG_REGS = ["rcx", "rdx", "r8", "r9", "xmm0", "xmm1", "xmm2", "xmm3"]
RAX_BIT = 1 << 8
ALL_WRITTEN = 0x1FF
MAX_INSNS = 300
WINDOW = 4096
STACK_SLOTS = 16
RET_NONE, RET_RAX, RET_XMM0, RET_CALLEE = 1, 2, 4, 8
BY_POSITION = [0, 4, 1, 5, 2, 6, 3, 7]  # rcx xmm0 rdx xmm1 r8 xmm2 r9 xmm3
INT64_MIN = -(1 << 63)

# the largest enclosing general register of every name capstone uses, as an index rax..r15 (0..15)
_GPR64 = ["rax", "rcx", "rdx", "rbx", "rsp", "rbp", "rsi", "rdi"] + [f"r{i}" for i in range(8, 16)]
_GPR = {}
for _i, (_q, _d, _w, _b) in enumerate([("rax", "eax", "ax", "al"), ("rcx", "ecx", "cx", "cl"), ("rdx", "edx", "dx", "dl"),
                                       ("rbx", "ebx", "bx", "bl"), ("rsp", "esp", "sp", "spl"), ("rbp", "ebp", "bp", "bpl"),
                                       ("rsi", "esi", "si", "sil"), ("rdi", "edi", "di", "dil")]):
    for _n in (_q, _d, _w, _b):
        _GPR[_n] = _i
for _n, _i in (("ah", 0), ("ch", 1), ("dh", 2), ("bh", 3)):
    _GPR[_n] = _i
for _i in range(8, 16):
    for _s in ("", "d", "w", "b"):
        _GPR[f"r{_i}{_s}"] = _i
RSP, RBP = 4, 5
VOLATILE = (1 << 0) | (1 << 1) | (1 << 2) | (1 << 8) | (1 << 9) | (1 << 10) | (1 << 11)

ZERO_IDIOM = {"xor", "sub", "pxor", "xorps", "xorpd", "vxorps", "vxorpd", "vpxor"}
MERGE_WRITE = {"movss", "movsd", "cvtsi2ss", "cvtsi2sd", "cvtss2sd", "cvtsd2ss", "sqrtss", "sqrtsd", "roundss", "roundsd",
               "rcpss", "rsqrtss"}
COND_BRANCH_EXTRA = {"jrcxz", "jecxz", "loop", "loope", "loopne"}

_md = None


def _decoder():
    global _md
    if _md is None:
        _md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
        _md.detail = True
    return _md


def reg_bit(name):
    """The mask bit of a register (argument registers 0..7, rax 8), or 0."""
    if not name:
        return 0
    for kind in ("xmm", "ymm", "zmm"):
        if name.startswith(kind) and name[3:].isdigit() and int(name[3:]) < 4:
            return 1 << (4 + int(name[3:]))
    g = _GPR.get(name)
    return {1: 1 << 0, 2: 1 << 1, 8: 1 << 2, 9: 1 << 3, 0: RAX_BIT}.get(g, 0)


def gpr_index(name):
    return _GPR.get(name, -1) if name else -1


def is64(name):
    return name in _GPR64


class Fingerprint:
    def __init__(self):
        self.ok = False
        self.why = ""
        self.entry = 0
        self.thunk = False
        self.reads = 0
        self.passes = 0
        self.indirect = 0
        self.stack = 0
        self.tail = False
        self.stack_blind = False
        self.rets = 0
        self.insns = 0
        self.truncated = False


class _Path:
    __slots__ = ("addr", "written", "off", "known", "last")

    def __init__(self, addr):
        self.addr = addr
        self.written = 0
        self.off = [0] * 16
        self.known = 1 << RSP
        self.last = "none"

    def copy(self):
        p = _Path(self.addr)
        p.written, p.off, p.known, p.last = self.written, list(self.off), self.known, self.last
        return p

    def knows(self, r):
        return r >= 0 and bool(self.known & (1 << r))


def mnemonic(i):
    """The mnemonic without prefixes (capstone writes "rep stosq", "lock cmpxchg", "notrack jmp"; Zydis's has none)."""
    return i.mnemonic.split()[-1]


def _rep(i):
    parts = i.mnemonic.split()
    return len(parts) > 1 and parts[0].startswith("rep")


def _category(i):
    m = mnemonic(i)
    if i.group(x86.X86_GRP_RET) or i.group(x86.X86_GRP_IRET) or m in ("ret", "retf", "retfq"):
        return "ret"
    if m in ("call", "lcall"):
        return "call"
    if m in ("jmp", "ljmp"):
        return "uncond"
    if (m.startswith("j") and i.group(x86.X86_GRP_JUMP)) or m in COND_BRANCH_EXTRA:
        return "cond"
    if m.startswith("cmov"):
        return "cmov"
    return ""


class _Ops:
    """One operand as the pass sees it. `cw`: a conditional write (Zydis CONDWRITE): it loses a register's known value
    but does not mark it written."""
    __slots__ = ("type", "reg", "base", "index", "disp", "rd", "wr", "cw", "visible", "agen")

    def __init__(self, type_, reg="", rd=False, wr=False, visible=False):
        self.type, self.reg, self.rd, self.wr, self.visible = type_, reg, rd, wr, visible
        self.base = self.index = ""
        self.disp = 0
        self.cw = self.agen = False


STRING_OPS = ("movs", "stos", "lods", "scas", "cmps", "ins", "outs")
STORES = ("mov", "vmov", "set", "pextr", "vpextr", "extractps", "vextract", "fist", "fst", "fbstp", "vcvtps2ph", "stmxcsr",
          "vstmxcsr")
RSI, RDI, RCX = 6, 7, 1


def _is_string(m, ops=()):
    """A string instruction (movsb .. outsq); movsd and cmpsd with an xmm operand are the SSE ones."""
    if any(o.type == "reg" and o.reg[:3] in ("xmm", "ymm", "zmm") for o in ops):
        return False
    return any(m.startswith(p) and len(m) == len(p) + 1 and m[-1] in "bwdq" for p in STRING_OPS)


def _operands(i):
    """The operands, as Zydis gives them (the visible ones, then the hidden registers): capstone's view with the
    corrections that make it Zydis's (each found by comparing the two decoders over the game exes' code,
    out/06b-typed-hooks S5; tests/fingerprint-vectors.json keeps a snippet of each)."""
    m = mnemonic(i)
    ops = []
    for o in i.operands:
        x = _Ops("imm", visible=True)
        x.rd = bool(o.access & capstone.CS_AC_READ)
        x.wr = bool(o.access & capstone.CS_AC_WRITE)
        if o.type == x86.X86_OP_REG:
            x.type, x.reg = "reg", i.reg_name(o.reg)
        elif o.type == x86.X86_OP_MEM:
            x.type = "mem"
            x.base = i.reg_name(o.mem.base) if o.mem.base else ""
            x.index = i.reg_name(o.mem.index) if o.mem.index else ""
            if x.index in ("riz", "eiz"):  # capstone's pseudo index of a SIB without one; Zydis has none
                x.index = ""
            x.disp = o.mem.disp
            x.agen = m == "lea"
        ops.append(x)
    imp_r = [i.reg_name(r) for r in i.regs_read]
    imp_w = [i.reg_name(r) for r in i.regs_write]
    cond_w = []
    if m == "test":  # capstone marks `test reg, imm` a write and `test mem, reg` no read at all: it only reads
        for x in ops:
            if x.type in ("reg", "mem"):
                x.rd, x.wr = True, False
    elif m in ("movhlps", "movlhps", "movhpd", "movlpd", "movhps", "movlps") and ops and ops[0].type == "reg":
        ops[0].rd = False  # Zydis: the destination is written only
    elif m in ("cdq", "cqo", "cwd"):
        imp_r, imp_w = ["rax"], ["rdx"]
    elif m == "enter":
        imp_r, imp_w = imp_r + ["rsp", "rbp"], imp_w + ["rsp", "rbp"]
    elif m == "vzeroupper":  # it keeps the low 128 bits of every ymm (the xmm arguments): Zydis gives it no register
        imp_r, imp_w = [], []
    elif m in ("pcmpestrm", "pcmpestri", "pcmpistrm", "pcmpistri", "vpcmpestrm", "vpcmpestri", "vpcmpistrm", "vpcmpistri"):
        if "estr" in m:
            imp_r = imp_r + ["rax", "rdx"]
        imp_w = imp_w + (["xmm0"] if m.endswith("m") else ["ecx"])
    elif m in ("cmpxchg",):
        imp_r = imp_r + ["rax"]
        cond_w = ["rax"]
        imp_w = [r for r in imp_w if gpr_index(r) != 0]
    elif m in ("vbroadcasti128", "vbroadcastf128") and ops and ops[0].type == "reg":
        ops[0].wr = True
        for x in ops[1:]:
            if x.type == "mem":
                x.rd = True
    elif "gather" in m and len(ops) >= 3 and ops[-1].type == "reg":
        ops[-1].wr = True  # the mask register is cleared as the gather goes
    if ops and ops[0].type == "mem" and m.startswith(STORES) and not _is_string(m, ops):
        ops[0].rd = False  # a store's destination: capstone marks the memory of SSE/AVX moves, setcc, fistp as read
    if ops and ops[0].type == "reg" and "{k" in i.op_str and "{z}" not in i.op_str and ops[0].wr:
        ops[0].wr, ops[0].cw = False, True  # AVX-512 merge masking: Zydis writes the destination conditionally
    if _is_string(m, ops):
        # As Zydis has them: stos moves rdi, lods rsi, movs both, scas, cmps, ins and outs keep them; a rep prefix adds
        # rcx (all of them conditional writes)
        imp_w = [r for r in imp_w if gpr_index(r) not in (RSI, RDI, RCX)]
        if _rep(i):
            cond_w.append("rcx")
        if m.startswith("stos"):
            cond_w.append("rdi")
        elif m.startswith("lods"):
            cond_w.append("rsi")
        elif m.startswith("movs"):
            cond_w += ["rsi", "rdi"]
    for r in imp_r:
        ops.append(_Ops("reg", r, rd=True))
    for r in imp_w:
        ops.append(_Ops("reg", r, wr=True))
    for r in cond_w:
        x = _Ops("reg", r)
        x.cw = True
        ops.append(x)
    return ops


def gpr_index_in(names, idx):
    return any(gpr_index(n) == idx for n in names)


def _rel_target(i):
    """The target of a relative branch (capstone resolves it), or None."""
    if i.operands and i.operands[0].type == x86.X86_OP_IMM and (
            i.group(x86.X86_GRP_JUMP) or i.group(x86.X86_GRP_CALL) or mnemonic(i) in COND_BRANCH_EXTRA):
        return i.operands[0].imm & 0xFFFFFFFFFFFFFFFF
    return None


def compute(read, entry, function=None):
    """The fingerprint of the code at `entry`. `read(addr, n)` gives up to n bytes of the original code (b"" = none);
    `function(addr)` (optional, the exception table) gives (inside, primary, next)."""
    f = Fingerprint()
    f.entry = entry
    md = _decoder()

    def decode(a):
        b = read(a, 15)
        if not b:
            return None
        for i in md.disasm(b, a, 1):
            return i
        return None

    i = decode(entry)
    if i is None:
        f.why = "the entry does not decode" if read(entry, 1) else "the exe file's code is not available"
        return f
    if mnemonic(i) == "jmp" and i.size == 5 and _rel_target(i) is not None:
        entry = _rel_target(i)
        f.thunk = True
        if decode(entry) is None:
            f.why = "the entry's jump target does not decode"
            return f
    f.entry = entry
    f.ok = True

    have_fn, fn_primary, fn_next = False, 0, 1 << 64
    if function:
        have_fn, fn_primary, fn_next = function(entry)

    def inside(t):
        if t < entry or t >= entry + WINDOW:
            return False
        if not function:
            return True
        in_fn, p, _n = function(t)
        if have_fn:
            return in_fn and p == fn_primary
        return not in_fn and t < fn_next

    seen = set()

    def first_visit(p):
        key = (p.addr, p.written, p.off[RSP] if p.knows(RSP) else INT64_MIN)
        if key in seen:
            return False
        seen.add(key)
        return True

    work = [_Path(entry)]
    passes = indirect = 0
    while work and f.insns < MAX_INSNS:
        p = work.pop()
        if not first_visit(p):
            continue
        while f.insns < MAX_INSNS:
            a = p.addr
            i = decode(a)
            if i is None:
                break
            f.insns += 1
            m = mnemonic(i)
            cat = _category(i)
            ops = _operands(i)
            vis = [o for o in ops if o.visible]
            nvis = len(vis)

            # ---- reads and writes ----
            zero = False
            if m in ZERO_IDIOM and nvis >= 2:
                s1, s2 = vis[nvis - 2], vis[nvis - 1]
                zero = s1.type == "reg" and s2.type == "reg" and s1.reg == s2.reg
            merge = m in MERGE_WRITE and nvis >= 1 and vis[0].type == "reg"
            cmov = cat == "cmov"
            reads = writes = 0
            for k, o in enumerate(ops):
                if o.type == "reg":
                    bit = reg_bit(o.reg)
                    if not bit:
                        continue
                    first = o.visible and k == 0
                    if first and cmov:
                        continue
                    if o.visible and zero:
                        if k == 0:
                            writes |= bit
                        continue
                    if first and merge:
                        writes |= bit
                        continue
                    if o.rd:
                        reads |= bit
                    if o.wr:
                        writes |= bit
                elif o.type == "mem":
                    reads |= reg_bit(o.base) | reg_bit(o.index)
                    base = gpr_index(o.base)
                    if p.knows(base):
                        off = p.off[base] + o.disp
                        args = 0x28 <= off < 0x28 + 8 * STACK_SLOTS
                        if o.index or o.agen:
                            if ((0 <= off < 0x28 + 8 * STACK_SLOTS) if o.index else args):
                                f.stack_blind = True
                        elif o.rd and args:
                            f.stack |= 1 << ((off - 0x28) // 8)
            f.reads |= reads & ~p.written & 0xFF
            p.written |= writes
            if writes & RAX_BIT:
                p.last = "rax"
            if writes & (1 << 4):
                p.last = "xmm0"

            # ---- registers that hold the entry rsp plus an offset ----
            before = p.known
            value, set_reg = 0, -1
            d64 = nvis >= 1 and vis[0].type == "reg" and is64(vis[0].reg)
            dst = gpr_index(vis[0].reg) if d64 else -1
            if dst >= 0 and nvis == 2:
                src = vis[1]
                if m == "mov" and src.type == "reg" and is64(src.reg) and p.knows(gpr_index(src.reg)):
                    set_reg, value = dst, p.off[gpr_index(src.reg)]
                elif m == "lea" and src.type == "mem" and not src.index and p.knows(gpr_index(src.base)):
                    set_reg, value = dst, p.off[gpr_index(src.base)] + src.disp
                elif m in ("add", "sub") and src.type == "imm" and p.knows(dst):
                    imm = i.operands[1].imm
                    set_reg, value = dst, p.off[dst] + (imm if m == "add" else -imm)
            stack_op = m in ("push", "pop", "pushfq", "popfq", "leave") or cat in ("call", "ret")
            for o in ops:  # every other register write loses its value
                if o.type != "reg" or not (o.wr or o.cw):
                    continue
                r = gpr_index(o.reg)
                if r < 0 or (r == RSP and stack_op):
                    continue
                p.known &= ~(1 << r) & 0xFFFF
            if set_reg >= 0:
                p.off[set_reg] = value
                p.known |= 1 << set_reg
            if m in ("push", "pushfq"):
                p.off[RSP] -= 8
            elif m in ("pop", "popfq"):
                if not (nvis >= 1 and vis[0].type == "reg" and gpr_index(vis[0].reg) == RSP):
                    p.off[RSP] += 8
            elif m == "leave":  # mov rsp, rbp; pop rbp
                if before & (1 << RBP):
                    p.off[RSP] = p.off[RBP] + 8
                    p.known |= 1 << RSP
                else:
                    p.known &= ~(1 << RSP) & 0xFFFF
                p.known &= ~(1 << RBP) & 0xFFFF
            elif cat == "call":
                p.known &= ~VOLATILE & 0xFFFF
            if (before & (1 << RSP)) and not p.knows(RSP):
                f.stack_blind = True

            # ---- control flow ----
            untouched = ~p.written & 0xFF
            if cat == "ret":
                f.rets |= {"none": RET_NONE, "rax": RET_RAX, "xmm0": RET_XMM0, "callee": RET_CALLEE}[p.last]
                break
            if m in ("int3", "ud2", "hlt"):
                break
            if cat == "call":
                passes |= untouched
                p.written = ALL_WRITTEN
                p.last = "callee"
            elif cat == "uncond":
                t = _rel_target(i)
                if t is not None and inside(t):
                    p.addr = t
                    if not first_visit(p):
                        break
                    continue
                if t is None:
                    indirect |= untouched
                passes |= untouched
                f.rets |= RET_CALLEE
                f.tail = True
                break
            elif cat == "cond":
                t = _rel_target(i)
                if t is not None and inside(t):
                    q = p.copy()
                    q.addr = t
                    work.append(q)
            p.addr = a + i.size
    f.truncated = bool(work)
    f.passes = passes & ~f.reads & 0xFF
    f.indirect = indirect & ~f.reads & 0xFF
    return f


def _regs(mask):
    return [ARG_REGS[i] for i in BY_POSITION if mask & (1 << i)]


def _stack(mask):
    return [hex(0x28 + 8 * k) for k in range(STACK_SLOTS) if mask & (1 << k)]


def ret_text(rets):
    s = [n for bit, n in ((RET_NONE, "none"), (RET_RAX, "rax"), (RET_XMM0, "xmm0"), (RET_CALLEE, "callee")) if rets & bit]
    return "|".join(s) if s else "none"


def to_dict(f):
    """The fingerprint as the core's to_json writes it (same keys, same order)."""
    d = {"result": "ok" if f.ok and not f.indirect else "unknown"}
    if not f.ok:
        d["why"] = f.why
    d.update({"reads": _regs(f.reads), "passes": _regs(f.passes), "indirect": _regs(f.indirect), "stack": _stack(f.stack),
              "ret": ret_text(f.rets), "insns": f.insns})
    if f.thunk:
        d["thunk"] = True
    if f.tail:
        d["tail"] = True
    if f.stack_blind:
        d["stack_blind"] = True
    if f.truncated:
        d["truncated"] = True
    return d


def from_dict(d):
    """A Fingerprint from the core's JSON (to_dict's inverse, as far as the checks read it)."""
    f = Fingerprint()
    f.ok = "why" not in d
    f.why = d.get("why", "")
    f.reads = _mask(d.get("reads"))
    f.passes = _mask(d.get("passes"))
    f.indirect = _mask(d.get("indirect"))
    f.stack = 0
    for off in d.get("stack", []):
        f.stack |= 1 << ((int(off, 16) - 0x28) // 8)
    f.tail, f.stack_blind, f.truncated, f.thunk = (bool(d.get(k)) for k in ("tail", "stack_blind", "truncated", "thunk"))
    f.insns = d.get("insns", 0)
    for name, bit in (("none", RET_NONE), ("rax", RET_RAX), ("xmm0", RET_XMM0), ("callee", RET_CALLEE)):
        if name in d.get("ret", "").split("|"):
            f.rets |= bit
    return f


def reads_text(d):
    """"[rcx, xmm1, r8, stack+0x28]" of a fingerprint dict."""
    return "[" + ", ".join(d["reads"] + ["stack+" + s for s in d["stack"]]) + "]"


# ---- descriptors: the core's parse_sig / shape_of / check / check_sig, with the same verdicts and texts ----

SCALARS = {"s8": 1, "u8": 1, "s16": 2, "u16": 2, "s32": 4, "u32": 4, "s64": 8, "u64": 8, "f32": 4, "f64": 8, "bool": 1, "ptr": 8}
REG_INDEX = {n: i for i, n in enumerate(ARG_REGS)}


def _mask(names):
    m = 0
    for n in names or []:
        if n not in REG_INDEX:
            raise ValueError(f"'{n}' is not one of rcx rdx r8 r9 xmm0 xmm1 xmm2 xmm3")
        m |= 1 << REG_INDEX[n]
    return m


def parse_sig(sig, structs=None, allow_trust=False):
    """A `sig` block (a dict) as the checks need it: {"names": [...], "floats": mask, "sret", "abi": ..., "trust"}.
    Raises ValueError for a descriptor the core refuses as malformed (the texts are shorter than the core's)."""
    structs = structs or {}
    if not isinstance(sig, dict):
        raise ValueError("sig must be an object")
    if sig.get("conv", "win64") != "win64" or sig.get("varargs"):
        raise ValueError("conv must be \"win64\" (varargs and other conventions cannot be described)")
    names, kinds = [], []
    sret = "sret" in sig
    if sret and sig["sret"] not in structs:
        raise ValueError(f"sret: struct '{sig['sret']}' is not declared under \"structs\"")
    if sret:
        names.append("(return buffer)")
        kinds.append("ptr")
    for a in sig.get("args", []):
        t = a.get("type", "")
        if t not in SCALARS and not t.endswith("*") and t not in structs:
            raise ValueError(f"arg '{a.get('name', '')}': type '{t}' is neither a scalar, a pointer nor a struct")
        names.append(a.get("name", ""))
        kinds.append(t)
    if len(names) > 20:
        raise ValueError("more than 20 argument positions")
    floats = 0
    for k, t in enumerate(kinds[:4]):
        if t in ("f32", "f64"):
            floats |= 1 << k
    abi = sig.get("abi")
    out = {"names": names, "floats": floats, "sret": sret, "trust": False, "has_abi": False}
    if abi == "trust":
        if not allow_trust:
            raise ValueError("\"abi\": \"trust\" is allowed only in a mod's own fragment")
        out["trust"] = True
    elif isinstance(abi, dict):
        stack = 0
        for e in abi.get("stack", []):
            off = int(e, 0) if isinstance(e, str) else int(e)
            if off < 0x28 or off >= 0x28 + 8 * STACK_SLOTS or off & 7:
                raise ValueError("abi.stack: offsets are 0x28 + 8k above the entry rsp, k < 16")
            stack |= 1 << ((off - 0x28) // 8)
        out.update(has_abi=True, abi_reads=_mask(abi.get("reads")), abi_passes=_mask(abi.get("passes")), abi_stack=stack)
    elif abi is not None:
        raise ValueError("abi must be an object or \"trust\"")
    return out


def _list_text(regs, stack):
    parts = [ARG_REGS[i] for i in BY_POSITION if regs & (1 << i)]
    parts += [f"stack+{hex(0x28 + 8 * k)}" for k in range(STACK_SLOTS) if stack & (1 << k)]
    return "[" + ", ".join(parts) + "]"


def _reg_of(sh, k):
    return 4 + k if sh["floats"] & (1 << k) else k


def _arg_label(sh, pos):
    if sh["sret"] and pos == 0:
        return "the return buffer"
    s = f"arg {pos + 1 - (1 if sh['sret'] else 0)}"
    if pos < len(sh["names"]) and sh["names"][pos]:
        s += f" '{sh['names'][pos]}'"
    return s


def _place(sh, pos):
    return ARG_REGS[_reg_of(sh, pos)] if pos < 4 else f"stack+{hex(0x28 + 8 * (pos - 4))}"


def check(sh, f):
    """(verdict, text) of a descriptor shape against a fingerprint: the core's check()."""
    reads = _list_text(f.reads, f.stack)
    if not f.ok:
        return "unknown", "the function's code could not be read: " + f.why
    positions = len(sh["names"])
    declared = 0
    for k in range(min(positions, 4)):
        declared |= 1 << _reg_of(sh, k)
    nstack = positions - 4 if positions > 4 else 0
    nargs = positions - (1 if sh["sret"] else 0)
    count = f"{nargs} argument" + ("" if nargs == 1 else "s") + (" and a return buffer" if sh["sret"] else "")
    for k in range(min(positions, 4)):
        other = k if sh["floats"] & (1 << k) else 4 + k
        if f.reads & (1 << other):
            return "mismatch", f"descriptor {_arg_label(sh, k)} is {_place(sh, k)}, the function reads {reads}"
    for i in range(8):
        if f.reads & (1 << i) and not declared & (1 << i):
            return "mismatch", f"the function reads {ARG_REGS[i]}, the descriptor has {count}; it reads {reads}"
    for k in range(STACK_SLOTS):
        if f.stack & (1 << k) and k >= nstack:
            return "mismatch", f"the function reads stack+{hex(0x28 + 8 * k)}, the descriptor has {count}; it reads {reads}"
    unknown = None
    for k in range(positions):
        if k < 4:
            bit = 1 << _reg_of(sh, k)
            used = bool((f.reads | f.passes) & bit)
            lost = bool(f.indirect & bit)
        else:
            read = bool(f.stack & (1 << (k - 4)))
            used = read or f.tail or f.stack_blind
            lost = not read and (f.indirect != 0 or f.stack_blind)
        if not used:
            return "mismatch", (f"descriptor {_arg_label(sh, k)} is {_place(sh, k)}, the function reads {reads}"
                                + (f" and passes {_list_text(f.passes, 0)}" if f.passes else ""))
        if lost and unknown is None:
            unknown = (f"descriptor {_arg_label(sh, k)} is {_place(sh, k)}: the function's use of it could not be followed "
                       f"(it reads {reads})")
    if unknown is not None:
        return "unknown", unknown
    places = ", ".join(_place(sh, k) for k in range(positions))
    return "match", f"{count} ({places}), the function reads {reads}"


def check_sig(sig, f):
    """(verdict, text) of a parsed descriptor against the code: the core's check_sig()."""
    if f.ok and sig["has_abi"]:
        if sig["abi_reads"] != f.reads or sig["abi_stack"] != f.stack:
            return "mismatch", (f"the function reads {_list_text(f.reads, f.stack)}, the descriptor's fingerprint says "
                                f"{_list_text(sig['abi_reads'], sig['abi_stack'])} (the function changed: review the descriptor)")
        if sig["abi_passes"] & ~f.passes:
            return "mismatch", (f"the function passes {_list_text(f.passes, 0)}, the descriptor's fingerprint says "
                                f"{_list_text(sig['abi_passes'], 0)} (the function changed: review the descriptor)")
    v, t = check(sig, f)
    if v == "unknown" and sig["trust"]:
        return "match", t + " (trusted by the fragment)"
    return v, t


# ---- an exe file as the core and kiln-fptool read it: sections at their RVAs, the first executable section is code ----

class ExeCode:
    def __init__(self, path):
        b = Path(path).read_bytes()
        if len(b) < 0x200 or b[:2] != b"MZ":
            raise ValueError(f"{path}: not a PE file")
        nt = struct.unpack_from("<I", b, 0x3C)[0]
        if b[nt:nt + 4] != b"PE\0\0" or struct.unpack_from("<H", b, nt + 24)[0] != 0x20B:
            raise ValueError(f"{path}: not a PE32+ file")
        nsec, optsize = struct.unpack_from("<H", b, nt + 6)[0], struct.unpack_from("<H", b, nt + 20)[0]
        size_of_image, size_of_headers = struct.unpack_from("<II", b, nt + 24 + 56)
        mem = bytearray(size_of_image)
        mem[:min(size_of_headers, len(b))] = b[:min(size_of_headers, len(b))]
        self.text_lo = self.text_hi = 0
        sec = nt + 24 + optsize
        for k in range(nsec):
            s = sec + 40 * k
            vsize, rva, rsize, raw = struct.unpack_from("<IIII", b, s + 8)
            flags = struct.unpack_from("<I", b, s + 36)[0]
            n = min(min(rsize, vsize if vsize else rsize), max(0, len(b) - raw))
            mem[rva:rva + n] = b[raw:raw + n]
            if (flags & 0x20000000) and not self.text_hi:
                self.text_lo, self.text_hi = rva, rva + max(vsize, rsize)
        self.mem = bytes(mem)
        self._begins, self._entries = [], []
        opt = nt + 24
        if struct.unpack_from("<I", b, opt + 108)[0] >= 4:
            prva, psize = struct.unpack_from("<II", b, opt + 112 + 3 * 8)
            if prva and psize >= 12 and prva + psize <= len(self.mem):
                for k in range(psize // 12):
                    beg, end, unwind = struct.unpack_from("<III", self.mem, prva + 12 * k)
                    self._entries.append((beg, end, unwind))
                self._begins = [e[0] for e in self._entries]

    def read(self, addr, n):
        if addr < self.text_lo or addr >= self.text_hi:
            return b""
        n = min(n, self.text_hi - addr, len(self.mem) - addr)
        return self.mem[addr:addr + n]

    def function(self, rva):
        """As pdata_lookup: (inside, primary, next)."""
        if not self._entries:
            return False, 0, 1 << 64
        lo = bisect.bisect_right(self._begins, rva)
        nxt = self._begins[lo] if lo < len(self._begins) else 1 << 64
        if lo == 0:
            return False, 0, nxt
        beg, end, unwind = self._entries[lo - 1]
        if rva >= end:
            return False, 0, nxt
        e = (beg, end, unwind)
        for _hop in range(8):
            u = e[2]
            if u & 1:  # an indirect entry names another RUNTIME_FUNCTION
                u &= ~1
                if u + 12 > len(self.mem):
                    break
                e = struct.unpack_from("<III", self.mem, u)
                continue
            if u + 4 > len(self.mem):
                break
            flags = self.mem[u] >> 3
            if not (flags & 4):  # UNW_FLAG_CHAININFO
                break
            codes = (self.mem[u + 2] + 1) & ~1
            if u + 4 + codes * 2 + 12 > len(self.mem):
                break
            e = struct.unpack_from("<III", self.mem, u + 4 + codes * 2)
        return True, e[0], nxt

    def compute(self, rva):
        return compute(self.read, rva, self.function if self._entries else None)

    def fingerprint(self, rva):
        return to_dict(self.compute(rva))


def of_bytes(code, base, entry=None):
    """The fingerprint of a snippet (no exception table): `code` at `base`, entered at `entry` (default `base`)."""
    def read(a, n):
        if a < base or a >= base + len(code):
            return b""
        return code[a - base:a - base + n]
    return to_dict(compute(read, base if entry is None else entry))


def main(argv):
    if len(argv) < 3:
        print(__doc__.strip().splitlines()[-1].strip(), file=sys.stderr)
        return 2
    exe = ExeCode(argv[1])
    for a in argv[2:]:
        print(json.dumps({"rva": hex(int(a, 0)), **exe.fingerprint(int(a, 0))}))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
