#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Must-port names: sigdb names a game update may not take away quietly (tools/sigport/must_port.json).

A listed name is one whose loss changes what Kiln does by itself, with a fallback players see (kiln.build's three
save-region callers are the first). Where the re-port and check flow meets one (docs/kiln/sigport.md):

  port.py                 a listed name the input definitions declare must resolve on the new exe at --min-confidence
                          or better; otherwise the port is incomplete (exit 1), report.json has the reason under
                          summary.must_port, report.txt and stderr a "must-port" line (stderr also with --quiet)
  build_record.py check   sigdb must still declare every listed name of the side, and the side's newest build record
                          must resolve it (any level but unresolved); otherwise a "must-port" line and exit 1
  must_port.py check      the same check, for the listed names alone
  sigdb_check.py          a listed name the definitions declare and the exe does not resolve gets a "must-port" line;
                          it fails the check (exit 1)

    python3 -I tools/sigport/must_port.py check [server] [client]

Exit status of check: 0 every listed name is declared and resolved, 1 one is not, 2 the list itself is malformed.
"""
import argparse
import json
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
LIST = HERE / "must_port.json"
SIDES = ("server", "client")
SECTIONS = ("symbols", "constants", "features")


class ListError(ValueError):
    """The must-port list cannot be read or an entry is malformed."""


def load(path=LIST):
    """[{"name", "section", "sides", "why"}] in the list's order; ListError when the list is malformed."""
    path = Path(path)
    try:
        doc = json.loads(path.read_text())
    except (OSError, json.JSONDecodeError) as e:
        raise ListError(f"{path}: {e}") from e
    if not isinstance(doc, dict) or doc.get("format") != 1 or not isinstance(doc.get("names"), list):
        raise ListError(f'{path}: expected {{"format": 1, "names": [...]}}')
    out, seen = [], set()
    for i, e in enumerate(doc["names"]):
        where = f"{path}: names[{i}]"
        if not isinstance(e, dict) or not isinstance(e.get("name"), str) or not e["name"].strip():
            raise ListError(f"{where}: no name")
        name, section, sides = e["name"], e.get("section", "symbols"), e.get("sides", list(SIDES))
        if section not in SECTIONS:
            raise ListError(f"{where} ({name}): section {section!r} is not one of {', '.join(SECTIONS)}")
        if not isinstance(sides, list) or not sides or any(s not in SIDES for s in sides):
            raise ListError(f"{where} ({name}): sides {sides!r} is not a list of {', '.join(SIDES)}")
        if not isinstance(e.get("why"), str) or not e["why"].strip():
            raise ListError(f"{where} ({name}): no why (what Kiln falls back to without it)")
        if (section, name) in seen:
            raise ListError(f"{where}: {section[:-1]} {name} is listed twice")
        seen.add((section, name))
        out.append({"name": name, "section": section, "sides": list(sides), "why": e["why"].strip()})
    return out


def for_side(entries, side):
    return [e for e in entries if side in e["sides"]]


def label(e):
    return f"{e['section'][:-1]} {e['name']}"


def record_failures(entries, side, declarer, rec, tag):
    """Lines for the listed names of `side` that sigdb no longer declares, or that `rec` (the side's newest build
    record, named `tag` in the lines) does not resolve. declarer: build_record.declared(side)."""
    bad = []
    for e in for_side(entries, side):
        if e["name"] not in declarer[e["section"]]:
            bad.append(f"must-port {label(e)}: sigdb no longer declares it for the {side}. {e['why']}")
            continue
        level, reason = None, ""
        for s in rec.get("services", {}).values():
            if e["name"] in s.get(e["section"], {}):
                level, reason = s[e["section"]][e["name"]], s.get("unresolved", {}).get(e["name"], "")
                break
        if level is None:
            bad.append(f"must-port {label(e)}: not listed in {tag}. {e['why']}")
        elif level == "unresolved":
            bad.append(f"must-port {label(e)}: does not resolve on {tag} ({reason}). {e['why']}")
    return bad


def port_check(entries, side, rsyms, rconsts, feats, min_conf):
    """port.py's summary.must_port: {"checked", "not_declared", "failed"}. A listed name the input definitions declare
    fails when it does not resolve on the new exe, or only below min_conf; a feature fails when it is off there. Names
    the input definitions do not declare (an older Kiln's) are listed under not_declared and fail nothing here:
    build_record.py check is where sigdb losing one fails."""
    checked, absent, failed = [], [], []
    for e in for_side(entries, side):
        name = e["name"]
        if e["section"] == "features":
            if name not in feats:
                absent.append(name)
                continue
            checked.append(name)
            if not feats[name]:
                failed.append(f"must-port {label(e)}: off on the new exe. {e['why']}")
            continue
        table = rsyms if e["section"] == "symbols" else rconsts
        if name not in table:
            absent.append(name)
            continue
        checked.append(name)
        r = table[name]
        if not r.get("ok_new"):
            failed.append(f"must-port {label(e)}: does not resolve on the new exe ({r.get('error') or 'no match'}). "
                          f"{e['why']}")
        elif r.get("confidence", 0.0) < min_conf:
            failed.append(f"must-port {label(e)}: ported only at confidence {r.get('confidence', 0.0):.2f}, below "
                          f"{min_conf}. {e['why']}")
    return {"checked": checked, "not_declared": absent, "failed": failed}


def check(sides=SIDES, symbols_dir=None, builds_dir=None, list_path=LIST):
    """-> (ok, lines): every listed name of each side is declared by sigdb and resolved by the side's newest record."""
    import build_record as br  # noqa: E402 (build_record imports this module)
    symbols_dir = Path(symbols_dir) if symbols_dir else br.SYMBOLS
    builds_dir = Path(builds_dir) if builds_dir else br.BUILDS
    entries = load(list_path)
    ok, lines = True, []
    for side in sides:
        files = sorted((builds_dir / side).glob("*.json"))
        if not files:
            lines.append(f"FAIL {side}: no build record in {builds_dir / side}")
            ok = False
            continue
        top_path, top = br.newest([(p, json.loads(p.read_text())) for p in files])
        tag = f"{side} {top_path.stem[:8]}… (steam build {top.get('steam_build', '?')})"
        bad = record_failures(entries, side, br.declared(side, symbols_dir), top, tag)
        n = len(for_side(entries, side))
        if bad:
            ok = False
            lines.append(f"FAIL {tag}: {len(bad)} of {n} must-port name(s) dropped or unresolved")
            lines += [f"  {b}" for b in bad]
        else:
            lines.append(f"ok   {tag}: {n} must-port name(s) declared and resolved")
    return ok, lines


def main(argv=None):
    ap = argparse.ArgumentParser(prog="must_port.py", description=__doc__.split("\n")[0])
    sub = ap.add_subparsers(dest="command", required=True)
    c = sub.add_parser("check", help="every listed name is declared by sigdb and resolved by the newest build records")
    c.add_argument("sides", nargs="*", choices=list(SIDES), default=None)
    c.add_argument("--list", default=str(LIST), help="the must-port list (default: tools/sigport/must_port.json)")
    c.add_argument("--symbols", default=None, help=argparse.SUPPRESS)
    c.add_argument("--builds", default=None, help=argparse.SUPPRESS)
    a = ap.parse_args(argv)
    try:
        ok, lines = check(a.sides or SIDES, a.symbols, a.builds, a.list)
    except ListError as e:
        print(f"must_port: {e}", file=sys.stderr)
        return 2
    print("\n".join(lines))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
