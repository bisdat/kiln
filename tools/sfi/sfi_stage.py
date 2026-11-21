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
          - component eml: kiln/tools/eml-inspect/<platform>/ for linux-x86_64 (an x86-64 ELF executable, mode 0755) and
            windows-x86_64 (an AMD64 PE32+ executable, not a DLL), each with exactly its LICENSE.txt and NOTICE.txt
            (gpl/kiln-eml's), archive-inspection.schema.json (the checked-in schema) and a version.json that binds them;
            no eml-inspect folder in any other component.
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
        print(f"sfi_stage: OK: component sfi ({SFI_DLL}) and kiln-eml's archive inspector for "
              f"{', '.join(PLATFORMS)} in {argv[3]}")
        return 0
    print("usage: sfi_stage.py version <repo> <platform folder> <platform> | sfi_stage.py check <repo> <stage dir>",
          file=sys.stderr)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))
