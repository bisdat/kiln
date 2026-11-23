#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Positional records need a documented property for each of their parameters.

  python3 -I tools/docs-site/record_params.py [--xml DIR] [--tree DIR]

`public record Pair(int Left, int Right)` has two properties the compiler writes from the parameters. CS1591 never asks for
their docs, and the compiler gives such a property its summary only from a `<param name="Left">` on the record. A record
whose parameters have no `<param>` ships properties with an empty reference page. This check finds them without DocFX,
from the two things tools/check.sh has built anyway:
  the PublicAPI record: a type with a constructor and a Deconstruct whose parameter names agree is positional, and the
      parameters that are also properties of it (the same name) are the ones checked; experimental ([KILNX...]) types are left
      out, as the API index leaves them out
  the XML doc files of a Release build (--xml DIR, found as in api_index.py: DIR/<Assembly>.xml, DIR/<Assembly>/release/,
      DIR/bin/<Assembly>/release/; $KILN_API_XML sets it too)
A property is documented when the XML has a non-empty summary (or an <inheritdoc cref="..."/>) for it, or a non-empty `<param>` of its
name on the record or on its constructor. The docfx-based gate (undocumented.py, in tools/docs-site/build.sh) counts these
properties too, on every release.
Exit codes: 0 every positional property is documented, 1 some are not (one line each; the fix is a `<param name="...">`
on the record), 2 bad input (no XML for an assembly that has a positional record, no PublicAPI record).
Tests: python3 -I tools/docs-site/test_record_params.py
"""
import argparse
import collections
import importlib.util
import os
import re
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent


def load_api_index():
    spec = importlib.util.spec_from_file_location("api_index", HERE / "api_index.py")
    mod = importlib.util.module_from_spec(spec)
    sys.modules.setdefault("api_index", mod)
    spec.loader.exec_module(mod)
    return mod


ai = load_api_index()


def param_names(plist):
    """('Left', 'Right') of 'int Left, out int Right = 3'."""
    out = []
    for p in ai.split_top(plist or ""):
        p = re.sub(r"\s*=\s*.*$", "", p).strip()
        m = re.match(r"^.*\s@?(\w+)$", p)
        if m:
            out.append(m.group(1))
    return tuple(out)


def positional(entries):
    """{type: [property names]} of the positional records among the entries of one assembly (see the module doc)."""
    ctors, decons, props, experimental = collections.defaultdict(set), {}, collections.defaultdict(set), set()
    for e in entries:
        if e.kind == "type" and e.exp:
            experimental.add(e.type)
        if e.kind in ("marker", "forward", "type") or e.exp or e.ext is not None:
            continue
        if e.kind == "ctor" and e.plist:
            ctors[e.type].add(param_names(e.plist))
        elif e.kind == "method" and e.name == "Deconstruct" and e.plist:
            decons[e.type] = param_names(e.plist)
        elif e.kind == "prop" and e.accessor in ("get", "set", "init"):
            props[e.type].add(e.name)
    out = {}
    for t, names in decons.items():
        if t in experimental or names not in ctors.get(t, ()):
            continue
        have = [n for n in names if n in props.get(t, ())]
        if have:
            out[t] = have
    return out


def documented(docs, type_key, name):
    d = docs.by_id.get(f"P:{type_key}.{name}")
    if d is not None and (d.summary.strip() or d.inherit):
        return True
    return bool(docs.param_doc(type_key, name).strip())


def check(root, xml_dir):
    """-> ([lines of missing docs], number of records, number of properties)."""
    tree = ai.Tree(root)
    missing, records, props = [], 0, 0
    for asm, folder in tree.projects():
        entries = [ai.parse_api_line(line) for line, _ in tree.record(folder)]
        found = positional(entries)
        if not found:
            continue
        path = ai.find_xml(xml_dir, asm)
        if path is None:
            raise ai.Problem(f"no XML docs for {asm} in {xml_dir}: it has {len(found)} positional records "
                             f"(build it Release with GenerateDocumentationFile)")
        docs = ai.Docs(path)
        for t in sorted(found):
            records += 1
            for name in found[t]:
                props += 1
                if not documented(docs, t, name):
                    missing.append(f"{asm}: {t}.{name}")
    return missing, records, props


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--tree", default=str(HERE.parent.parent), help="the repository (default: this script's)")
    ap.add_argument("--xml", default=os.environ.get("KILN_API_XML"), help="the compiled XML docs (see the module doc)")
    a = ap.parse_args(argv)
    if not a.xml:
        print("record-params: give --xml DIR (or set KILN_API_XML): the XML docs of a Release build", file=sys.stderr)
        return 2
    try:
        missing, records, props = check(Path(a.tree).resolve(), a.xml)
    except (ai.Problem, OSError) as e:
        print(f"record-params: {e}", file=sys.stderr)
        return 2
    if missing:
        print(f"record-params: {len(missing)} of {props} positional-record properties have no <param> and no summary:")
        for m in missing[:200]:
            print(f"  {m}")
        if len(missing) > 200:
            print(f"  ... and {len(missing) - 200} more")
        print('record-params: add a <param name="Property">...</param> to the record\'s doc comment for each')
        return 1
    print(f"record-params: {props} properties of {records} positional records, all documented")
    return 0


if __name__ == "__main__":
    sys.exit(main())
