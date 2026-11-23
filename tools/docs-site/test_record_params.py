#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Tests for tools/docs-site/record_params.py: which types count as positional records, what documents a property (a
<param> on the record or its constructor, or a summary of its own, or an <inheritdoc cref=.../>), what is left out (experimental types,
a Deconstruct that does not match a constructor, parameters that are no property), and the exit codes.

  python3 -I tools/docs-site/test_record_params.py
Builds small repositories under a temporary folder; nothing else is written. KILN_API_XML=<xml dir> also checks this
repository against the XML docs of a Release build (the same check tools/check.sh runs).
"""
import contextlib
import importlib.util
import io
import os
from pathlib import Path
import tempfile
import unittest
from unittest import mock

HERE = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location("record_params", HERE / "record_params.py")
rp = importlib.util.module_from_spec(spec)
spec.loader.exec_module(rp)

API = """#nullable enable
Kiln.Mods
const Kiln.Mods.ApiVersion = "0.2.15" -> string!
Kiln.Pair
Kiln.Pair.<Clone>$() -> Kiln.Pair!
Kiln.Pair.Deconstruct(out int Left, out int Right) -> void
Kiln.Pair.Left.get -> int
Kiln.Pair.Left.init -> void
Kiln.Pair.Pair(int Left, int Right = 0) -> void
Kiln.Pair.Right.get -> int
Kiln.Pair.Right.init -> void
Kiln.Box<T>
Kiln.Box<T>.Box(string! Name) -> void
Kiln.Box<T>.Deconstruct(out string! Name) -> void
Kiln.Box<T>.Name.get -> string!
Kiln.Plain
Kiln.Plain.Plain(int A) -> void
Kiln.Plain.A.get -> int
Kiln.Odd
Kiln.Odd.Deconstruct(out int A) -> void
Kiln.Odd.Odd(int B) -> void
Kiln.Odd.A.get -> int
Kiln.Mixed
Kiln.Mixed.Deconstruct(out int Shown, out int hidden) -> void
Kiln.Mixed.Mixed(int Shown, int hidden) -> void
Kiln.Mixed.Shown.get -> int
[KILNX001]Kiln.Lab
[KILNX001]Kiln.Lab.Deconstruct(out int Z) -> void
[KILNX001]Kiln.Lab.Lab(int Z) -> void
[KILNX001]Kiln.Lab.Z.get -> int
"""
XML = """<?xml version="1.0"?>
<doc><assembly><name>Kiln.Api</name></assembly><members>
<member name="T:Kiln.Pair"><summary>Two numbers.</summary><param name="Left">The left one.</param><param name="Right">The right one.</param></member>
<member name="P:Kiln.Pair.Left"><summary>The left one.</summary></member>
<member name="P:Kiln.Pair.Right"><summary>The right one.</summary></member>
<member name="T:Kiln.Box`1"><summary>A box.</summary><param name="Name">What it holds.</param></member>
<member name="P:Kiln.Box`1.Name"><summary>What it holds.</summary></member>
<member name="T:Kiln.Plain"><summary>Plain.</summary></member>
<member name="T:Kiln.Mixed"><summary>Mixed.</summary></member>
<member name="P:Kiln.Mixed.Shown"><summary>Shown.</summary></member>
</members></doc>
"""


class Repo:
    def __init__(self, tmp, xml=XML, api=API):
        self.root = Path(tmp)
        (self.root / "managed/Kiln.Api").mkdir(parents=True)
        (self.root / "managed/Kiln.Api/Mods.cs").write_text('class Mods { const string ApiVersion = "0.2.15"; }\n')
        (self.root / "managed/Kiln.Api/PublicAPI.Shipped.txt").write_text(api)
        (self.root / "managed/Kiln.Api/PublicAPI.Unshipped.txt").write_text("#nullable enable\n")
        self.xml = self.root / "xml"
        self.xml.mkdir()
        if xml is not None:
            (self.xml / "Kiln.Api.xml").write_text(xml)

    def check(self):
        return rp.check(self.root, self.xml)


class Records(unittest.TestCase):
    def repo(self, **kw):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        return Repo(tmp.name, **kw)

    def test_the_positional_records_are_found(self):
        found = rp.positional([rp.ai.parse_api_line(l) for l in API.splitlines() if l and not l.startswith("#")])
        # Plain has no Deconstruct, Odd's Deconstruct names no constructor's parameters, Lab is experimental, Mixed's
        # `hidden` is no property
        self.assertEqual(sorted(found), ["Kiln.Box`1", "Kiln.Mixed", "Kiln.Pair"])
        self.assertEqual(found["Kiln.Mixed"], ["Shown"])
        self.assertEqual(found["Kiln.Pair"], ["Left", "Right"])

    def test_a_documented_tree_is_clean(self):
        missing, records, props = self.repo().check()
        self.assertEqual((missing, records, props), ([], 3, 4))

    def test_a_param_without_a_property_doc_is_enough(self):
        # the compiler writes P: members from <param>; a XML written another way has the <param> only
        xml = XML.replace('<member name="P:Kiln.Pair.Right"><summary>The right one.</summary></member>\n', "")
        self.assertEqual(self.repo(xml=xml).check()[0], [])

    def test_a_summary_without_a_param_is_enough(self):
        xml = XML.replace('<param name="Right">The right one.</param>', "")
        self.assertEqual(self.repo(xml=xml).check()[0], [])

    def test_a_constructors_param_counts(self):
        xml = XML.replace('<param name="Right">The right one.</param>', "").replace(
            '<member name="P:Kiln.Pair.Right"><summary>The right one.</summary></member>\n', "") + ""
        xml = xml.replace("</members>", '<member name="M:Kiln.Pair.#ctor(System.Int32,System.Int32)"><param name="Right">Right.</param></member></members>')
        self.assertEqual(self.repo(xml=xml).check()[0], [])

    def test_inheritdoc_with_a_cref_counts(self):
        xml = XML.replace('<member name="P:Kiln.Pair.Right"><summary>The right one.</summary></member>',
                          '<member name="P:Kiln.Pair.Right"><inheritdoc cref="P:Kiln.Pair.Left"/></member>').replace('<param name="Right">The right one.</param>', "")
        self.assertEqual(self.repo(xml=xml).check()[0], [])

    def test_a_missing_param_is_found(self):
        xml = XML.replace('<param name="Right">The right one.</param>', "").replace(
            '<member name="P:Kiln.Pair.Right"><summary>The right one.</summary></member>\n', "")
        self.assertEqual(self.repo(xml=xml).check()[0], ["Kiln.Api: Kiln.Pair.Right"])

    def test_an_empty_param_is_found(self):
        xml = XML.replace('<param name="Left">The left one.</param>', '<param name="Left"> </param>').replace(
            '<member name="P:Kiln.Pair.Left"><summary>The left one.</summary></member>\n', "")
        self.assertEqual(self.repo(xml=xml).check()[0], ["Kiln.Api: Kiln.Pair.Left"])

    def test_a_generic_record(self):
        xml = XML.replace('<param name="Name">What it holds.</param>', "").replace(
            '<member name="P:Kiln.Box`1.Name"><summary>What it holds.</summary></member>\n', "")
        self.assertEqual(self.repo(xml=xml).check()[0], ["Kiln.Api: Kiln.Box`1.Name"])

    def test_no_xml_for_an_assembly_with_a_record_is_an_error(self):
        with self.assertRaises(rp.ai.Problem):
            self.repo(xml=None).check()

    def test_an_assembly_without_records_needs_no_xml(self):
        r = self.repo(xml=None, api="#nullable enable\nKiln.Mods\nconst Kiln.Mods.ApiVersion = \"0.2.15\" -> string!\n")
        self.assertEqual(r.check(), ([], 0, 0))

    def test_main_exit_codes(self):
        r = self.repo()
        base = ["--tree", str(r.root)]
        with contextlib.redirect_stdout(io.StringIO()) as out:
            self.assertEqual(rp.main(base + ["--xml", str(r.xml)]), 0)
        self.assertIn("all documented", out.getvalue())
        (r.xml / "Kiln.Api.xml").write_text(XML.replace('<param name="Right">The right one.</param>', "").replace(
            '<member name="P:Kiln.Pair.Right"><summary>The right one.</summary></member>\n', ""))
        with contextlib.redirect_stdout(io.StringIO()) as out:
            self.assertEqual(rp.main(base + ["--xml", str(r.xml)]), 1)
        self.assertIn("Kiln.Pair.Right", out.getvalue())
        (r.xml / "Kiln.Api.xml").unlink()
        with contextlib.redirect_stderr(io.StringIO()):
            self.assertEqual(rp.main(base + ["--xml", str(r.xml)]), 2)          # no XML for Kiln.Api
            with mock.patch.dict(os.environ):
                os.environ.pop("KILN_API_XML", None)
                self.assertEqual(rp.main(base), 2)                              # no --xml at all


@unittest.skipUnless(os.environ.get("KILN_API_XML"), "KILN_API_XML=<xml dir> checks this repository")
class ThisRepository(unittest.TestCase):
    def test_every_positional_property_is_documented(self):
        missing, records, props = rp.check(HERE.parent.parent, os.environ["KILN_API_XML"])
        self.assertEqual(missing, [], f"{len(missing)} of {props} properties of {records} records")


if __name__ == "__main__":
    unittest.main()
