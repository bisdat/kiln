#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""since.py: the "Since mod API" line of a member, from the ledger and PublicAPI.Unshipped.txt.

  python3 -I tools/docs-site/test_since.py
"""
import importlib.util
import os
from pathlib import Path
import tempfile
import unittest
from unittest import mock

HERE = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location("since", HERE / "since.py")
since = importlib.util.module_from_spec(spec)
spec.loader.exec_module(since)

HEADER = "kind\tproject\tentry\trelease\tmod_api\n"


def ledger_row(project, entry, release, mod_api, kind="managed"):
    return f"{kind}\t{project}\t{entry}\t{release}\t{mod_api}\n"


def make_repo(root, rows, unshipped=None, api_version="0.2.15"):
    root = Path(root)
    (root / "managed/Kiln.Api").mkdir(parents=True, exist_ok=True)
    (root / "managed/Kiln.Api/Mods.cs").write_text(f'public static class Mods {{ public const string ApiVersion = "{api_version}"; }}\n')
    (root / "managed/api-history.tsv").write_text(HEADER + "".join(rows))
    for project, lines in (unshipped or {}).items():
        d = root / "managed/services" / project
        d.mkdir(parents=True, exist_ok=True)
        (d / "PublicAPI.Unshipped.txt").write_text("#nullable enable\n" + "\n".join(lines) + "\n")
    return root


class Lines(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.env = mock.patch.dict(os.environ, {}, clear=False)
        self.env.start()
        self.addCleanup(self.env.stop)
        for name in ("KILN_DOCS_RELEASE", "KILN_SDK_VERSION"):
            os.environ.pop(name, None)

    def repo(self, rows, unshipped=None):
        return since.Since(make_repo(self.tmp.name, rows, unshipped))

    def test_a_shipped_member_names_its_mod_api_and_release(self):
        s = self.repo([ledger_row("Kiln.Items", "Kiln.Items.Give(Kiln.EntityId who, string! id) -> bool", "0.2.0-dev.30", "0.2.12")])
        self.assertEqual(s.text("M:Kiln.Items.Give(Kiln.EntityId,System.String)"), "Since mod API 0.2.12 (0.2.0-dev.30)")

    def test_the_first_record_is_at_most(self):
        s = self.repo([ledger_row("Kiln.Chat", "Kiln.Chat", "<=0.2.0-dev.26", "<=0.2.1")])
        self.assertEqual(s.text("T:Kiln.Chat"), "Since mod API 0.2.1 or earlier (0.2.0-dev.26 or earlier)")

    def test_an_unshipped_line_is_the_next_release(self):
        s = self.repo([], {"Kiln.Build": ["static Kiln.Build.BlockAt(Kiln.WorldPosition position) -> Kiln.BlockQueryResult!"]})
        self.assertEqual(s.text("M:Kiln.Build.BlockAt(Kiln.WorldPosition)"), "Since mod API 0.2.15 (next release)")

    def test_a_build_that_knows_its_release_names_it(self):
        unshipped = {"Kiln.Build": ["static Kiln.Build.BlockAt(Kiln.WorldPosition position) -> Kiln.BlockQueryResult!"]}
        for name in ("KILN_DOCS_RELEASE", "KILN_SDK_VERSION"):
            with self.subTest(name), mock.patch.dict(os.environ, {name: "0.2.0-dev.31"}):
                self.assertEqual(self.repo([], unshipped).text("M:Kiln.Build.BlockAt(Kiln.WorldPosition)"),
                                 "Since mod API 0.2.15 (0.2.0-dev.31)")

    def test_a_local_pack_version_is_no_release(self):
        with mock.patch.dict(os.environ, {"KILN_SDK_VERSION": "0.2.0-dev.local.20261122.200000"}):
            self.assertIsNone(since.building_release())
        with mock.patch.dict(os.environ, {"KILN_SDK_VERSION": "v0.2.0-dev.30.6"}):
            self.assertEqual(since.building_release(), "0.2.0-dev.30.6")

    def test_a_member_two_lines_describe_has_the_earliest(self):
        # a property's get and set, and a type that moved out of an assembly and was forwarded
        s = self.repo([
            ledger_row("Kiln.Foo", "Kiln.Foo.Bar.set -> void", "0.2.0-dev.30", "0.2.12"),
            ledger_row("Kiln.Foo", "Kiln.Foo.Bar.get -> int", "0.2.0-dev.28", "0.2.3"),
            ledger_row("Kiln.Old", "Kiln.Moved", "0.2.0-dev.27", "0.2.2"),
            ledger_row("Kiln.Api", "Kiln.Moved", "0.2.0-dev.29.11", "0.2.5"),
            ledger_row("Kiln.Old", "Kiln.Moved (forwarded, contained in Kiln.Api)", "0.2.0-dev.29.11", "0.2.5"),
        ])
        self.assertEqual(s.text("P:Kiln.Foo.Bar"), "Since mod API 0.2.3 (0.2.0-dev.28)")
        self.assertEqual(s.text("T:Kiln.Moved"), "Since mod API 0.2.2 (0.2.0-dev.27)")

    def test_dev_numbers_are_numbers_and_at_most_comes_first(self):
        s = self.repo([
            ledger_row("Kiln.A", "Kiln.A.M() -> void", "0.2.0-dev.30", "0.2.12"),
            ledger_row("Kiln.A", "Kiln.A.M() -> void", "0.2.0-dev.9", "0.2.0"),
            ledger_row("Kiln.B", "Kiln.B", "0.2.0-dev.26", "0.2.1"),
            ledger_row("Kiln.B", "Kiln.B", "<=0.2.0-dev.26", "<=0.2.1"),
        ])
        self.assertEqual(s.text("M:Kiln.A.M"), "Since mod API 0.2.0 (0.2.0-dev.9)")
        self.assertEqual(s.text("T:Kiln.B"), "Since mod API 0.2.1 or earlier (0.2.0-dev.26 or earlier)")

    def test_a_shipped_row_wins_over_an_unshipped_line(self):
        # promote moved the line and wrote its row, but the release has no tag yet
        s = self.repo([ledger_row("Kiln.Build", "static Kiln.Build.Dig() -> void", "0.2.0-dev.31", "0.2.15")],
                      {"Kiln.Build": ["static Kiln.Build.Dig() -> void"]})
        self.assertEqual(s.text("M:Kiln.Build.Dig"), "Since mod API 0.2.15 (0.2.0-dev.31)")

    def test_native_rows_and_removed_lines_are_not_members(self):
        s = self.repo([ledger_row("services/kiln_ui.h", "services/kiln_ui.h:KilnUiApi:text", "<=0.2.0-dev.10", "<=0.2.0", "native")],
                      {"Kiln.Build": ["*REMOVED*static Kiln.Build.Gone() -> void"]})
        self.assertEqual(s.entries, {})

    def test_an_unknown_member_has_no_line(self):
        self.assertIsNone(self.repo([]).text("M:Kiln.Nothing.Here"))

    def test_no_ledger_is_an_error_not_a_silent_site_without_lines(self):
        with self.assertRaises(FileNotFoundError):
            since.Since(self.tmp.name)


if __name__ == "__main__":
    unittest.main()
