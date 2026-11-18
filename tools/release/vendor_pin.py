#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Pins the vendor files of the launcher's manifest (launcher/src/data/vendor-components.json): fills in each file's
SHA-256 from a copy the release process downloaded once, by hand, from the URL the manifest names.

  python3 -I tools/release/vendor_pin.py --dir <folder> [--component amd-fsr ...] [--manifest <file>] [--write]
  python3 -I tools/release/vendor_pin.py --check --dir <folder>

<folder> holds the downloaded files by their manifest names, either side by side or one folder per component
(<folder>/<component id>/<name>). For every file of the chosen components (all of them by default) the tool checks
that the copy is there, has the manifest's size and, when the manifest has a gitBlob, the git blob SHA-1 GitHub lists
for the file at the tag (so the copy is the vendor's file at that tag, not whatever came down the wire). Only then does
it compute the SHA-256. Without --write it prints what it would write; with --write it writes the manifest (the same
layout: two-space JSON, a newline at the end). A file pinned already is compared, never replaced: a different SHA-256
is an error (the vendor changed the file under the same tag). --check writes nothing and fails unless every chosen file
is pinned and its copy matches.

This tool never downloads anything. Exit 0: done (or nothing to do); 1: a copy is missing or does not match; 2: usage.
"""
import argparse
import hashlib
import json
from pathlib import Path
import sys

HERE = Path(__file__).resolve().parent
REPO = HERE.parent.parent
MANIFEST = REPO / "launcher" / "src" / "data" / "vendor-components.json"
NOT_PINNED = "not-pinned"


def sha256_of(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def git_blob_of(path, size):
    """The SHA-1 git gives the file's content as a blob ("blob <size>\\0" + content)."""
    h = hashlib.sha1()
    h.update(b"blob %d\0" % size)
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def find_copy(folder, component, name):
    for p in (folder / component / name, folder / name):
        if p.is_file() and not p.is_symlink():
            return p
    return None


def pin(manifest, folder, components=None, check=False, out=print):
    """Pins (or checks) the files of `components` (None: all) in the parsed `manifest` from copies in `folder`.
    Returns (changed, problems): how many sha256 values were filled in, and one line per problem."""
    known = [c["id"] for c in manifest.get("components", [])]
    for want in components or []:
        if want not in known:
            return 0, [f"no component {want} in the manifest (it has {', '.join(known) or 'none'})"]
    changed = 0
    problems = []
    for c in manifest.get("components", []):
        if components and c["id"] not in components:
            continue
        for f in c["files"]:
            name, size = f["name"], f["size"]
            label = f"{c['id']}/{name}"
            copy = find_copy(folder, c["id"], name)
            if copy is None:
                problems.append(f"{label}: no copy in {folder} (download {f['url']} there first)")
                continue
            got = copy.stat().st_size
            if got != size:
                problems.append(f"{label}: the copy has {got} bytes, the manifest {size}; not the vendor's file")
                continue
            blob = f.get("gitBlob")
            if blob and git_blob_of(copy, got) != blob:
                problems.append(f"{label}: the copy's git blob SHA-1 is not {blob} (GitHub's for this file at the tag)")
                continue
            sha = sha256_of(copy)
            pinned = f.get("sha256", NOT_PINNED)
            if pinned == NOT_PINNED:
                if check:
                    problems.append(f"{label}: not pinned")
                    continue
                f["sha256"] = sha
                changed += 1
                out(f"{label}: sha256 {sha} ({size} bytes)")
            elif pinned != sha:
                problems.append(f"{label}: pinned {pinned}, the copy has {sha}: the file changed under the same tag")
            else:
                out(f"{label}: pinned, the copy matches")
    return changed, problems


def dump(manifest):
    return json.dumps(manifest, indent=2, ensure_ascii=False) + "\n"


def main(argv=None):
    ap = argparse.ArgumentParser(description="Pin the vendor files of the launcher's manifest from downloaded copies.")
    ap.add_argument("--dir", required=True, type=Path, help="the folder with the downloaded files")
    ap.add_argument("--manifest", type=Path, default=MANIFEST)
    ap.add_argument("--component", action="append", help="only this component (repeatable)")
    ap.add_argument("--write", action="store_true", help="write the manifest")
    ap.add_argument("--check", action="store_true", help="write nothing; fail unless every file is pinned and matches")
    a = ap.parse_args(argv)
    if a.write and a.check:
        print("--write and --check do not go together", file=sys.stderr)
        return 2
    if not a.dir.is_dir():
        print(f"{a.dir} is not a folder", file=sys.stderr)
        return 2
    text = a.manifest.read_text(encoding="utf-8")
    manifest = json.loads(text)
    if manifest.get("format") != 1:
        print(f"{a.manifest}: format {manifest.get('format')} is not 1", file=sys.stderr)
        return 2
    changed, problems = pin(manifest, a.dir, a.component, check=a.check)
    for p in problems:
        print(f"PROBLEM {p}", file=sys.stderr)
    if problems:
        print(f"{len(problems)} problem(s); the manifest was not written", file=sys.stderr)
        return 1
    if a.check:
        print("every file is pinned and matches its copy")
        return 0
    if not changed:
        print("nothing to pin")
        return 0
    if a.write:
        a.manifest.write_text(dump(manifest), encoding="utf-8")
        print(f"pinned {changed} file(s) in {a.manifest}")
    else:
        print(f"would pin {changed} file(s); run again with --write to write {a.manifest}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
