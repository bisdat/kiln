#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Writes tools/sigport/tests/fingerprint-vectors.json: the cases both fingerprint passes are tested against (the core's,
native/core/tests/hooks_fingerprint_test.cpp; the Python twin, test_abifp.py). The expected values are the core's
answers, never the twin's:

  snippets  our own assembly (llvm-mc, Intel syntax) as bytes at a base address, one per rule of the pass and one per
            place capstone and Zydis were found to see an instruction differently; expected: the core pass's JSON
  sigs      descriptors checked against a snippet; expected: check_sig's verdict and text
  exes      the described functions of the game exes, by RVA (never their bytes) with the exe's sha256; expected:
            kiln-fptool's fingerprint. Tests skip an exe that is not given (SIGPORT_SERVER / SIGPORT_CLIENT)

    python3 tools/sigport/tests/make_vectors.py <hooks_fingerprint_test> <kiln-fptool> [--server <exe>] [--client <exe>]
(build both with native/core/tests/test-hooks-fingerprint.sh and tools/sigport/fptool/build.sh)."""
import argparse
import hashlib
import json
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[2]
OUT = HERE / "fingerprint-vectors.json"
BASE = 0x140010000

FAR_JMP = ".byte 0xE9\n.long -0x2000"     # a jmp rel32 out of the snippet: a tail call
FAR_CALL = ".byte 0xE8\n.long -0x3000"    # a call of another function

SNIPPETS = {
    # void SetTimeScale(Daytime* d, float scale, s64 now): rcx, xmm1, r8
    "time": "movss dword ptr [rcx+0x10], xmm1\nmov qword ptr [rcx+0x18], r8\nret",
    # an entry that is only a jmp rel32 to the real code
    "thunk": ".byte 0xE9\n.long 1f - . - 4\nint3\n1:\nmov eax, edx\nret",
    # a jump-through: one register shuffled, the rest left to another function
    "through": f"mov rax, qword ptr [rcx+8]\nmov rcx, rax\n{FAR_JMP}",
    # a virtual tail call: the arguments' use cannot be followed
    "indirect": "mov rax, qword ptr [rcx]\njmp qword ptr [rax+0x18]",
    # an MSVC frame: rax copies rsp, rbp from it, two stack arguments read through rbp and rsp
    "msvc_frame": ("mov rax, rsp\nmov qword ptr [rax+8], rbx\npush rbp\nlea rbp, [rax-0x58]\nsub rsp, 0x140\ntest rdx, rdx\n"
                   "mov rbx, rcx\nadd r8, r9\nmov eax, dword ptr [rbp+0x80]\nmovzx ecx, byte ptr [rsp+0x178]\nadd eax, ecx\n"
                   "add rsp, 0x140\npop rbp\nret"),
    # zeroing idioms and merging writes are writes, not reads
    "idioms": "xor ecx, ecx\nxorps xmm1, xmm1\ncvtsi2ss xmm2, edx\nmovss xmm3, xmm0\nret",
    "vex_zero": "vxorps xmm0, xmm2, xmm2\nvpxor xmm1, xmm1, xmm1\nret",
    "merge": "movsd xmm1, xmm2\nmovaps xmm0, xmm1\nret",
    # a call: what it has not touched is passed on; afterwards the volatile registers are the callee's
    "call": f"sub rsp, 0x28\nmov r8, rcx\n{FAR_CALL}\nmov rcx, rdx\nmovaps xmm1, xmm0\nadd rsp, 0x28\nret",
    # both sides of a branch count
    "branches": "test ecx, ecx\njz 1f\nmov eax, edx\nret\n1:\nmovd eax, xmm2\nret",
    # cmov's destination is neither read nor written
    "cmov": "cmovz rcx, rdx\nmov rax, rcx\nret",
    # a frame pointer and leave
    "leave": "push rbp\nmov rbp, rsp\nmov eax, dword ptr [rbp+0x30]\nleave\nret",
    "pushfq": "pushfq\nmov eax, dword ptr [rsp+0x30]\npopfq\nret",
    # the address of an argument slot, an indexed access into the argument area: stack blind
    "blind_lea": "lea rax, [rsp+0x28]\nmov ecx, dword ptr [rax]\nret",
    "blind_index": "mov eax, dword ptr [rsp+rcx*8+0x28]\nret",
    # narrow registers are their family's
    "narrow": "mov al, cl\nmovzx eax, dx\nret",
    "ymm": "vmovups ymm1, ymmword ptr [rdx]\nvaddps ymm0, ymm1, ymm2\nret",
    # the instruction limit with a path left over
    "truncated": "test ecx, ecx\njz 1f\n" + "nop\n" * 310 + "1:\nret",
    "hlt": "mov eax, ecx\nhlt",
    # where capstone and Zydis see an instruction differently (the twin follows Zydis)
    "vzeroupper": f"vmovups ymm0, ymmword ptr [rcx]\nvzeroupper\n{FAR_JMP}",
    "test_mem": "push rbp\nmov rbp, rsp\ntest byte ptr [rbp+0x30], al\npop rbp\nret",
    "test_imm": "test ecx, 0x7fffffff\njz 1f\nmov rax, rdx\n1:\nret",
    "test_imm_rax": "test eax, 0x7fffffff\nret",
    "cdq": "mov eax, ecx\ncdq\nidiv r8d\nret",
    "cqo": "mov rax, rcx\ncqo\nmov rcx, rdx\nret",
    "rep_stos": "mov rdi, rcx\nrep stosb\nmov rax, rdx\nret",
    "stos_rdi": "mov rdi, rsp\nstosd\nmov eax, dword ptr [rdi+0x24]\nret",
    "movs_sse": "movsd xmm0, xmm1\nret",
    "store_arg": "movups xmmword ptr [rsp+0x28], xmm0\nret",
    "setcc_arg": "test ecx, ecx\nsetz byte ptr [rsp+0x28]\nret",
    "cmpxchg": "lock cmpxchg qword ptr [rcx], rdx\nret",
    "movhlps": "movhlps xmm1, xmm0\nmovaps xmm0, xmm1\nret",
    "movhpd": "movhpd xmm0, qword ptr [rcx]\nret",
    "pcmpestri": "mov eax, 16\nmov edx, 16\npcmpestri xmm1, xmmword ptr [r8], 0xc\nmov eax, ecx\nret",
    "broadcast": "push rbp\nmov rbp, rsp\nvbroadcasti128 ymm2, xmmword ptr [rbp+0x30]\nvmovups ymmword ptr [rcx], ymm2\npop rbp\nvzeroupper\nret",
    "gather": "vpgatherdd xmm5, dword ptr [rdx + xmm2*4], xmm3\nmovaps xmm0, xmm3\nret",
    "masked": "vpsubq zmm3 {k1}, zmm0, zmm3\nvmovaps xmm0, xmm3\nret",
}

STRUCTS = {"OptS32": {"size": 8, "align": 4, "fields": [{"name": "value", "type": "s32", "offset": 0},
                                                           {"name": "has", "type": "u8", "offset": 4}]}}

SIGS = [
    ("time_ok", "time", {"ret": "void", "args": [{"name": "daytime", "type": "ptr"}, {"name": "scale", "type": "f32"},
                                                 {"name": "ecsNow", "type": "s64"}], "abi": {"reads": ["rcx", "xmm1", "r8"], "stack": []}}),
    ("time_names_ecsnow", "time", {"args": [{"name": "daytime", "type": "ptr"}, {"name": "ecsNow", "type": "s64"}]}),
    ("time_two_args", "time", {"args": [{"name": "daytime", "type": "ptr"}, {"name": "scale", "type": "f32"}]}),
    ("time_four_args", "time", {"args": [{"name": "daytime", "type": "ptr"}, {"name": "scale", "type": "f32"},
                                         {"name": "ecsNow", "type": "s64"}, {"name": "extra", "type": "u32"}]}),
    ("time_abi_changed", "time", {"args": [{"name": "daytime", "type": "ptr"}, {"name": "scale", "type": "f32"},
                                           {"name": "ecsNow", "type": "s64"}], "abi": {"reads": ["rcx", "xmm1"], "stack": []}}),
    ("through_passes", "through", {"args": [{"name": "self", "type": "ptr"}, {"name": "a", "type": "ptr"},
                                            {"name": "b", "type": "ptr"}, {"name": "c", "type": "ptr"}],
                                   "abi": {"reads": ["rcx"], "passes": ["rdx", "r8", "r9"], "stack": []}}),
    ("indirect_unknown", "indirect", {"args": [{"name": "self", "type": "ptr"}, {"name": "a", "type": "u32"}]}),
    ("indirect_trusted", "indirect", {"args": [{"name": "self", "type": "ptr"}, {"name": "a", "type": "u32"}], "abi": "trust"}),
    ("trust_refused", "indirect", {"args": [{"name": "self", "type": "ptr"}], "abi": "trust"}),
    ("msvc_stack", "msvc_frame", {"args": [{"name": "a", "type": "ptr"}, {"name": "b", "type": "ptr"}, {"name": "c", "type": "u64"},
                                           {"name": "d", "type": "u64"}, {"name": "e", "type": "u32"}, {"name": "f", "type": "u8"}]}),
    ("msvc_one_stack_short", "msvc_frame", {"args": [{"name": "a", "type": "ptr"}, {"name": "b", "type": "ptr"}, {"name": "c", "type": "u64"},
                                                     {"name": "d", "type": "u64"}, {"name": "e", "type": "u32"}]}),
    ("sret_volume", "leave", {"sret": "OptS32", "args": [{"name": "item", "type": "ptr"}]}),
    ("bad_type", "time", {"args": [{"name": "x", "type": "nosuch"}]}),
]

SIDES = {"server": "SIGPORT_SERVER", "client": "SIGPORT_CLIENT"}


def assemble(src):
    with tempfile.TemporaryDirectory() as t:
        s, o, b = Path(t) / "s.s", Path(t) / "s.o", Path(t) / "s.bin"
        s.write_text(".intel_syntax noprefix\n.text\n" + src + "\n")
        subprocess.run(["llvm-mc", "-triple=x86_64-pc-linux-gnu", "-filetype=obj", "-mattr=+avx512f", "-o", str(o), str(s)], check=True)
        subprocess.run(["llvm-objcopy", "-O", "binary", "--only-section=.text", str(o), str(b)], check=True)
        return b.read_bytes()


def described(side):
    """(symbol, sig) of every descriptor of the core's definitions for a side."""
    sys.path.insert(0, str(HERE.parent))
    from sides import merged  # noqa: E402
    doc = merged(REPO / "sigdb" / "symbols" / f"{side}.json")
    out = dict(doc.get("sigs", {}))
    for name, entry in doc.get("symbols", {}).items():
        if isinstance(entry, dict) and "sig" in entry:
            out[name] = entry["sig"]
    return out


