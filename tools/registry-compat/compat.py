#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""registry-compat: the registry stage's compatibility harness (registry P1 S12), the offline half.

mods/tests/registry-compat-test writes a dump at every content ready of a lab session (the served bytes of every resource of
the registries' types as SHA-256, kiln.content's changed fields and conflicts, registry.json). This tool:

    compat.py archive --exe <the lab's server exe> --dump <dump.json> --out <archive.json>
        the same resources' bytes in the game's own archive (read as data with tools/bootsnap; the exe is never run): what a
        Kiln-off boot reads.
    compat.py check --dump <dump.json> --archive <archive.json> [--allow <guid> ...]
        every resource the dump has is byte-identical to the archive's, except the allowed ones (what the registry's boot
        back-end and old boot patches write) and the dump's "touched" ones (resources whose loaded bytes the store changed:
        old packs and the registry's live fields). Exit 1 on any other difference.
    compat.py diff --before <dump.json> --after <dump.json> [--allow <guid> ...] [--allow-field <registry>:<entry>.<field> ...]
        two sessions' dumps (an old-only set against the same set with registry content, or two points of one session):
        resources and store fields equal except the allowed ones. Exit 1 on any other difference.

Dumps hold hashes and field values of the game's data: keep them in the output folder, never in the repository.
"""
import argparse
import hashlib
import json
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent


def key_of(r):
    return f"{r['guid'].lower()}:{int(r['typeHash']):08x}:{int(r.get('part', 0))}"


def load(path):
    return json.loads(Path(path).read_text(encoding="utf-8"))


def cmd_archive(a):
    sys.path.insert(0, str(HERE.parent / "bootsnap"))
    sys.path.insert(0, str(HERE.parent / "kfc"))
    import bootsnap  # noqa: E402
    from kfc.archive import ResourceKey  # noqa: E402
    src = bootsnap.open_source(Path(a.exe))
    out = {"format": 1, "exe": Path(a.exe).name, "resources": {}}
    try:
        for r in load(a.dump)["resources"]:
            k = ResourceKey(r["guid"], int(r["typeHash"]), int(r.get("part", 0)))
            if not src.kfc.has(k):
                out["resources"][key_of(r)] = None
                continue
            b = src.kfc.read(k)
            out["resources"][key_of(r)] = {"size": len(b), "sha256": hashlib.sha256(b).hexdigest()}
    finally:
        src.close()
    Path(a.out).write_text(json.dumps(out), encoding="utf-8")
    print(f"archive: {len(out['resources'])} resource(s) -> {a.out}")
    return 0


def check(dump, archive, allow):
    """(same, allowed differences, other differences, not in the archive)"""
    same, allowed, other, new = 0, [], [], []
    allow = {g.lower() for g in allow} | {g.lower() for g in dump.get("touched", [])}
    for r in dump["resources"]:
        k = key_of(r)
        a = archive["resources"].get(k)
        if a is None:
            new.append(k)
            continue
        if a["sha256"] == r["sha256"] and a["size"] == r["size"]:
            same += 1
        elif r["guid"].lower() in allow:
            allowed.append(k)
        else:
            other.append(k)
    return same, allowed, other, new


def cmd_check(a):
    same, allowed, other, new = check(load(a.dump), load(a.archive), a.allow or [])
    print(f"check: {same} identical to the archive, {len(allowed)} allowed difference(s) {allowed}, {len(other)} other difference(s), "
          f"{len(new)} not in the archive")
    for k in other[:50]:
        print(f"  differs: {k}")
    return 1 if other else 0


def fields(dump):
    out = {}
    for c in dump.get("store", {}).get("changed", []):
        out[f"{c['registry']}:{c['entry']}.{c['field']}"] = json.dumps(c.get("value"), sort_keys=True)
    return out


def diff(before, after, allow, allow_fields):
    """(resource differences, field differences) not allowed"""
    allow = {g.lower() for g in allow} | {g.lower() for g in before.get("touched", []) + after.get("touched", [])}
    ra = {key_of(r): r["sha256"] for r in before["resources"]}
    rb = {key_of(r): r["sha256"] for r in after["resources"]}
    res = sorted(k for k in set(ra) | set(rb) if ra.get(k) != rb.get(k) and k.split(":")[0] not in allow)
    fa, fb = fields(before), fields(after)
    fl = sorted(k for k in set(fa) | set(fb) if fa.get(k) != fb.get(k) and k not in set(allow_fields))
    return res, fl


def cmd_diff(a):
    res, fl = diff(load(a.before), load(a.after), a.allow or [], a.allow_field or [])
    print(f"diff: {len(res)} resource difference(s), {len(fl)} store field difference(s) outside the allowed ones")
    for k in res[:50]:
        print(f"  resource: {k}")
    for k in fl[:50]:
        print(f"  field: {k}")
    return 1 if res or fl else 0


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("archive")
    p.add_argument("--exe", required=True)
    p.add_argument("--dump", required=True)
    p.add_argument("--out", required=True)
    p = sub.add_parser("check")
    p.add_argument("--dump", required=True)
    p.add_argument("--archive", required=True)
    p.add_argument("--allow", action="append")
    p = sub.add_parser("diff")
    p.add_argument("--before", required=True)
    p.add_argument("--after", required=True)
    p.add_argument("--allow", action="append")
    p.add_argument("--allow-field", action="append")
    a = ap.parse_args(argv)
    return {"archive": cmd_archive, "check": cmd_check, "diff": cmd_diff}[a.cmd](a)


if __name__ == "__main__":
    sys.exit(main())
