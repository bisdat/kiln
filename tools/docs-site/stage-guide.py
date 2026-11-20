#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Stage only tracked guide pages; link unshipped targets to the public repository.

  python3 -I tools/docs-site/stage-guide.py <repository> <docs-work>

The site's modding/ layout matches the repository. Other pages, source files and
folders stay in the repository rather than becoming broken relative site links.
Private or missing targets fail staging, never become public repository URLs.
"""
import importlib.util
import os
from pathlib import Path
import re
import subprocess
import sys
from urllib.parse import quote, unquote

spec = importlib.util.spec_from_file_location("check_links", Path(__file__).with_name("check-links.py"))
links = importlib.util.module_from_spec(spec)
spec.loader.exec_module(links)

REPOSITORY = "https://github.com/frostdev-ops/kiln"
INLINE = re.compile(r"(\]\(\s*)(<[^>]+>|[^\s)]+)")
REFERENCE = re.compile(r"(^ {0,3}\[[^\]]+\]:\s*)(<[^>]+>|\S+)")


def tracked_files(repo):
    """The repository's files: KILN_DOCS_TRACKED names a NUL-separated list of paths relative to the repository (a
    release's export has no .git: release.sh lists what it exported), else git's tracked files of this checkout.
    A folder that is not the top of a git checkout (an export, or a folder git ignores inside another repository,
    where ls-files lists nothing) without the list stops staging."""
    listed = os.environ.get("KILN_DOCS_TRACKED")
    if listed:
        names = Path(listed).read_bytes().decode("utf-8").split("\0")
    else:
        top = subprocess.run(["git", "-C", str(repo), "rev-parse", "--show-toplevel"], capture_output=True, text=True)
        if top.returncode or Path(top.stdout.strip()).resolve() != repo:
            raise SystemExit(f"stage-guide: {repo} is not the top of a git checkout (an export?): "
                             "set KILN_DOCS_TRACKED to the list of its files")
        names = subprocess.run(["git", "-C", str(repo), "ls-files", "-z"],
                               check=True, capture_output=True).stdout.decode("utf-8").split("\0")
    return {repo / f for f in names if f}


def stage(repo, work):
    repo, work = Path(repo).resolve(), Path(work)
    tracked = tracked_files(repo)
    guide = repo / "docs/kiln/modding"
    pages = {p for p in tracked if p.is_relative_to(guide) and p.suffix == ".md"}
    if not pages:
        raise SystemExit(f"stage-guide: no guide pages (docs/kiln/modding/*.md) among the {len(tracked)} files of {repo}")

    def rewrite(source, target):
        if links.SCHEME.match(target) or target.startswith(("//", "#")):
            return target
        path = unquote(target.split("#", 1)[0].split("?", 1)[0])
        if not path:
            return target
        dest = ((repo / path.lstrip("/")) if path.startswith("/") else source.parent / path).resolve()
        if dest in pages:
            return target
        directory = dest.is_dir() and any(p.is_relative_to(dest) for p in tracked)
        if not dest.is_relative_to(repo) or not (dest in tracked or directory):
            raise ValueError(f"{source.relative_to(repo)}: missing or private link target {target}")
        suffix = target[len(target.split("#", 1)[0].split("?", 1)[0]):]
        relative = quote(dest.relative_to(repo).as_posix()) + ("/" if directory else "")
        return f"{REPOSITORY}/{'tree' if directory else 'blob'}/main/{relative}{suffix}"

    for source in sorted(pages):
        fence = None
        output = []
        for line in source.read_text(encoding="utf-8").splitlines(keepends=True):
            marker = links.FENCE.match(line)
            if fence:
                if (marker and marker.group(1)[0] == fence[0] and len(marker.group(1)) >= len(fence)
                        and not line.strip()[len(marker.group(1)):].strip()):
                    fence = None
            elif marker:
                fence = marker.group(1)
            else:
                def replace(match):
                    target = match[2]
                    if target.startswith("<"):
                        return match[1] + "<" + rewrite(source, target[1:-1]) + ">"
                    return match[1] + rewrite(source, target)

                # Keep code examples intact, even if they contain Markdown links.
                parts, start = [], 0
                for span in links.CODE_SPAN.finditer(line):
                    parts.append(INLINE.sub(replace, REFERENCE.sub(replace, line[start:span.start()])))
                    parts.append(span[0])
                    start = span.end()
                parts.append(INLINE.sub(replace, REFERENCE.sub(replace, line[start:])))
                line = "".join(parts)
            output.append(line)
        dest = work / "modding" / source.relative_to(guide)
        dest.parent.mkdir(parents=True, exist_ok=True)
        dest.write_text("".join(output), encoding="utf-8")
    print(f"stage-guide: {len(pages)} git-tracked pages; unshipped links point to {REPOSITORY}")


if __name__ == "__main__":
    stage(*sys.argv[1:])
