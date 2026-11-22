#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""sfi_stage.py: Kiln's interpreter for ShroudForge mods (kiln-sfi) as a release component, and kiln-eml's archive
inspector beside it, in a Kiln stage.

tools/package/stage.sh runs it:
  python3 -I tools/sfi/sfi_stage.py version <repo> <platform folder> <platform>
          writes <platform folder>/version.json for the helper staged there: its tool name, version (kiln-eml's
          Cargo.toml, the version its answers name), platform, file name and SHA-256, the schema version, schema file
          and SHA-256 and digest framing it answers in, and the Kiln it was built with. The launcher and kilnctl take
          the helper's version and SHA-256 from it and the install receipt (kiln-manage sfi_inspection::installed).
  python3 -I tools/sfi/sfi_stage.py check <repo> <stage dir>
          the stage holds both, complete and where they belong:
          - component sfi (MIT): exactly kiln/plugins/kiln-sfi.dll, a non-empty AMD64 PE32+ DLL; installed by default
            on both sides and requiring core and eml; no kiln-sfi.* anywhere else (never swept into core, never in the
            hosted ShroudForge folder); ShroudForge's runtime (component shroudforge) still there and a default;
          - kiln-sfi.dll carries the core's build stamp: its kiln_build_stamp() export equals the stamp every native
            service DLL in core/kiln/services exports (one build: the core's), that stamp is "<VERSION><build metadata>"
            and kiln-core.dll holds the same build metadata. The core makes kiln-sfi.dll the ShroudForge-dialect owner
            only then (native/core/src/api.cpp load_one); without it kiln mode never starts. Read from the files, never
            run: the export's code is the "lea rax, [rip+X]; ret" both builds emit, X the NUL-terminated stamp;
          - component eml: kiln/tools/eml-inspect/<platform>/ for linux-x86_64 (an x86-64 ELF executable, mode 0755) and
            windows-x86_64 (an AMD64 PE32+ executable, not a DLL), each with exactly its LICENSE.txt and NOTICE.txt
            (gpl/kiln-eml's), archive-inspection.schema.json (the checked-in schema) and a version.json that binds them;
            no eml-inspect folder in any other component.
  python3 -I tools/sfi/sfi_stage.py stamp <dll>
          prints a DLL's kiln_build_stamp() as the check reads it (exit 1 when it has none or it cannot be read).
Exit 0: complete. 1: problems, each on stderr. 2: wrong usage."""
import hashlib
import json
import os
import re
import stat
import sys
import tomllib

MANIFEST = "kiln-runtime.json"
SIDES = ("server", "client")
SFI_DLL = "kiln/plugins/kiln-sfi.dll"
HELPER_DIR = "kiln/tools/eml-inspect"
PLATFORMS = {"linux-x86_64": "kiln-eml-inspect", "windows-x86_64": "kiln-eml-inspect.exe"}
TOOL = "kiln-eml-inspect"
SCHEMA = "archive-inspection.schema.json"
SCHEMA_SRC = "gpl/kiln-eml/kiln-eml/schema/archive-inspection.schema.json"
LICENSE_SRC = "gpl/kiln-eml/LICENSE"
NOTICE_SRC = "gpl/kiln-eml/NOTICE.txt"
CARGO = "gpl/kiln-eml/kiln-eml/Cargo.toml"
INSPECTION_RS = "gpl/kiln-eml/kiln-eml/src/inspection.rs"
VERSION_KEYS = {"format", "tool", "version", "platform", "executable", "sha256", "schemaVersion", "schema",
                "schemaSha256", "framing", "kiln", "git"}
OPTIONAL_KEYS = {"git"}
SCHEMA_VERSION = 1


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 16), b""):
            h.update(block)
    return h.hexdigest()


def helper_version(repo):
    """The version kiln-eml-inspect names in its answers: kiln-eml's own package version (crate::VERSION)."""
    with open(os.path.join(repo, CARGO), "rb") as f:
        v = tomllib.load(f)["package"]["version"]
    if not isinstance(v, str) or not v:
        raise ValueError(f"{CARGO}: no literal package version")
    return v


def framing(repo):
    """The digest framing kiln-eml-inspect answers with (inspection.rs FRAMING)."""
    with open(os.path.join(repo, INSPECTION_RS), encoding="utf-8") as f:
        m = re.search(r'pub const FRAMING: &str = "([^"]+)";', f.read())
    if not m:
        raise ValueError(f"{INSPECTION_RS}: no FRAMING")
    return m.group(1)


def kiln_version(repo):
    with open(os.path.join(repo, "VERSION"), encoding="utf-8") as f:
        return f.read().strip()


def git_commit(repo):
    """The commit of <repo> when it is its own git checkout (informational), else None."""
    try:
        import subprocess
        env = {k: v for k, v in os.environ.items() if not k.startswith("GIT_")}
        p = subprocess.run(["git", "-C", repo, "rev-parse", "HEAD"], env=env, capture_output=True, text=True, timeout=60)
        c = p.stdout.strip()
        return c if p.returncode == 0 and re.fullmatch(r"[0-9a-f]{40}", c) else None
    except (OSError, ValueError):
        return None


def version_doc(repo, folder, platform):
    exe = PLATFORMS[platform]
    doc = {"format": 1, "tool": TOOL, "version": helper_version(repo), "platform": platform, "executable": exe,
           "sha256": sha256(os.path.join(folder, exe)), "schemaVersion": SCHEMA_VERSION, "schema": SCHEMA,
           "schemaSha256": sha256(os.path.join(folder, SCHEMA)), "framing": framing(repo), "kiln": kiln_version(repo)}
    commit = git_commit(repo)
    if commit:
        doc["git"] = commit
    return doc


def write_version(repo, folder, platform):
    if platform not in PLATFORMS:
        raise ValueError(f"unknown platform {platform}")
    doc = version_doc(repo, folder, platform)
    with open(os.path.join(folder, "version.json"), "w", encoding="utf-8", newline="\n") as f:
        json.dump(doc, f, indent=2)
        f.write("\n")


def elf_x86_64(data):
    """An x86-64 ELF executable (ET_EXEC or ET_DYN), little-endian, 64-bit."""
    return (len(data) >= 20 and data[:4] == b"\x7fELF" and data[4] == 2 and data[5] == 1
            and int.from_bytes(data[16:18], "little") in (2, 3) and int.from_bytes(data[18:20], "little") == 0x3E)


def pe_amd64(data, dll):
    """An AMD64 PE32+ image; a DLL when `dll`, else an executable that is no DLL."""
    if len(data) < 0x40 or data[:2] != b"MZ":
        return False
    off = int.from_bytes(data[0x3C:0x40], "little")
    if off + 24 + 2 > len(data) or data[off:off + 4] != b"PE\0\0":
        return False
    machine = int.from_bytes(data[off + 4:off + 6], "little")
    chars = int.from_bytes(data[off + 22:off + 24], "little")
    magic = int.from_bytes(data[off + 24:off + 26], "little")
    return machine == 0x8664 and magic == 0x20B and bool(chars & 0x2000) == dll and bool(chars & 0x0002)


STAMP_EXPORT = b"kiln_build_stamp"
CORE_DLL = "kiln/kiln-core.dll"
SERVICES_DIR = "kiln/services"
STAMP_MAX = 128  # the core copies at most 128 bytes of a stamp (api.cpp dll_build_stamp)
BUILD_METADATA = re.compile(r"\+[0-9A-Za-z.-]+")


def pe_build_stamp(data):
    """The kiln_build_stamp() string of a PE32+ image without running it: None when it has no such export. The export
    must be "lea rax, [rip+X]; ret" (what native/cmake/kiln_build_stamp.cpp and kiln-sfi's Rust both compile to), after
    at most two "jmp rel32" thunks; X a printable NUL-terminated string. Anything else raises ValueError."""
    def u16(o):
        return int.from_bytes(data[o:o + 2], "little")

    def u32(o):
        return int.from_bytes(data[o:o + 4], "little")

    if len(data) < 0x40 or data[:2] != b"MZ":
        raise ValueError("not a PE image")
    pe = u32(0x3C)
    if pe + 24 > len(data) or data[pe:pe + 4] != b"PE\0\0":
        raise ValueError("not a PE image")
    sections, opt_size = u16(pe + 6), u16(pe + 20)
    opt = pe + 24
    if opt + 2 > len(data) or u16(opt) != 0x20B:
        raise ValueError("not a PE32+ image")
    if opt_size < 112 + 8 or u32(opt + 108) < 1:
        return None  # no export directory
    exp_rva, exp_size = u32(opt + 112), u32(opt + 116)
    if not exp_rva:
        return None
    table = []
    for i in range(sections):
        s = opt + opt_size + 40 * i
        if s + 40 > len(data):
            raise ValueError("truncated section table")
        vsize, va, raw_size, raw = u32(s + 8), u32(s + 12), u32(s + 16), u32(s + 20)
        table.append((va, max(vsize, raw_size), raw, raw_size))

    def at(rva, n):
        for va, size, raw, raw_size in table:
            if va <= rva and rva + n <= va + size:
                off = rva - va
                if off + n > raw_size:
                    raise ValueError(f"RVA {rva:#x} is not in the file")
                return raw + off
        raise ValueError(f"RVA {rva:#x} is in no section")

    e = at(exp_rva, 40)
    count, functions, names, ordinals = u32(e + 24), u32(e + 28), u32(e + 32), u32(e + 36)
    rva = None
    for i in range(count):
        n = at(u32(at(names + 4 * i, 4)), len(STAMP_EXPORT) + 1)
        if data[n:n + len(STAMP_EXPORT) + 1] == STAMP_EXPORT + b"\0":
            rva = u32(at(functions + 4 * u16(at(ordinals + 2 * i, 2)), 4))
            break
    if rva is None:
        return None
    if exp_rva <= rva < exp_rva + exp_size:
        raise ValueError("kiln_build_stamp is a forwarder")
    for _ in range(3):
        o = at(rva, 5)
        if data[o] != 0xE9:
            break
        rva = (rva + 5 + int.from_bytes(data[o + 1:o + 5], "little", signed=True)) & 0xFFFFFFFF
    o = at(rva, 8)
    if data[o:o + 3] != b"\x48\x8d\x05" or data[o + 7] != 0xC3:
        raise ValueError(f"kiln_build_stamp is not 'lea rax, [rip+X]; ret' ({data[o:o + 8].hex(' ')})")
    target = (rva + 7 + int.from_bytes(data[o + 3:o + 7], "little", signed=True)) & 0xFFFFFFFF
    s = at(target, 1)
    end = data.find(b"\0", s, s + STAMP_MAX + 1)
    if end < 0:
        raise ValueError(f"kiln_build_stamp's string is longer than {STAMP_MAX} bytes")
    text = data[s:end]
    if not all(0x20 < c < 0x7F for c in text):
        raise ValueError(f"kiln_build_stamp's string is not printable: {text!r}")
    return text.decode("ascii")


def stamp_problems(repo, stage):
    """kiln-sfi.dll's stamp against the core's build (empty: it is the core's, or there is no kiln-sfi.dll)."""
    dll = os.path.join(stage, "sfi", *SFI_DLL.split("/"))
    core = os.path.join(stage, "core", *CORE_DLL.split("/"))
    if not os.path.isfile(dll):
        return []  # check() reports it
    if not os.path.isfile(core):
        return [f"core/{CORE_DLL}: missing, so {SFI_DLL}'s build stamp has no core to match"]
    bad = []
    services = os.path.join(stage, "core", *SERVICES_DIR.split("/"))
    stamps = {}
    for name in sorted(os.listdir(services)) if os.path.isdir(services) else []:
        if not name.lower().endswith(".dll"):
            continue
        try:
            stamps[name] = pe_build_stamp(read(os.path.join(services, name)))
        except (OSError, ValueError) as e:
            bad.append(f"core/{SERVICES_DIR}/{name}: its build stamp cannot be read ({e})")
    for name, s in stamps.items():
        if s is None:
            bad.append(f"core/{SERVICES_DIR}/{name}: exports no kiln_build_stamp (every native service DLL does)")
    found = sorted({s for s in stamps.values() if s is not None})
    if not found:
        return bad + [f"core/{SERVICES_DIR}: no native service DLL with a build stamp, so the core's build is unknown"]
    if len(found) > 1:
        return bad + [f"core/{SERVICES_DIR}: the service DLLs are of different builds ({', '.join(found)})"]
    identity = found[0]
    version = kiln_version(repo)
    meta = identity[len(version):] if identity.startswith(version) else ""
    if not BUILD_METADATA.fullmatch(meta):
        return bad + [f"core/{SERVICES_DIR}: the core's build stamp {identity!r} is not '{version}<build metadata>' "
                      "(a build without build metadata: the core trusts no DLL)"]
    if read(core).count(meta.encode("ascii") + b"\0") < 1:
        bad.append(f"core/{CORE_DLL}: does not hold the build metadata {meta} of its services (not the same build)")
    try:
        sfi = pe_build_stamp(read(dll))
    except (OSError, ValueError) as e:
        return bad + [f"sfi/{SFI_DLL}: its build stamp cannot be read ({e})"]
    if sfi is None:
        bad.append(f"sfi/{SFI_DLL}: exports no kiln_build_stamp, so the core never makes it the ShroudForge-dialect owner "
                   f"(kiln mode would never start); this build's stamp is {identity}")
    elif sfi != identity:
        bad.append(f"sfi/{SFI_DLL}: its build stamp {sfi} is not the core's {identity} (a stale or foreign kiln-sfi.dll: "
                   "the core would not make it the ShroudForge-dialect owner, kiln mode would never start)")
    return bad


def read(path, limit=None):
    with open(path, "rb") as f:
        return f.read() if limit is None else f.read(limit)


def files_of(stage):
    """{component: [component-relative paths]} of a stage folder."""
    out = {}
    for comp in sorted(os.listdir(stage)):
        root = os.path.join(stage, comp)
        if not os.path.isdir(root):
            continue
        rels = []
        for d, dirs, names in os.walk(root):
            dirs.sort()
            for n in sorted(names):
                rels.append(os.path.relpath(os.path.join(d, n), root).replace(os.sep, "/"))
        out[comp] = rels
    return out


def check(repo, stage):
    """Problems of the stage's sfi component and archive inspector (empty: complete)."""
    bad = []
    try:
        with open(os.path.join(stage, MANIFEST), encoding="utf-8") as f:
            components = json.load(f).get("components") or {}
    except (OSError, ValueError) as e:
        return [f"{MANIFEST}: cannot be read ({e})"]
    files = files_of(stage)
    # component sfi
    c = components.get("sfi")
    if c is None:
        bad.append("component sfi is not in the manifest")
    else:
        if c.get("default", True) is not True:
            bad.append("component sfi is not installed by default")
        missing = [s for s in SIDES if s not in (c.get("sides") or [])]
        if missing:
            bad.append(f"component sfi is not for {', '.join(missing)}")
        for r in ("core", "eml"):
            if r not in (c.get("requires") or []):
                bad.append(f"component sfi does not require {r}")
            elif r not in components:
                bad.append(f"component sfi requires {r}, which the manifest does not have")
    dll = os.path.join(stage, "sfi", SFI_DLL)
    if not os.path.isfile(dll):
        bad.append(f"{SFI_DLL}: missing (expected in component sfi)")
    elif os.path.getsize(dll) == 0:
        bad.append(f"{SFI_DLL}: empty")
    elif not pe_amd64(read(dll, 1 << 16), dll=True):
        bad.append(f"{SFI_DLL}: not an AMD64 PE32+ DLL")
    bad.extend(stamp_problems(repo, stage))
    for rel in files.get("sfi", []):
        if rel != SFI_DLL:
            bad.append(f"sfi/{rel}: component sfi holds only {SFI_DLL}")
    for comp, rels in files.items():
        for rel in rels:
            if os.path.basename(rel).lower().startswith("kiln-sfi") and not (comp == "sfi" and rel == SFI_DLL):
                bad.append(f"{comp}/{rel}: kiln-sfi (Kiln's interpreter) outside its component (only sfi/{SFI_DLL})")
            if (rel.lower().startswith(HELPER_DIR + "/") or rel.lower() == HELPER_DIR) and comp != "eml":
                bad.append(f"{comp}/{rel}: kiln-eml's archive inspector belongs to component eml (GPL-3)")
    # the hosted backend stays
    if (os.path.isdir(os.path.join(repo, "native", "plugins", "shroudforge"))
            or os.path.isfile(os.path.join(repo, "third_party", "shroudforge", "kiln", "build.sh"))):
        sf = components.get("shroudforge")
        if sf is None or not files.get("shroudforge"):
            bad.append("component shroudforge (ShroudForge's runtime, the default backend) is missing")
        elif sf.get("default", True) is not True:
            bad.append("component shroudforge (the default backend) is not installed by default")
    # kiln-eml's archive inspector
    if "eml" not in components:
        bad.append("component eml (kiln-eml and its archive inspector) is not in the manifest")
    try:
        want = {"LICENSE.txt": read(os.path.join(repo, LICENSE_SRC)), "NOTICE.txt": read(os.path.join(repo, NOTICE_SRC)),
                SCHEMA: read(os.path.join(repo, SCHEMA_SRC))}
        expected_version, expected_framing, kiln = helper_version(repo), framing(repo), kiln_version(repo)
    except (OSError, ValueError, KeyError, tomllib.TOMLDecodeError) as e:
        return bad + [f"the tree's inspector sources cannot be read ({e})"]
    eml = set(files.get("eml", []))
    for platform, exe in PLATFORMS.items():
        base = f"{HELPER_DIR}/{platform}"
        folder = os.path.join(stage, "eml", *base.split("/"))
        here = sorted(r[len(base) + 1:] for r in eml if r.startswith(base + "/"))
        expect = sorted([exe, "LICENSE.txt", "NOTICE.txt", SCHEMA, "version.json"])
        if here != expect:
            bad.append(f"eml/{base}: holds {here}, expected exactly {expect}")
        path = os.path.join(folder, exe)
        if not os.path.isfile(path) or os.path.islink(path):
            bad.append(f"eml/{base}/{exe}: missing")
            continue
        head = read(path, 1 << 16)
        if platform.startswith("linux-"):
            if not elf_x86_64(head):
                bad.append(f"eml/{base}/{exe}: not an x86-64 ELF executable (Linux starts it through its checked handle)")
            if stat.S_IMODE(os.lstat(path).st_mode) != 0o755:
                bad.append(f"eml/{base}/{exe}: mode {oct(stat.S_IMODE(os.lstat(path).st_mode))}, expected 0o755")
        elif not pe_amd64(head, dll=False):
            bad.append(f"eml/{base}/{exe}: not an AMD64 PE32+ executable")
        for name, data in want.items():
            p = os.path.join(folder, name)
            if not os.path.isfile(p) or read(p) != data:
                bad.append(f"eml/{base}/{name}: missing or not the tree's {name}")
        try:
            with open(os.path.join(folder, "version.json"), encoding="utf-8") as f:
                v = json.load(f)
        except (OSError, ValueError) as e:
            bad.append(f"eml/{base}/version.json: cannot be read ({e})")
            continue
        if not isinstance(v, dict) or set(v) - VERSION_KEYS or VERSION_KEYS - OPTIONAL_KEYS - set(v):
            bad.append(f"eml/{base}/version.json: not the closed version file")
            continue
        expect_v = {"format": 1, "tool": TOOL, "version": expected_version, "platform": platform, "executable": exe,
                    "sha256": sha256(path), "schemaVersion": SCHEMA_VERSION, "schema": SCHEMA,
                    "schemaSha256": hashlib.sha256(want[SCHEMA]).hexdigest(), "framing": expected_framing, "kiln": kiln}
        for k, val in expect_v.items():
            if v.get(k) != val:
                bad.append(f"eml/{base}/version.json: {k} is {v.get(k)!r}, expected {val!r}")
        if "git" in v and not (isinstance(v["git"], str) and re.fullmatch(r"[0-9a-f]{40}", v["git"])):
            bad.append(f"eml/{base}/version.json: git is not a commit")
    return bad


def main(argv):
    if len(argv) == 5 and argv[1] == "version":
        try:
            write_version(argv[2], argv[3], argv[4])
        except (OSError, ValueError, KeyError, tomllib.TOMLDecodeError) as e:
            print(f"sfi_stage: version.json: {e}", file=sys.stderr)
            return 1
        return 0
    if len(argv) == 4 and argv[1] == "check":
        bad = check(argv[2], argv[3])
        for p in bad:
            print(f"sfi_stage: {p}", file=sys.stderr)
        if bad:
            print(f"sfi_stage: FAIL: {len(bad)} problem(s) in {argv[3]}", file=sys.stderr)
            return 1
        print(f"sfi_stage: OK: component sfi ({SFI_DLL}, the core's build stamp) and kiln-eml's archive inspector "
              f"for {', '.join(PLATFORMS)} in {argv[3]}")
        return 0
    if len(argv) == 3 and argv[1] == "stamp":
        try:
            stamp = pe_build_stamp(read(argv[2]))
        except (OSError, ValueError) as e:
            print(f"sfi_stage: {argv[2]}: {e}", file=sys.stderr)
            return 1
        if stamp is None:
            print(f"sfi_stage: {argv[2]}: no kiln_build_stamp export", file=sys.stderr)
            return 1
        print(stamp)
        return 0
    print("usage: sfi_stage.py version <repo> <platform folder> <platform> | sfi_stage.py check <repo> <stage dir> | "
          "sfi_stage.py stamp <dll>", file=sys.stderr)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))
