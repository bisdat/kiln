#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""The site's search index: types, members, guide pages and their sections, small enough to load the first time someone
searches.

  tools/docs-site/search_index.py <site> <api folder> <stage folder>      -> <site>/search/pages.json, members.json

Run by build.sh after `docfx build`: <api folder> is the ManagedReference YAML (docs-work/api), <stage folder> the annotated
Kiln.*.xml docs, <site> the built site. DocFX's own index.json holds the whole text of every page (10 MB on 0.2.0-dev.30.6,
1.8 MB with brotli) and its template's worker parsed and indexed it on every page before the search box worked; the
template's search (template/public/kiln-docs.js) loads these two files instead, on first use, and matches names:
  pages.json    {"v":1, "asm":[assembly], "t":[[name, page, kind, assembly index, summary, generated]...],
                 "g":[[title, href, kind, page title, text]...]}
                t: every type (page relative to api/; kind c s e i d; generated 1 for the game types typegen writes into
                Kiln.Ecs); g: the overview, the service pages (a), the guide pages (g) and their sections (h), href relative
                to the site
  members.json  {"v":1, "m":[[type index, name, kind, anchor, overloads, summary]...]}
                the members, a method's overloads as one entry (kind m p f v k o: method, property, field, event,
                constructor, operator). The anchor (anchor()) is "" for <type's id>_<name>_ (a member with overloads, the
                usual case), "=" for <type's id>_<name> (a field), else the part after the type's id, or the whole id after a
                #; an id is DocFX's: the uid with every non-word character as _.
                Left out, as they are noise in a search of 35,000 names: the members the compiler writes for a record
                (Equals, GetHashCode, ToString, ==, !=, Deconstruct, PrintMembers, EqualityContract) when they have no
                summary, and of the game types typegen generates everything but their fields, which have no summary here.
Summaries are the first sentence of the XML summary, cut short. Every page and anchor is checked against the built HTML: a
name the template cannot find stops the build. Standard library only.
"""
import argparse
from html import unescape
import importlib.util
import json
from pathlib import Path
import re
import sys
import xml.etree.ElementTree as ET

HERE = Path(__file__).resolve().parent


def load(name, filename):
    spec = importlib.util.spec_from_file_location(name, HERE / filename)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


undocumented = load("undocumented", "undocumented.py")
docxml = load("docxml", "docxml.py")
services = load("services", "services.py")

TYPE_KINDS = {"Class": "c", "Struct": "s", "Enum": "e", "Interface": "i", "Delegate": "d"}
MEMBER_KINDS = {"Method": "m", "Property": "p", "Field": "f", "Event": "v", "Constructor": "k", "Operator": "o"}
TYPE_SUMMARY, MEMBER_SUMMARY, TEXT = 130, 100, 140
COMPILER = {"Equals", "GetHashCode", "ToString", "operator ==", "operator !=", "Deconstruct", "PrintMembers", "EqualityContract"}
ID = re.compile(r'\sid="([^"]+)"')
TAG = re.compile(r"<[^>]+>")
HEADING = re.compile(r'<h([123])\b[^>]*\bid="([^"]+)"[^>]*>(.*?)</h\1>', re.S)
PARA = re.compile(r"<p>(.*?)</p>", re.S)


def html_id(uid):
    """DocFX's anchor for a uid (templates/common/common.js getHtmlId: every \\W as _)."""
    return re.sub(r"[^A-Za-z0-9_]", "_", uid)


def anchor(page, name, code):
    """The id of a member on its type's page from its code in members.json (see the module's doc)."""
    own = html_id(page[:-len(".html")])
    if code == "":
        return f"{own}_{html_id(name)}_"
    if code == "=":
        return f"{own}_{html_id(name)}"
    return code[1:] if code.startswith("#") else own + code


def encode(page, name, full):
    """The code of a member's id for members.json: the inverse of anchor()."""
    own = html_id(page[:-len(".html")])
    for code in ("", "="):
        if anchor(page, name, code) == full:
            return code
    return full[len(own):] if full.startswith(own) and full != own else "#" + full


def short(text, limit):
    text = " ".join((text or "").split())
    return text if len(text) <= limit else text[:limit].rsplit(" ", 1)[0].rstrip(",;:") + "…"


def summaries(stage):
    """{documentation id: (first sentence, generated)} of the stage's XML docs; generated: the summary has the shape
    typegen writes (undocumented.GENERATED)."""
    out = {}
    for f in sorted(Path(stage).glob("Kiln.*.xml")):
        for m in ET.parse(f).iter("member"):
            s = m.find("summary")
            if s is not None:
                text = docxml.plain(s)
                gen = bool(undocumented.GENERATED.match(re.sub(r"^(\S+::\S+): ", r"<code>\1</code>: ", text))) or \
                    text.startswith(("Game enum ", "Game bitmask ", "A game StaticArray"))
                out[m.get("name")] = (docxml.first_sentence(s, 400), gen)
    return out


def bare(name):
    """A member's name without its parameter list: BlockAt(WorldPosition) -> BlockAt, operator ==(A, A) -> operator ==."""
    return name.split("(", 1)[0].strip()


