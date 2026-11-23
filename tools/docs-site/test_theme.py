#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""theme.py (the template in a build: hashed static files, the navigation, the release) and the template it copies.

  python3 -I tools/docs-site/test_theme.py
"""
import importlib.util
import json
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import unittest

HERE = Path(__file__).resolve().parent


def load(name, filename):
    spec = importlib.util.spec_from_file_location(name, HERE / filename)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


theme = load("theme", "theme.py")


class Theme(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.work = Path(self.tmp.name) / "docs-work"
        guide = self.work / "modding"
        (guide / "schemas").mkdir(parents=True)
        (guide / "README.md").write_text("# Writing Kiln mods\n\n| [b.md](b.md) | [a.md](a.md) | [x](https://github.com/o/r/blob/main/docs/c.md) "
                                         "| [s](schemas/s.md) | [b again](b.md#top) |\n")
        (guide / "a.md").write_text("Intro.\n\n# The `a` page\n")
        (guide / "b.md").write_text("```md\n# not a title\n```\n# B: [linked](a.md) title #\n")
        (guide / "z.md").write_text("no heading\n")
        (guide / "schemas/s.md").write_text("# Schema S\n")
        (self.work / "api/toc").mkdir(parents=True)
        (self.work / "api/toc/index.json").write_text(json.dumps({"v": 1, "groups": [
            {"name": "Kiln.Api", "href": "services/Kiln.Api.html", "service": None, "summary": "", "types": 3}]}))
        (self.work / "docfx.json").write_text(json.dumps({"build": {"globalMetadata": {"_appName": "Kiln"}}}))
        self.assets, self.nav = theme.configure(self.work, release="0.2.0-dev.30.6", year=2026)
        self.meta = json.loads((self.work / "docfx.json").read_text())["build"]["globalMetadata"]

    def test_static_files_get_content_hashed_names(self):
        for key, name in theme.ASSETS.items():
            stem, ext = name.rsplit(".", 1)
            self.assertRegex(self.assets[key], rf"^static/{re.escape(stem)}\.[0-9a-f]{{10}}\.{ext}$")
            self.assertTrue((self.work / "template" / self.assets[key]).is_file(), key)
        self.assertEqual(sorted(p.name for p in (self.work / "template/static").iterdir()),
                         sorted(a.split("/")[1] for a in self.assets.values()))

    def test_the_stylesheet_names_the_hashed_font(self):
        css = (self.work / "template" / self.assets["css"]).read_text()
        font = self.assets["font"].split("/")[1]
        self.assertIn(f"url('{font}')", css)
        self.assertNotIn("url('fraunces.woff2')", css)

    def test_a_changed_file_changes_its_name(self):
        other = Path(self.tmp.name) / "template"
        subprocess.run(["cp", "-r", str(HERE / "template"), str(other)], check=True)
        (other / "static/kiln-docs.css").write_text((other / "static/kiln-docs.css").read_text() + "\n/* changed */\n")
        assets = theme.copy_template(other, Path(self.tmp.name) / "out", {"nav.json": json.dumps(self.nav, ensure_ascii=False, separators=(",", ":")).encode()})
        self.assertNotEqual(assets["css"], self.assets["css"])
        self.assertEqual(assets["js"], self.assets["js"])

    def test_the_metadata_names_the_assets_the_release_and_the_year(self):
        self.assertEqual(self.meta["_kilnAssets"], self.assets)
        self.assertEqual(self.meta["_kilnRelease"], "0.2.0-dev.30.6")
        self.assertEqual(self.meta["_kilnYear"], "2026")
        self.assertEqual(self.meta["_appName"], "Kiln")
        self.assertNotIn("_kilnGuides", self.meta)      # the navigation is one shared file, not text in every page

    def test_the_navigation_lists_the_groups_and_the_guide_in_the_readme_order(self):
        self.assertEqual(self.nav["groups"], [["Kiln.Api", "api/services/Kiln.Api.html", 3]])
        self.assertEqual(self.nav["guides"], [["Writing Kiln mods", "modding/README.html"], ["B", "modding/b.html"],
                                              ["The a page", "modding/a.html"], ["z", "modding/z.html"]])
        self.assertEqual(self.nav["schemas"], [["Schema S", "modding/schemas/s.html"]])
        shipped = json.loads((self.work / "template" / self.assets["nav"]).read_text())
        self.assertEqual(shipped, self.nav)

    def test_a_guide_page_has_a_short_name_in_the_navigation(self):
        self.assertEqual(theme.label("Tasks: heavy work off the server's tick"), "Tasks")
        self.assertEqual(theme.label("VS Code extension (tools/vscode)"), "VS Code extension")
        self.assertEqual(theme.label("Lifecycle, the Mods API, IMC, hot reload"), "Lifecycle, the Mods API, IMC, hot reload")
        self.assertEqual(theme.label("(advanced)"), "(advanced)")

    def test_the_command_line(self):
        r = subprocess.run([sys.executable, "-I", str(HERE / "theme.py"), str(self.work), "--release", ""], capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn("theme: 7 static files with hashed names", r.stdout)
        self.assertIn("1 groups, 4 guide pages and 1 schemas in the navigation, release (none: next release)", r.stdout)


class Template(unittest.TestCase):
    """The layout and the template's files: what every page of every release's reference carries."""
    master = (HERE / "template/layout/_master.tmpl").read_text()
    css = (HERE / "template/static/kiln-docs.css").read_text()
    js = (HERE / "template/static/kiln-docs.js").read_text()

    def test_docfx_uses_the_template_over_default_and_modern(self):
        config = json.loads((HERE / "docfx.json").read_text())["build"]
        self.assertEqual(config["template"], ["default", "modern", "template"])
        self.assertIn({"files": ["api/toc/*.json"]}, config["resource"])
        self.assertFalse(config["globalMetadata"]["_enableSearch"])

    def test_only_the_templates_static_files_ship(self):
        # the modern template's public/ (Bootstrap, highlight.js, mermaid, lunr: 7 MB) is included by its own layout only
        self.assertIn("{{!include(/^static/.*/)}}", self.master)
        self.assertNotIn("include(/^public", self.master)
        self.assertNotIn("docfx.min", self.master)

    def test_every_asset_link_is_relative(self):
        # a release's pinned copy at /kiln/downloads/<version>/sdk/api/ needs its own files
        for ref in re.findall(r'(?:href|src)="([^"]+)"', self.master):
            if not ref.startswith(("https://frostdev.io/", "https://github.com/", "mailto:", "#")):
                self.assertTrue(ref.startswith("{{_rel}}"), ref)

    def test_no_external_hosts_trackers_or_cdns(self):
        hosts = set(re.findall(r"https?://([^/\"'\s)]+)", self.master + self.css + self.js))
        self.assertLessEqual(hosts, {"frostdev.io", "github.com"})
        for text in (self.master, self.css, self.js):
            self.assertNotRegex(text, r"googletagmanager|google-analytics|gtag\(|@import|fonts\.googleapis")

    def test_the_header_is_frostdev_ios_kiln_navigation_with_the_api_reference(self):
        nav = re.search(r'<nav aria-label="Main navigation">(.*?)</nav>', self.master, re.S).group(1)
        self.assertEqual(re.findall(r">([^<>]+?)\s*(?:<span|</a>)", nav),
                         ["Projects", "Guides", "Crucible", "Download", "Wiki", "API reference", "Roadmap", "Agent skill", "Sign in"])
        self.assertIn('<a href="https://frostdev.io/kiln/api/" aria-current="page">API reference</a>', nav)

    def test_cloudflare_does_not_hold_back_the_script(self):
        self.assertIn('<script defer data-cfasync="false" src="{{_rel}}{{_kilnAssets.js}}"></script>', self.master)

    def test_the_layout_shows_the_release(self):
        self.assertIn("Kiln {{_kilnRelease}}", self.master)
        self.assertIn('<meta name="kiln:release" content="{{_kilnRelease}}">', self.master)

    def test_the_script_loads_the_data_the_build_writes(self):
        for path in ("api/toc/", "search/pages.json", "search/members.json", "kiln:nav"):
            self.assertIn(path, self.js)

    def test_the_fonts_licence_ships_with_it(self):
        self.assertIn("{{!include(fraunces-OFL.txt)}}", self.master)
        self.assertIn("SIL Open Font License", (HERE / "template/fraunces-OFL.txt").read_text())


if __name__ == "__main__":
    unittest.main()
