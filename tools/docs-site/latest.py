#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Decides whether a release becomes the "latest" API reference (publish.sh --sdk-site, the /kiln/api/ alias).

  tools/docs-site/latest.py newer <release> [<current>]

<release> is the release being published (0.2.0-dev.31); <current> is what the alias names now: the release (`0.2.0-dev.30.5`)
or the link's target as `readlink` gives it (`downloads/0.2.0-dev.30.5/sdk/api`), empty when there is no alias yet. Exit 0: the
release is the newest one (or the same one again, or nothing named yet): move the alias. Exit 1: <current> is newer (a hotfix
of an older line is being published): leave it. Exit 2: <release> is not a release name. Release order is the one of the API
history ledger (tools/release/lib/apihistory.py release_key): 0.2.0-dev.9 < 0.2.0-dev.30 < 0.2.0-dev.30.5 < 0.2.0 < 0.2.1.
A <current> that names no release (a hand-made alias) is replaced, with a note.
"""
import importlib.util
from pathlib import Path
import re
import sys

HERE = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location("apihistory", HERE.parent / "release/lib/apihistory.py")
history = importlib.util.module_from_spec(spec)
spec.loader.exec_module(history)
TARGET = re.compile(r"^(?:\./)?(?:downloads/)?([^/]+)(?:/sdk/api)?/?$")


def current_release(text):
    """The release a link target or name stands for, or None."""
    m = TARGET.match((text or "").strip())
    if not m:
        return None
    try:
        history.release_key(m.group(1))
    except history.HistoryError:
        return None
    return m.group(1)


def newer(release, current):
    """(move the alias?, the reason)."""
    key = history.release_key(release)
    if not (current or "").strip():
        return True, "no latest API reference yet"
    name = current_release(current)
    if name is None:
        return True, f"the alias names {current.strip()!r}, which is no release"
    if history.release_key(name) > key:
        return False, f"/kiln/api/ is {name}, newer than {release}"
    return True, f"/kiln/api/ was {name}"


def main():
    if len(sys.argv) not in (3, 4) or sys.argv[1] != "newer":
        sys.exit(__doc__.split("\n\n")[1])
    try:
        move, why = newer(sys.argv[2], sys.argv[3] if len(sys.argv) == 4 else "")
    except history.HistoryError as e:
        print(e, file=sys.stderr)
        sys.exit(2)
    print(why)
    sys.exit(0 if move else 1)


if __name__ == "__main__":
    main()
