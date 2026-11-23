#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""The "Since mod API 0.2.x (0.2.0-dev.N)" of every type and member in the API reference.

  tools/docs-site/since.py <repository> <api folder>      match statistics, and the entries that matched nothing (for the maintainer)

Reads managed/api-history.tsv (the ledger: the first release that shipped each PublicAPI line and its mod API,
tools/release/lib/apihistory.py) and the lines still in managed/**/PublicAPI.Unshipped.txt (they get their row when
the release promotes them), and matches both to the members DocFX shows through their documentation ids (apiids.py).
A member that several lines describe (a property's get and set, a type moved out of one assembly into another and forwarded)
gets the earliest of their releases. annotate.py writes the line into the XML docs; the text is made here.

  Since mod API 0.2.12 (0.2.0-dev.30)
  Since mod API 0.2.1 or earlier (0.2.0-dev.26 or earlier)      the ledger starts at 0.2.0-dev.26: what stood there is "<="
  Since mod API 0.2.15 (next release)                           in PublicAPI.Unshipped.txt, not released yet
  Since mod API 0.2.15 (0.2.0-dev.31)                           the same, when the build is told which release it is part of

The release a build belongs to is KILN_DOCS_RELEASE, else KILN_SDK_VERSION (release.sh sets it) when that names a release
(0.2.0-dev.31, not a local pack's 0.2.0-dev.local.*). release.sh builds the site before it promotes the record, so a release's
own new members are still unshipped then.
Standard library only.
"""
import importlib.util
import os
from pathlib import Path
import re
import sys

HERE = Path(__file__).resolve().parent


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


ids = load("apiids", HERE / "apiids.py")
history = load("apihistory", HERE.parent / "release/lib/apihistory.py")      # release_key, the ledger's own ordering

LEDGER = "managed/api-history.tsv"
MODS_CS = "managed/Kiln.Api/Mods.cs"
API_VERSION = re.compile(r'ApiVersion\s*=\s*"([^"]+)"')
RELEASE_NAME = re.compile(r"\d+\.\d+\.\d+(?:-dev\.\d+(?:\.\d+)*)?")


class Entry:
    """When a member first shipped: mod_api and release, as the ledger writes them ("<=0.2.1" is "0.2.1 or earlier");
    release None means in PublicAPI.Unshipped.txt (or, with a name given, in the release being built)."""

    def __init__(self, mod_api, release, upcoming=False):
        self.mod_api, self.release, self.upcoming = mod_api, release, upcoming

    def order(self):
        if self.upcoming:
            return (((9999, 0, 0), 1, ()), 1)
        at_most = self.release.startswith("<=")
        return (history.release_key(history.bare(self.release)), 0 if at_most else 1)

    def text(self, building=None):
        if self.upcoming:
            return f"Since mod API {self.mod_api} ({building or 'next release'})"
        if self.release.startswith("<="):
            return f"Since mod API {history.bare(self.mod_api)} or earlier ({history.bare(self.release)} or earlier)"
        return f"Since mod API {self.mod_api} ({self.release})"


def building_release():
    """The release this build is part of, or None."""
    for name in ("KILN_DOCS_RELEASE", "KILN_SDK_VERSION"):
        v = os.environ.get(name, "").strip()
        if v and RELEASE_NAME.fullmatch(v.lstrip("v")):
            return v.lstrip("v")
    return None


def api_version(repo):
    m = API_VERSION.search((Path(repo) / MODS_CS).read_text(encoding="utf-8"))
    return m.group(1) if m else None


def record_lines(text):
    return [l.rstrip("\r") for l in text.split("\n") if l.strip() and not l.startswith(("#nullable", "*REMOVED*"))]


class Since:
    """key (apiids) -> Entry, for the ledger of a repository and its unshipped lines."""

    def __init__(self, repo, release=None):
        repo = Path(repo)
        self.entries, self.unreadable = {}, 0
        self.release = release if release is not None else building_release()
        ledger = repo / LEDGER
        if not ledger.is_file():
            raise FileNotFoundError(f"no API history ledger: {ledger} (release.sh promote writes it; docs/kiln/launcher/releasing.md)")
        rows = ledger.read_text(encoding="utf-8").split("\n")
        if rows[0].split("\t") != list(history.COLUMNS):
            raise ValueError(f"{ledger}: unexpected header {rows[0]!r}")
        for row in rows[1:]:
            f = row.split("\t")
            if len(f) == 5 and f[0] == "managed":
                self.add(f[2], Entry(f[4], f[3]))
        upcoming = Entry(api_version(repo), None, True)
        for path in sorted((repo / "managed").rglob("PublicAPI.Unshipped.txt")):
            if {"bin", "obj"} & set(path.parts):
                continue
            for line in record_lines(path.read_text(encoding="utf-8-sig")):
                self.add(line, upcoming)

    def add(self, line, entry):
        try:
            key = ids.key_of_line(line)
        except Exception:                                      # a line the reader does not know: no line for it
            key = None
            self.unreadable += 1
        if key is None:
            return
        old = self.entries.get(key)
        if old is None or entry.order() < old.order():
            self.entries[key] = entry

    def lookup(self, comment_id):
        try:
            key = ids.key_of_doc_id(comment_id)
        except Exception:
            return None
        return self.entries.get(key) if key else None

    def text(self, comment_id):
        e = self.lookup(comment_id)
        return e.text(self.release) if e else None


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__.split("\n\n")[1])
    undocumented = load("undocumented", HERE / "undocumented.py")
    since = Since(sys.argv[1])
    items = [i for f in sorted(Path(sys.argv[2]).glob("*.yml")) if f.name != "toc.yml" for i in undocumented.items(f)]
    items = [i for i in items if i.get("commentId") and i.get("type") != "Namespace"]
    miss = [i["commentId"] for i in items if not since.lookup(i["commentId"])]
    print(f"{len(items) - len(miss)} of {len(items)} items have a since line; {len(since.entries)} keys, "
          f"{since.unreadable} unreadable lines")
    for m in miss[:200]:
        print("  no entry:", m)


if __name__ == "__main__":
    main()
