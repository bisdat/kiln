#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Tests for the API history ledger (managed/api-history.tsv; tools/release/lib/apihistory.py, called through
tools/release/api-gate.sh history, and promote's append): the first release and mod API version of every entry from the
release tags (managed lines, native table rows), the `<=` of a record that begins late, experimental and removed lines,
--check (missing, extra, wrong, unrecorded, order, format), promote's append (its release name, the native rows of the
released commit, nothing written on a refusal, idempotence) and --append.

  python3 -I tools/release/test_apihistory.py
Every case builds a throwaway git repository under TMPDIR (tags are made only there) and never touches this one.
"""
import importlib.util
import os
from pathlib import Path
import subprocess
import unittest

HERE = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location("test_apigate", HERE / "test_apigate.py")
tag = importlib.util.module_from_spec(spec)
spec.loader.exec_module(tag)          # its Repo, api() and run() helpers; its cases are not run from here
ag = tag.ag
ah = ag.apihistory

API, GAME, SVC = tag.API, tag.GAME, tag.SVC
FOO = "native/core/abi/services/kiln_foo.h"
BAR = "native/core/abi/services/kiln_bar.h"
LEDGER = "managed/api-history.tsv"


def mods(v):
    return {ag.MODS_CS: f'    public const string ApiVersion = "{v}";\n'}


def const(v):
    return f'const Kiln.Mods.ApiVersion = "{v}" -> string!'


def header(table, *rows):
    body = "".join(f"    int (KILN_CALL *{r})(void);   /* the {r} row */\n" for r in rows)
    return (f"/* KILN_CALL *outside) is in a comment */\ntypedef struct {table} {{\n"
            f"    /* int (KILN_CALL *notarow)(void); */\n{body}    // int (KILN_CALL *nor)(void);\n}} {table};\n")


def ledger(text):
    """{(kind, project, entry): (release, mod_api)} of a ledger text, which must be sound."""
    rows, problems = ah.parse(text)
    assert not problems, problems
    return {tuple(r[:3]): (r[3], r[4]) for r in rows}


class History(tag.Base):
    """A repository with releases: dev.1 (no record), dev.2 (the record begins), dev.3, dev.4 (a beta before it), and
    work for dev.5 at HEAD."""

    def setUp(self):
        super().setUp()
        r = self.r
        r.commit({"README": "x", f"{API}/Kiln.Api.csproj": "<Project/>", f"{GAME}/Kiln.Game.csproj": "<Project/>",
                  FOO: header("KilnFooApi", "a", "b"), **mods("0.2.0")}, tag="v0.2.0-dev.1")
        r.commit({**self.project(API, ["Kiln.A", "Kiln.A.X() -> void", const("0.2.1")]),
                  **self.project(SVC, ["Kiln.Chat", "Kiln.Chat.Say() -> void"]),
                  FOO: header("KilnFooApi", "a", "b", "c"), BAR: header("KilnBarApiV1", "p"), **mods("0.2.1")},
                 tag="v0.2.0-dev.2")
        r.commit({**self.project(API, ["Kiln.A", "Kiln.A.X() -> void", const("0.2.2")], ["Kiln.A.New() -> void"]),
                  **self.project(SVC, ["Kiln.Chat", "Kiln.Chat.Say() -> void"]),
                  **self.project(GAME, ["Kiln.G"]),
                  FOO: header("KilnFooApi", "a", "b", "c", "d"), **mods("0.2.2")}, tag="v0.2.0-dev.3")
        r.commit({**self.project(API, ["Kiln.A", "Kiln.A.X() -> void", "Kiln.A.New() -> void", const("0.2.2")])})
        r.commit({**self.project(API, ["Kiln.A", "Kiln.A.X() -> void", "Kiln.A.New() -> void", const("0.2.3")],
                                 ["Kiln.A.Beta() -> void"]), **mods("0.2.3")}, tag="v0.2.0-dev.4-beta.1")
        r.commit({**self.project(API, ["Kiln.A", "Kiln.A.X() -> void", "Kiln.A.New() -> void", const("0.2.3")],
                                 ["Kiln.A.Beta() -> void", "[KILNX001]Kiln.A.Lab() -> void", "*REMOVED*Kiln.A.X() -> void",
                                  "[KILNX002]Kiln.A.X() -> void"])}, tag="v0.2.0-dev.4")
        r.git("tag", "updates-dev")
        r.commit({**self.project(API, ["Kiln.A", "Kiln.A.New() -> void", "Kiln.A.Beta() -> void",
                                       "[KILNX001]Kiln.A.Lab() -> void", "[KILNX002]Kiln.A.X() -> void", const("0.2.3")])})
        # work for dev.5
        r.commit({**self.project(API, ["Kiln.A", "Kiln.A.New() -> void", "Kiln.A.Beta() -> void",
                                       "[KILNX001]Kiln.A.Lab() -> void", "[KILNX002]Kiln.A.X() -> void", const("0.2.4")],
                                 ["Kiln.A.Next() -> void"]),
                  FOO: header("KilnFooApi", "a", "b", "c", "d", "e"), **mods("0.2.4")})

    def cli(self, *argv):
        return tag.run(self.r, "history", *argv)

    def seed(self):
        code, out = self.cli("--seed")
        self.assertEqual(0, code, out)
        return self.r.read(LEDGER)


class Seed(History):
    def test_every_entry_has_the_first_release_and_mod_api_of_its_tag(self):
        rows = ledger(self.seed())
        m = lambda p, e: rows.get(("managed", p, e))
        # the record begins at dev.2 for a project whose files were there at dev.1: the entry may be older
        self.assertEqual(("<=0.2.0-dev.2", "<=0.2.1"), m("Kiln.Api", "Kiln.A"))
        self.assertEqual(("<=0.2.0-dev.2", "<=0.2.1"), m("Kiln.Api", "Kiln.A.X() -> void"))
        # a project that did not exist at the tag before: exact
        self.assertEqual(("0.2.0-dev.2", "0.2.1"), m("Kiln.Chat", "Kiln.Chat.Say() -> void"))
        # a project gated later whose folder was there before: its record begins then
        self.assertEqual(("<=0.2.0-dev.3", "<=0.2.2"), m("Kiln.Game", "Kiln.G"))
        # still in Unshipped.txt at its tag: shipped with it
        self.assertEqual(("0.2.0-dev.3", "0.2.2"), m("Kiln.Api", "Kiln.A.New() -> void"))
        # in a beta's Unshipped.txt first: the beta is no release
        self.assertEqual(("0.2.0-dev.4", "0.2.3"), m("Kiln.Api", "Kiln.A.Beta() -> void"))

    def test_experimental_and_removed_lines_keep_their_rows(self):
        rows = ledger(self.seed())
        self.assertEqual(("0.2.0-dev.4", "0.2.3"), rows[("managed", "Kiln.Api", "[KILNX001]Kiln.A.Lab() -> void")])
        self.assertEqual(("0.2.0-dev.4", "0.2.3"), rows[("managed", "Kiln.Api", "[KILNX002]Kiln.A.X() -> void")])
        # A.X was shipped at dev.2 and is an experimental member now: its first line keeps the old date
        self.assertEqual(("<=0.2.0-dev.2", "<=0.2.1"), rows[("managed", "Kiln.Api", "Kiln.A.X() -> void")])

    def test_a_line_removed_in_the_record_that_has_it_never_shipped(self):
        r = self.r
        r.commit({**self.project(API, ["Kiln.A", "Kiln.A.Gone() -> void"], ["*REMOVED*Kiln.A.Gone() -> void"]),
                  **mods("0.2.4")}, tag="v0.2.0-dev.4.1")
        rows = ledger(self.seed())
        self.assertNotIn(("managed", "Kiln.Api", "Kiln.A.Gone() -> void"), rows)

    def test_unshipped_lines_at_head_and_the_api_version_const_are_not_in_the_ledger(self):
        entries = {k[2] for k in ledger(self.seed())}
        self.assertNotIn("Kiln.A.Next() -> void", entries)
        self.assertFalse([e for e in entries if e.startswith("const Kiln.Mods.ApiVersion")])

    def test_native_rows_are_dated_by_their_header(self):
        rows = ledger(self.seed())
        n = lambda e: rows.get(("native", e.split(":")[0], e))
        self.assertEqual(("<=0.2.0-dev.1", "<=0.2.0"), n("services/kiln_foo.h:KilnFooApi:a"))   # at the first release tag
        self.assertEqual(("0.2.0-dev.2", "0.2.1"), n("services/kiln_foo.h:KilnFooApi:c"))
        self.assertEqual(("0.2.0-dev.3", "0.2.2"), n("services/kiln_foo.h:KilnFooApi:d"))
        # a header that did not exist at the tag before
        self.assertEqual(("0.2.0-dev.2", "0.2.1"), n("services/kiln_bar.h:KilnBarApiV1:p"))
        # row e is HEAD's work, and a row in a comment is none
        self.assertIsNone(n("services/kiln_foo.h:KilnFooApi:e"))
        self.assertFalse([k for k in rows if k[2].endswith(":notarow")])

    def test_the_file_is_canonical_sorted_and_the_same_every_time(self):
        text = self.seed()
        lines = text.split("\n")
        self.assertEqual("kind\tproject\tentry\trelease\tmod_api", lines[0])
        self.assertTrue(text.endswith("\n") and "\r" not in text)
        self.assertEqual([], ah.parse(text)[1])
        self.assertEqual(text, self.seed())
        self.assertLess(text.index("\tKiln.Api\t"), text.index("\tKiln.Chat\t"))
        self.assertLess(text.index("managed\t"), text.index("native\t"))

    def test_seed_keeps_the_rows_of_a_release_without_a_tag_and_drops_those_of_a_tagged_one(self):
        text = self.seed()
        rows = ah.parse(text)[0]
        fake = [("managed", "Kiln.Api", "Kiln.A.Next() -> void", "0.2.0-dev.5", "0.2.4"),
                ("managed", "Kiln.Api", "Kiln.A.Ghost() -> void", "0.2.0-dev.4", "0.2.3")]
        self.r.put({LEDGER: ah.render(rows + fake)})
        got = ledger(self.seed())
        self.assertEqual(("0.2.0-dev.5", "0.2.4"), got[("managed", "Kiln.Api", "Kiln.A.Next() -> void")])
        self.assertNotIn(("managed", "Kiln.Api", "Kiln.A.Ghost() -> void"), got)

    def test_without_release_tags_it_says_so(self):
        import tempfile
        tmp = tempfile.mkdtemp(prefix="apihistory-")
        self.addCleanup(__import__("shutil").rmtree, tmp, True)
        bare = tag.Repo(Path(tmp))
        bare.commit({"README": "x"})
        code, out = tag.run(bare, "history", "--seed")
        self.assertEqual(2, code)
        self.assertIn("no release tag", out)

    def test_a_record_with_no_released_api_gives_an_empty_ledger(self):
        import tempfile
        tmp = tempfile.mkdtemp(prefix="apihistory-")
        self.addCleanup(__import__("shutil").rmtree, tmp, True)
        r = tag.Repo(Path(tmp))
        r.commit({"README": "x", **mods("0.2.0")}, tag="v0.2.0-dev.1")
        code, out = tag.run(r, "history", "--seed")
        self.assertEqual(0, code, out)
        self.assertEqual("kind\tproject\tentry\trelease\tmod_api\n", r.read(LEDGER))

    def test_release_order_is_semver_with_numbered_dev_builds(self):
        names = ["0.2.0-dev.30.5", "0.2.0", "0.2.0-dev.9", "0.2.0-dev.29.10", "0.2.0-dev.29.9", "0.2.0-dev.10", "0.1.9",
                 "0.2.0-dev.29", "0.2.0-dev.30"]
        self.assertEqual(["0.1.9", "0.2.0-dev.9", "0.2.0-dev.10", "0.2.0-dev.29", "0.2.0-dev.29.9", "0.2.0-dev.29.10",
                          "0.2.0-dev.30", "0.2.0-dev.30.5", "0.2.0"], sorted(names, key=ah.release_key))


class Check(History):
    def setUp(self):
        super().setUp()
        self.seed()

    def check(self):
        return self.cli("--check")

    def edit(self, fn):
        rows = ah.parse(self.r.read(LEDGER))[0]
        self.r.put({LEDGER: ah.render(fn(rows))})

    def test_a_fresh_ledger_is_sound(self):
        code, out = self.check()
        self.assertEqual(0, code, out)
        self.assertIn("is sound", out)

    def test_a_missing_ledger_is_named(self):
        self.r.put({LEDGER: None})
        code, out = self.check()
        self.assertEqual(1, code, out)
        self.assertIn("does not exist", out)

    def test_a_missing_row_is_named(self):
        self.edit(lambda rows: [r for r in rows if r[2] != "Kiln.A.New() -> void"])
        code, out = self.check()
        self.assertEqual(1, code, out)
        self.assertIn("missing: managed Kiln.Api: Kiln.A.New() -> void (0.2.0-dev.3 per the tags)", out)

    def test_a_missing_native_row_is_named(self):
        self.edit(lambda rows: [r for r in rows if not r[2].endswith(":KilnFooApi:d")])
        code, out = self.check()
        self.assertEqual(1, code, out)
        self.assertIn("missing: native services/kiln_foo.h: services/kiln_foo.h:KilnFooApi:d", out)

    def test_an_extra_row_is_named(self):
        self.edit(lambda rows: rows + [("managed", "Kiln.Api", "Kiln.A.Never() -> void", "0.2.0-dev.3", "0.2.2")])
        code, out = self.check()
        self.assertEqual(1, code, out)
        self.assertIn("extra: managed Kiln.Api: Kiln.A.Never() -> void: the tag of 0.2.0-dev.3 does not have it", out)

    def test_a_wrong_release_or_mod_api_is_named(self):
        self.edit(lambda rows: [(*r[:3], "0.2.0-dev.2", r[4]) if r[2] == "Kiln.A.New() -> void" else
                                (*r[:4], "0.2.9") if r[2] == "Kiln.A.Beta() -> void" else r for r in rows])
        code, out = self.check()
        self.assertEqual(1, code, out)
        self.assertIn("wrong: managed Kiln.Api: Kiln.A.New() -> void: the tags say 0.2.0-dev.3 / 0.2.2, the ledger "
                      "0.2.0-dev.2 / 0.2.2", out)
        self.assertIn("wrong: managed Kiln.Api: Kiln.A.Beta() -> void: the tags say 0.2.0-dev.4 / 0.2.3, the ledger "
                      "0.2.0-dev.4 / 0.2.9", out)

    def test_a_row_for_a_release_without_a_tag_that_is_not_in_the_record_is_extra(self):
        self.edit(lambda rows: rows + [("managed", "Kiln.Api", "Kiln.A.Next() -> void", "0.2.0-dev.5", "0.2.4")])
        code, out = self.check()
        self.assertEqual(1, code, out)
        self.assertIn("extra: managed Kiln.Api: Kiln.A.Next() -> void: not in the Shipped.txt of Kiln.Api", out)

    def test_a_shipped_line_without_a_row_is_unrecorded(self):
        p = f"{API}/PublicAPI.Shipped.txt"
        self.r.put({p: self.r.read(p) + "Kiln.A.Sneaked() -> void\n"})
        code, out = self.check()
        self.assertEqual(1, code, out)
        self.assertIn("unrecorded: managed Kiln.Api: Kiln.A.Sneaked() -> void", out)

    def test_an_unshipped_line_needs_no_row(self):
        self.assertEqual(0, self.check()[0])      # Kiln.A.Next() is in Unshipped.txt at HEAD

    def test_the_format_is_checked(self):
        text = self.r.read(LEDGER)
        for bad, why in ((text.replace("\n", "\r\n"), "carriage returns"),
                         (text.replace("kind\t", "type\t", 1), "not the header"),
                         (text.rstrip("\n"), "does not end with a newline"),
                         (text + "managed\tKiln.Api\tKiln.A\t0.2.0-dev.9\t0.2.1\n", "repeat"),
                         (text + "managed\tKiln.Api\n", "filled cells"),
                         (text.replace("\t0.2.0-dev.3\t", "\tdev three\t", 1), "not a version")):
            with self.subTest(why):
                self.r.put({LEDGER: bad})
                code, out = self.check()
                self.assertEqual(1, code, out)
                self.assertIn(why, out)

    def test_rows_out_of_order_are_named(self):
        lines = self.r.read(LEDGER).split("\n")
        lines[1], lines[2] = lines[2], lines[1]
        self.r.put({LEDGER: "\n".join(lines)})
        code, out = self.check()
        self.assertEqual(1, code, out)
        self.assertIn("not sorted", out)

    def test_the_working_tree_is_the_record_not_head(self):
        p = f"{API}/PublicAPI.Shipped.txt"
        self.r.put({p: self.r.read(p) + "Kiln.A.Sneaked() -> void\n"})
        self.assertEqual(1, self.check()[0])
        self.r.git("checkout", "--", p)
        self.assertEqual(0, self.check()[0])

    def test_api_gate_sh_checks(self):
        r = subprocess.run([str(HERE / "api-gate.sh"), "history", "--check"], capture_output=True, text=True,
                           env={**os.environ, **tag.GIT_ENV, "KILN_API_GATE_REPO": str(self.r.root)})
        self.assertEqual(0, r.returncode, r.stdout + r.stderr)
        self.assertIn("is sound", r.stdout)


class Promote(History):
    def setUp(self):
        super().setUp()
        self.seed()
        self.r.commit({LEDGER: self.r.read(LEDGER)})     # the ledger of the release before, committed
        self.head = self.r.git("rev-parse", "HEAD").strip()

    def promote(self, *argv):
        return tag.run(self.r, "promote", *argv)

    def test_promote_appends_the_moved_lines_and_the_new_native_rows(self):
        code, out = self.promote("--release", "0.2.0-dev.5")
        self.assertEqual(0, code, out)
        self.assertIn("managed/api-history.tsv: 1 managed and 1 native rows for 0.2.0-dev.5 (mod API 0.2.4)", out)
        rows = ledger(self.r.read(LEDGER))
        self.assertEqual(("0.2.0-dev.5", "0.2.4"), rows[("managed", "Kiln.Api", "Kiln.A.Next() -> void")])
        self.assertEqual(("0.2.0-dev.5", "0.2.4"), rows[("native", "services/kiln_foo.h",
                                                         "services/kiln_foo.h:KilnFooApi:e")])
        self.assertIn("Kiln.A.Next() -> void\n", self.r.read(f"{API}/PublicAPI.Shipped.txt"))
        code, out = self.cli_check()
        self.assertEqual(0, code, out)
        self.assertIn("0.2.0-dev.5, which has no tag yet", out)

    def cli_check(self):
        return tag.run(self.r, "history", "--check")

    def test_the_ledger_after_promote_is_what_the_tag_gives(self):
        self.assertEqual(0, self.promote("--release", "0.2.0-dev.5")[0])
        promoted = self.r.read(LEDGER)
        self.r.git("tag", "v0.2.0-dev.5", self.head)         # the tag is made on the released commit, after the release
        code, out = self.cli_check()
        self.assertEqual(0, code, out)
        self.assertNotIn("no tag yet", out)
        self.assertEqual(0, tag.run(self.r, "history", "--seed")[0])
        self.assertEqual(promoted, self.r.read(LEDGER), "promote's rows and the seed's rows for a tag are the same")

    def test_the_release_is_the_tag_on_head_when_it_is_not_given(self):
        self.r.git("tag", "v0.2.0-dev.5")
        code, out = self.promote()
        self.assertEqual(0, code, out)
        self.assertIn("for 0.2.0-dev.5", out)

    def test_the_release_is_the_tag_on_the_released_commit_with_if_same_as(self):
        self.r.git("tag", "v0.2.0-dev.5")
        self.r.commit({"README": "later"})
        code, out = self.promote("--if-same-as", "v0.2.0-dev.5")
        self.assertEqual(0, code, out)
        self.assertIn("for 0.2.0-dev.5", out)

    def test_no_release_name_is_a_refusal_and_nothing_is_written(self):
        shipped, ledger_text = self.r.read(f"{API}/PublicAPI.Shipped.txt"), self.r.read(LEDGER)
        code, out = self.promote()
        self.assertEqual(1, code, out)
        self.assertIn("give --release NAME", out)
        self.assertEqual(shipped, self.r.read(f"{API}/PublicAPI.Shipped.txt"))
        self.assertEqual(ledger_text, self.r.read(LEDGER))

    def test_a_bad_release_name_is_a_refusal(self):
        code, out = self.promote("--release", "dev five")
        self.assertEqual(1, code, out)
        self.assertIn("not a release name", out)

    def test_an_unsound_ledger_is_a_refusal_and_nothing_is_written(self):
        self.r.put({LEDGER: self.r.read(LEDGER).replace("\n", "\r\n")})
        shipped = self.r.read(f"{API}/PublicAPI.Shipped.txt")
        code, out = self.promote("--release", "0.2.0-dev.5")
        self.assertEqual(1, code, out)
        self.assertIn("is not sound", out)
        self.assertEqual(shipped, self.r.read(f"{API}/PublicAPI.Shipped.txt"))

    def test_running_it_twice_changes_nothing(self):
        self.assertEqual(0, self.promote("--release", "0.2.0-dev.5")[0])
        once = self.r.read(LEDGER)
        code, out = self.promote("--release", "0.2.0-dev.5")
        self.assertEqual(0, code, out)
        self.assertIn("nothing pending", out)
        self.assertEqual(once, self.r.read(LEDGER))

    def test_a_release_with_only_new_native_rows_still_gets_its_rows(self):
        self.r.commit({f"{API}/PublicAPI.Unshipped.txt": tag.api()})      # no pending managed lines
        code, out = self.promote("--release", "0.2.0-dev.5")
        self.assertEqual(0, code, out)
        self.assertIn("0 managed and 1 native rows", out)

    def test_the_native_rows_and_mod_api_are_those_of_the_released_commit(self):
        self.r.commit({FOO: header("KilnFooApi", "a", "b", "c", "d", "e", "f"), **mods("0.2.5")})   # later work
        code, out = self.promote("--if-same-as", self.head, "--release", "0.2.0-dev.5")
        self.assertEqual(0, code, out)
        rows = ledger(self.r.read(LEDGER))
        self.assertEqual(("0.2.0-dev.5", "0.2.4"), rows[("managed", "Kiln.Api", "Kiln.A.Next() -> void")])
        self.assertIn(("native", "services/kiln_foo.h", "services/kiln_foo.h:KilnFooApi:e"), rows)
        self.assertNotIn(("native", "services/kiln_foo.h", "services/kiln_foo.h:KilnFooApi:f"), rows)

    def test_the_api_version_const_gets_no_row(self):
        self.r.commit({f"{API}/PublicAPI.Unshipped.txt": tag.api("Kiln.A.Next() -> void", const("0.2.5"))})
        self.assertEqual(0, self.promote("--release", "0.2.0-dev.5")[0])
        self.assertFalse([k for k in ledger(self.r.read(LEDGER)) if k[2].startswith("const Kiln.Mods.ApiVersion")])

    def test_a_forwarded_line_is_a_new_line_dated_by_the_move(self):
        self.r.commit({f"{API}/PublicAPI.Unshipped.txt": tag.api(
            "Kiln.A.New() -> void (forwarded, contained in Kiln.Game)", "*REMOVED*Kiln.A.New() -> void"),
            f"{GAME}/PublicAPI.Shipped.txt": tag.api("Kiln.G", "Kiln.A.New() -> void")})
        code, out = self.promote("--release", "0.2.0-dev.5")
        self.assertEqual(0, code, out)
        rows = ledger(self.r.read(LEDGER))
        self.assertEqual(("0.2.0-dev.5", "0.2.4"), rows[("managed", "Kiln.Api",
                                                         "Kiln.A.New() -> void (forwarded, contained in Kiln.Game)")])
        self.assertEqual(("0.2.0-dev.3", "0.2.2"), rows[("managed", "Kiln.Api", "Kiln.A.New() -> void")])

    def test_without_a_ledger_promote_works_as_before_and_says_so(self):
        self.r.put({LEDGER: None})
        code, out = self.promote()
        self.assertEqual(0, code, out)
        self.assertIn("the API history ledger is not updated", out)
        self.assertIn("Kiln.A.Next() -> void\n", self.r.read(f"{API}/PublicAPI.Shipped.txt"))


class Append(History):
    def test_append_gives_rows_to_shipped_lines_and_native_rows_that_have_none(self):
        self.seed()
        p = f"{API}/PublicAPI.Shipped.txt"
        self.r.put({p: self.r.read(p) + "Kiln.A.Late() -> void\n"})
        self.assertEqual(1, self.cli("--check")[0])
        code, out = self.cli("--append", "--release", "0.2.0-dev.5")
        self.assertEqual(0, code, out)
        self.assertIn("1 managed and 1 native rows appended for 0.2.0-dev.5 (mod API 0.2.4)", out)
        rows = ledger(self.r.read(LEDGER))
        self.assertEqual(("0.2.0-dev.5", "0.2.4"), rows[("managed", "Kiln.Api", "Kiln.A.Late() -> void")])
        code, out = self.cli("--append", "--release", "0.2.0-dev.5")
        self.assertIn("nothing to append", out)
        self.assertEqual(0, self.cli("--check")[0])

    def test_append_takes_a_mod_api_and_needs_a_release(self):
        self.seed()
        code, out = self.cli("--append")
        self.assertEqual(2, code, out)
        code, out = self.cli("--append", "--release", "v0.2.0-dev.5", "--mod-api", "0.2.9")
        self.assertEqual(0, code, out)
        self.assertIn("for 0.2.0-dev.5 (mod API 0.2.9)", out)

    def test_append_without_a_ledger_says_so(self):
        code, out = self.cli("--append", "--release", "0.2.0-dev.5")
        self.assertEqual(2, code, out)
        self.assertIn("api-gate.sh history --seed", out)


if __name__ == "__main__":
    unittest.main()
