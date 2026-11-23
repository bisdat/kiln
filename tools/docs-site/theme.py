#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""The site's template in a build: frostdev.io's Kiln look, its static files under content-hashed names, and the navigation.

  tools/docs-site/theme.py <docs-work> [--template <folder>] [--release <version>]

Run by build.sh after services.py, before `docfx build`. It
  - writes the navigation, static/nav.json: the API's groups (from api/toc/index.json, which services.py wrote), the guide
    pages (modding/*.md in the order the guide's README links them, then A-Z) and the file formats (modding/schemas/*.md),
    hrefs relative to the site. One small file every page shares, cached, not 19 KB of links in each of 4,300 pages;
  - copies template/ (tools/docs-site/template: layout/_master.tmpl over DocFX's default and modern templates) to
    <docs-work>/template, each file of static/ (and nav.json) under a name with the first 10 hex digits of its SHA-256
    (kiln-docs.css -> kiln-docs.1a2b3c4d5e.css), with the names inside the CSS and JS rewritten first; the frostdev.io
    server can then keep these files a year (ops/public-site.nginx.conf: static/*.<10 hex>.*), and a release that changes
    one changes its name;
  - writes into <docs-work>/docfx.json's globalMetadata the hashed names (_kilnAssets), the release (_kilnRelease) and the
    year (_kilnYear).
The layout's links are relative ({{_rel}}), so a release's pinned copy at /kiln/downloads/<version>/sdk/api/ works as well as
/kiln/api/. Standard library only.
"""
import argparse
import datetime
import hashlib
import json
from pathlib import Path
import re
import shutil
import sys

HERE = Path(__file__).resolve().parent
ASSETS = {  # key in _kilnAssets: file in template/static (nav.json: written by nav())
    "css": "kiln-docs.css",
    "js": "kiln-docs.js",
    "font": "fraunces.woff2",
    "mark": "kiln-mark.svg",
    "icon": "frostdev-icon.webp",
    "favicon": "frostdev-favicon.png",
    "nav": "nav.json",
}
STATIC = "static"
TEXT = (".css", ".js")
HEADING = re.compile(r"^#\s+(.+?)\s*#*\s*$")
LINK = re.compile(r"\]\(\s*<?([^)\s>#]+\.md)(?:#[^)\s>]*)?>?\s*\)")


def hashed_name(name, h):
    stem, dot, ext = name.rpartition(".")
    return f"{stem}.{h}.{ext}" if dot else f"{name}.{h}"


def copy_template(src, dst, extra=None):
    """Copies the template, with extra {name: bytes} added to its static/; returns {key: "static/<hashed name>"} for ASSETS.
    Other files are hashed first, then the CSS and JS, whose references to the others are rewritten to the hashed names
    before their own hash is taken."""
    src, dst = Path(src), Path(dst)
    if dst.exists():
        shutil.rmtree(dst)
    shutil.copytree(src, dst, ignore=shutil.ignore_patterns(STATIC))
    out = dst / STATIC
    out.mkdir(parents=True)
    content = {p.name: p.read_bytes() for p in sorted((src / STATIC).iterdir()) if p.is_file()}
    content.update(extra or {})
    renamed = {}
    for name, data in sorted(content.items()):
        if Path(name).suffix not in TEXT:
            renamed[name] = hashed_name(name, hashlib.sha256(data).hexdigest()[:10])
            (out / renamed[name]).write_bytes(data)
    for name, data in sorted(content.items()):
        if Path(name).suffix in TEXT:
            text = data.decode("utf-8")
            for old, new in renamed.items():
                text = text.replace(old, new)
            renamed[name] = hashed_name(name, hashlib.sha256(text.encode("utf-8")).hexdigest()[:10])
            (out / renamed[name]).write_text(text, encoding="utf-8")
    missing = [n for n in ASSETS.values() if n not in renamed]
    if missing:
        raise SystemExit(f"theme: template/{STATIC} has no {', '.join(missing)}")
    return {key: f"{STATIC}/{renamed[name]}" for key, name in ASSETS.items()}


def title_of(page):
    """The page's first heading as plain text (code spans and emphasis marks dropped)."""
    fence = False
    for line in page.read_text(encoding="utf-8").splitlines():
        if line.lstrip().startswith(("```", "~~~")):
            fence = not fence
        m = None if fence else HEADING.match(line)
        if m:
            return re.sub(r"[`*_]", "", re.sub(r"\[([^\]]*)\]\([^)]*\)", r"\1", m.group(1))).strip()
    return page.stem


def label(title):
    """A guide page's name in the navigation: its title up to the first colon, without a closing parenthesis
    ("Tasks: heavy work off the server's tick" -> "Tasks", "VS Code extension (tools/vscode)" -> "VS Code extension")."""
    short = re.sub(r"\s*\([^()]*\)$", "", title.split(": ", 1)[0]).strip()
    return short or title


def guides(work):
    """(pages, schemas): [{"href", "name"}] of the staged guide (<docs-work>/modding). Pages in the order README.md links them
    (README first), the rest A-Z; schemas A-Z."""
    root = Path(work) / "modding"
    if not root.is_dir():
        return [], []
    pages = {p.name: p for p in root.glob("*.md")}
    order = ["README.md"] if "README.md" in pages else []
    if "README.md" in pages:
        for target in LINK.findall(pages["README.md"].read_text(encoding="utf-8")):
            name = Path(target).name
            if "/" not in target.strip("./") and name in pages and name not in order:
                order.append(name)
    order += sorted(n for n in pages if n not in order)
    entry = lambda p, rel: {"href": "modding/" + rel[:-len(".md")] + ".html", "name": label(title_of(p))}
    schemas = sorted((root / "schemas").glob("*.md")) if (root / "schemas").is_dir() else []
    return [entry(pages[n], n) for n in order], [entry(p, "schemas/" + p.name) for p in schemas]


def groups(work):
    """[[name, href, number of types]] of the API's groups, from api/toc/index.json (services.py); hrefs relative to the site."""
    index = Path(work) / "api/toc/index.json"
    if not index.is_file():
        return []
    return [[g["name"], "api/" + g["href"], g["types"]] for g in json.loads(index.read_text(encoding="utf-8"))["groups"]]


def nav(work):
    """static/nav.json: {"v":1, "groups":[[name, href, types]], "guides":[[name, href]], "schemas":[[name, href]]}."""
    pages, schemas = guides(work)
    return {"v": 1, "groups": groups(work), "guides": [[p["name"], p["href"]] for p in pages],
            "schemas": [[p["name"], p["href"]] for p in schemas]}


def configure(work, template=HERE / "template", release=None, year=None):
    work = Path(work)
    navigation = nav(work)
    data = json.dumps(navigation, ensure_ascii=False, separators=(",", ":")).encode("utf-8")
    assets = copy_template(template, work / "template", {ASSETS["nav"]: data})
    config_path = work / "docfx.json"
    config = json.loads(config_path.read_text(encoding="utf-8"))
    meta = config["build"].setdefault("globalMetadata", {})
    meta.update({
        "_kilnAssets": assets,
        "_kilnRelease": release or "",
        "_kilnYear": str(year or datetime.date.today().year),
    })
    config_path.write_text(json.dumps(config, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    return assets, navigation


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("work")
    ap.add_argument("--template", default=str(HERE / "template"))
    ap.add_argument("--release", default="")
    a = ap.parse_args()
    if not (Path(a.work) / "docfx.json").is_file():
        sys.exit(f"theme: no docfx.json in {a.work}")
    assets, navigation = configure(a.work, a.template, a.release)
    print(f"theme: {len(assets)} static files with hashed names ({assets['css'].split('/')[-1]}, {assets['js'].split('/')[-1]}), "
          f"{len(navigation['groups'])} groups, {len(navigation['guides'])} guide pages and {len(navigation['schemas'])} schemas "
          f"in the navigation, release {a.release or '(none: next release)'}")


if __name__ == "__main__":
    main()