def main(argv):
    ap = argparse.ArgumentParser()
    ap.add_argument("test_binary")
    ap.add_argument("fptool")
    ap.add_argument("--server")
    ap.add_argument("--client")
    a = ap.parse_args(argv)
    doc = {"comment": __doc__.split("\n\n")[0].replace("\n", " "), "structs": STRUCTS, "snippets": [], "sigs": [], "exes": []}
    for name, src in SNIPPETS.items():
        doc["snippets"].append({"name": name, "asm": src.split("\n"), "base": hex(BASE), "hex": assemble(src).hex(), "expect": None})
    for name, snip, sig in SIGS:
        case = {"name": name, "snippet": snip, "sig": sig, "expect": None}
        if name == "indirect_trusted":
            case["trust_allowed"] = True
        doc["sigs"].append(case)
    with tempfile.NamedTemporaryFile("w", suffix=".json", delete=False) as t:
        json.dump(doc, t)
    core = json.loads(subprocess.run([a.test_binary, "--print-vectors", t.name], capture_output=True, text=True, check=True).stdout)
    for v in doc["snippets"]:
        v["expect"] = core["snippets"][v["name"]]
    for v in doc["sigs"]:
        v["expect"] = core["sigs"][v["name"]]
    for side in ("server", "client"):
        exe = getattr(a, side)
        if not exe:
            continue
        sha = hashlib.sha256(Path(exe).read_bytes()).hexdigest()
        rec = json.loads((REPO / "sigdb" / "builds" / side / f"{sha}.json").read_text())
        names = sorted(described(side))
        rvas = {n: int(rec["symbols"][n], 16) for n in names if n in rec["symbols"]}
        with tempfile.NamedTemporaryFile("w", suffix=".json", delete=False) as t:
            json.dump({"rvas": sorted(rvas.values())}, t)
        fps = json.loads(subprocess.run([a.fptool, exe, t.name], capture_output=True, text=True, check=True).stdout)["rvas"]
        for n, r in sorted(rvas.items()):
            doc["exes"].append({"side": side, "sha256": sha, "symbol": n, "rva": hex(r), "expect": fps[hex(r)]})
    OUT.write_text(json.dumps(doc, indent=1) + "\n")
    print(f"{OUT}: {len(doc['snippets'])} snippets, {len(doc['sigs'])} descriptor cases, {len(doc['exes'])} exe functions")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
