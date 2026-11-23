#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""search_index.py: the search's two files (types and guide pages, members) and the check of their pages and anchors.

  python3 -I tools/docs-site/test_search_index.py
"""
import importlib.util
import json
from pathlib import Path
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


search = load("search_index", "search_index.py")


def item(uid, kind, parent=None, name=None, overload=None, assembly="Kiln.Build", cid=None):
    lines = [f"- uid: {uid}", f"  commentId: {cid or ('T:' if parent is None else 'M:') + uid}", f"  name: {name or uid.rsplit('.', 1)[-1]}",
             f"  type: {kind}", "  assemblies:", f"  - {assembly}", "  namespace: Kiln"]
    if parent:
        lines.append(f"  parent: {parent}")
    if overload:
        lines.append(f"  overload: {overload}")
    return "\n".join(lines) + "\n"


class Index(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        root = Path(self.tmp.name)
        self.api, self.stage, self.site = root / "api", root / "stage", root / "site"
        for p in (self.api / "toc", self.stage, self.site / "api/services", self.site / "modding"):
            p.mkdir(parents=True)
        (self.api / "Kiln.Build.yml").write_text("items:\n" + "".join([
            item("Kiln.Build", "Class"),
            item("Kiln.Build.BlockAt(Kiln.WorldPosition)", "Method", "Kiln.Build", "BlockAt(WorldPosition)", "Kiln.Build.BlockAt*"),
            item("Kiln.Build.BlockAt(Kiln.CellPos)", "Method", "Kiln.Build", "BlockAt(CellPos)", "Kiln.Build.BlockAt*"),
            item("Kiln.Build.Grid", "Field", "Kiln.Build", cid="F:Kiln.Build.Grid"),
            item("Kiln.Build.#ctor", "Constructor", "Kiln.Build", "Build()", "Kiln.Build.#ctor*"),
            item("Kiln.Build.op_Equality(Kiln.Build,Kiln.Build)", "Operator", "Kiln.Build", "operator ==(Build, Build)", "Kiln.Build.op_Equality*"),
            item("Kiln.Build.ToString", "Method", "Kiln.Build", "ToString()", "Kiln.Build.ToString*"),
        ]) + "references:\n- uid: System.Object\n")
        (self.api / "Kiln.Box-1.yml").write_text("items:\n" + item("Kiln.Box`1", "Struct", name="Box<T>") +
                                                item("Kiln.Box`1.Value", "Property", "Kiln.Box`1", "Value", "Kiln.Box`1.Value*"))
        (self.api / "Kiln.Ecs.Position.yml").write_text("items:\n" + "".join([
            item("Kiln.Ecs.Position", "Struct", name="Ecs.Position", assembly="Kiln.Game"),
            item("Kiln.Ecs.Position.x", "Field", "Kiln.Ecs.Position", cid="F:Kiln.Ecs.Position.x", assembly="Kiln.Game"),
            item("Kiln.Ecs.Position.Hash", "Property", "Kiln.Ecs.Position", overload="Kiln.Ecs.Position.Hash*", assembly="Kiln.Game")]))
        (self.api / "toc.yml").write_text("### YamlMime:TableOfContent\n")
        (self.stage / "Kiln.Build.xml").write_text(
            "<doc><members>"
            "<member name='T:Kiln.Build'><summary>Changes the world. Second sentence.</summary></member>"
            "<member name='M:Kiln.Build.BlockAt(Kiln.WorldPosition)'><summary>The block at a <see cref='T:Kiln.WorldPosition'/>.</summary></member>"
            "<member name='F:Kiln.Build.Grid'><summary>The grid.</summary></member>"
            "<member name='T:Kiln.Ecs.Position'><summary><c>keen::ecs::Position</c>: an ECS component, 12 bytes, align 4.</summary></member>"
            "<member name='F:Kiln.Ecs.Position.x'><summary>x: float.</summary></member>"
            "</members></doc>")
        (self.api / "toc/index.json").write_text(json.dumps({"v": 1, "groups": [
            {"name": "Kiln.Build", "href": "services/Kiln.Build.html", "service": "Build", "summary": "Voxels and props", "types": 1}]}))
        (self.site / "api/services/Kiln.Build.html").write_text("<h1 id='x'>Kiln.Build</h1>")
        (self.site / "api/index.html").write_text("<h1>API</h1>")
        (self.site / "modding/blocks.html").write_text(
            '<header>nav</header><article><h1 id="building-blocks">Building <code>blocks</code></h1><p>Say what you &amp; mean.</p>'
            '<h2 id="spells">Spells</h2><p>A spell is <a href="x">a block</a>.</p><h3 id="cost">Cost</h3></article>')
        self.asms, self.types, self.members, self.pages = search.build(self.site, self.api, self.stage)

    def pages_html(self, ids):
        for page, anchors in ids.items():
            (self.site / "api" / page).write_text("".join(f'<h3 id="{a}"></h3>' for a in anchors))

    def member(self, name):
        return next(m for m in self.members if m[1] == name)

    def test_types_have_their_page_kind_assembly_and_first_sentence(self):
        self.assertEqual(self.asms, ["Kiln.Build", "Kiln.Game"])
        self.assertIn(["Build", "Kiln.Build.html", "c", 0, "Changes the world.", 0], self.types)
        self.assertIn(["Box<T>", "Kiln.Box-1.html", "s", 0, "", 0], self.types)

    def test_a_generated_game_type_is_marked_and_keeps_only_its_fields(self):
        position = self.types.index(next(t for t in self.types if t[0] == "Ecs.Position"))
        self.assertEqual(self.types[position][5], 1)
        self.assertEqual([m[1] for m in self.members if m[0] == position], ["x"])
        self.assertEqual(self.member("x")[5], "")          # its summary is the generator's

    def test_overloads_are_one_entry_with_their_count(self):
        self.assertEqual(self.member("BlockAt"), [self.types.index(next(t for t in self.types if t[0] == "Build")), "BlockAt", "m", "", 2,
                                                  "The block at a WorldPosition."])

    def test_compiler_members_without_a_summary_are_left_out(self):
        names = [m[1] for m in self.members]
        self.assertNotIn("ToString", names)
        self.assertNotIn("operator ==", names)
        self.assertIn("Build", names)                      # the constructor

    def test_anchors_are_docfx_ids(self):
        build = self.types[self.member("BlockAt")[0]][1]
        self.assertEqual(search.anchor(build, "BlockAt", self.member("BlockAt")[3]), "Kiln_Build_BlockAt_")
        self.assertEqual(self.member("Grid")[3], "=")
        self.assertEqual(search.anchor(build, "Grid", "="), "Kiln_Build_Grid")
        self.assertEqual(search.anchor(build, "Build", self.member("Build")[3]), "Kiln_Build__ctor_")
        self.assertEqual(search.anchor("Kiln.Box-1.html", "Value", self.member("Value")[3]), "Kiln_Box_1_Value_")
        for page, name, full in (("Kiln.Build.html", "X", "Other_Id"), ("Kiln.Build.html", "Y", "Kiln_Build_op_Y_")):
            self.assertEqual(search.anchor(page, name, search.encode(page, name, full)), full)

    def test_guide_pages_and_sections_come_from_the_built_html(self):
        self.assertIn(["Building blocks", "modding/blocks.html", "g", "", "Say what you & mean."], self.pages)
        self.assertIn(["Spells", "modding/blocks.html#spells", "h", "Building blocks", "A spell is a block."], self.pages)
        self.assertIn(["Cost", "modding/blocks.html#cost", "h", "Building blocks", ""], self.pages)
        self.assertIn(["Kiln.Build", "api/services/Kiln.Build.html", "a", "", "KilnServices Build. Voxels and props"], self.pages)
        self.assertEqual(self.pages[0][:3], ["API reference overview", "api/index.html", "o"])

    def test_the_files_are_compact_json(self):
        text = (self.site / "search/members.json").read_text()
        self.assertNotIn(": ", text)
        self.assertEqual(json.loads(text)["v"], 1)
        self.assertEqual(json.loads((self.site / "search/pages.json").read_text())["asm"], self.asms)

    def test_check_finds_missing_pages_and_anchors(self):
        missing = search.check(self.site)
        self.assertIn("api/Kiln.Build.html#Kiln_Build", missing)
        self.pages_html({"Kiln.Build.html": ["Kiln_Build", "Kiln_Build_BlockAt_", "Kiln_Build_Grid", "Kiln_Build__ctor_"],
                         "Kiln.Box-1.html": ["Kiln_Box_1", "Kiln_Box_1_Value_"], "Kiln.Ecs.Position.html": ["Kiln_Ecs_Position", "Kiln_Ecs_Position_x"]})
        self.assertEqual(search.check(self.site), [])
        (self.site / "api/Kiln.Box-1.html").write_text('<h1 id="Kiln_Box_1"></h1>')
        self.assertEqual(search.check(self.site), ["api/Kiln.Box-1.html#Kiln_Box_1_Value_"])

    def test_the_command_line_fails_on_a_missing_anchor(self):
        r = subprocess.run([sys.executable, "-I", str(HERE / "search_index.py"), str(self.site), str(self.api), str(self.stage)],
                           capture_output=True, text=True)
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("search: 3 types, 5 members, 5 pages and sections", r.stdout)
        self.assertIn("entries point at no page or anchor of the site", r.stderr)


if __name__ == "__main__":
    unittest.main()