def api_entries(api, stage):
    """(assemblies, types, members) for the index, from the metadata (one YAML file per type, its members in it)."""
    sums = summaries(stage)
    files = sorted(p for p in Path(api).glob("*.yml") if p.name != "toc.yml")
    asms, types, members = [], [], []
    asm_index, type_index = {}, {}
    pending = []
    for f in files:
        items = undocumented.items(f)
        page = services.page(f.name)
        for item in items:
            kind = item.get("type")
            if kind not in TYPE_KINDS or not item.get("assemblies"):
                continue
            asm = item["assemblies"][0]
            if asm not in asm_index:
                asm_index[asm] = len(asms)
                asms.append(asm)
            text, gen = sums.get(item.get("commentId"), ("", False))
            gen = gen and item["uid"].startswith(undocumented.GENERATED_ROOT)
            type_index[item["uid"]] = len(types)
            types.append([services.display(item, asm), page, TYPE_KINDS[kind], asm_index[asm],
                          short(text, 60 if gen else TYPE_SUMMARY), 1 if gen else 0])
        for item in items:
            if item.get("type") in MEMBER_KINDS and item.get("parent") in type_index:
                pending.append(item)
    groups = {}
    for item in pending:
        kind = item["type"]
        ti = type_index[item["parent"]]
        generated = types[ti][5]
        if generated and kind != "Field":
            continue
        name = bare(item.get("name") or item["uid"].rsplit(".", 1)[-1])
        text, _ = sums.get(item.get("commentId"), ("", False))
        if name in COMPILER and not text:
            continue
        key = (ti, name, kind)
        g = groups.get(key)
        if g is None:
            page = types[ti][1]
            code = encode(page, name, html_id(item.get("overload") or item["uid"]))
            g = groups[key] = [ti, name, MEMBER_KINDS[kind], code, 0, "" if generated else short(text, MEMBER_SUMMARY)]
            members.append(g)
        g[4] += 1
    return asms, types, members


def page_entries(site, toc_index=None):
    """[title, href, kind, page title, text] of the overview, the service pages and the guide (modding/**) with its h2 and h3
    sections, read from the built HTML (DocFX's own heading ids)."""
    site = Path(site)
    out = []
    overview = site / "api/index.html"
    if overview.is_file():
        out.append(["API reference overview", "api/index.html", "o", "", "Every assembly of the API: its KilnServices name, what it is, its guide"])
    if toc_index and Path(toc_index).is_file():
        for g in json.loads(Path(toc_index).read_text(encoding="utf-8"))["groups"]:
            if (site / "api" / g["href"]).is_file():
                what = (f"KilnServices {g['service']}. " if g.get("service") else "") + (g.get("summary") or "")
                out.append([g["name"], "api/" + g["href"], "a", "", short(what, TEXT)])
    for page in sorted((site / "modding").rglob("*.html")) if (site / "modding").is_dir() else []:
        html = page.read_text(encoding="utf-8", errors="replace")
        body = html.split("<article", 1)[-1]
        href = page.relative_to(site).as_posix()
        heads = list(HEADING.finditer(body))
        title = next((clean(m.group(3)) for m in heads if m.group(1) == "1"), page.stem)
        first = PARA.search(body)
        out.append([title, href, "g", "", short(clean(first.group(1)) if first else "", TEXT)])
        for i, m in enumerate(heads):
            if m.group(1) == "1":
                continue
            end = heads[i + 1].start() if i + 1 < len(heads) else len(body)
            para = PARA.search(body, m.end(), end)
            out.append([clean(m.group(3)), f"{href}#{m.group(2)}", "h", title, short(clean(para.group(1)) if para else "", TEXT)])
    return out


def clean(fragment):
    return " ".join(unescape(TAG.sub("", fragment)).split())


def build(site, api, stage):
    site, api = Path(site), Path(api)
    asms, types, members = api_entries(api, stage)
    pages = page_entries(site, api / "toc/index.json")
    out = site / "search"
    out.mkdir(parents=True, exist_ok=True)
    dump = lambda doc: json.dumps(doc, ensure_ascii=False, separators=(",", ":")) + "\n"
    (out / "pages.json").write_text(dump({"v": 1, "asm": asms, "t": types, "g": pages}), encoding="utf-8")
    (out / "members.json").write_text(dump({"v": 1, "m": members}), encoding="utf-8")
    return asms, types, members, pages


def check(site):
    """The pages and anchors of the index that the built site does not have: ["href#anchor"]."""
    site = Path(site)
    doc = json.loads((site / "search/pages.json").read_text(encoding="utf-8"))
    members = json.loads((site / "search/members.json").read_text(encoding="utf-8"))["m"]
    ids = {}

    def has(href):
        path, _, anchor = href.partition("#")
        if path not in ids:
            f = site / path
            ids[path] = set(ID.findall(f.read_text(encoding="utf-8", errors="replace"))) if f.is_file() else None
        return ids[path] is not None and (not anchor or anchor in ids[path])

    missing = [href for href in ("api/" + t[1] + "#" + html_id(t[1][:-len(".html")]) for t in doc["t"]) if not has(href)]
    missing += [g[1] for g in doc["g"] if not has(g[1])]
    for ti, name, _, code, _, _ in members:
        page = doc["t"][ti][1]
        href = f"api/{page}#{anchor(page, name, code)}"
        if not has(href):
            missing.append(href)
    return missing


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("site")
    ap.add_argument("api")
    ap.add_argument("stage")
    a = ap.parse_args()
    for p in (a.site, a.api):
        if not Path(p).is_dir():
            sys.exit(f"search_index: no folder {p}")
    asms, types, members, pages = build(a.site, a.api, a.stage)
    missing = check(a.site)
    sizes = ", ".join(f"{f.name} {f.stat().st_size // 1024} KB" for f in sorted((Path(a.site) / "search").glob("*.json")))
    print(f"search: {len(types)} types, {len(members)} members, {len(pages)} pages and sections ({sizes})")
    if missing:
        sys.exit(f"search: {len(missing)} entries point at no page or anchor of the site, the first: " + ", ".join(missing[:5]))


if __name__ == "__main__":
    main()
