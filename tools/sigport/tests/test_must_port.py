# SPDX-License-Identifier: MIT
"""Must-port names (tools/sigport/must_port.json): the list, and the re-port and check flows failing when a listed name
is dropped or does not resolve: build_record.py check, must_port.py check, port.py and sigdb_check.py.

    python3 -m unittest discover -s tools/sigport/tests -v
"""
import copy
import io
import json
import sys
import tempfile
import unittest
from contextlib import redirect_stderr, redirect_stdout
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent))
sys.path.insert(0, str(HERE))

import build_record as br  # noqa: E402
import must_port as mp  # noqa: E402
import sigdb_check  # noqa: E402
from test_build_record import EXTRA, Synthetic, run  # noqa: E402
from test_sigport import SyntheticPort, build_pe, filler, target  # noqa: E402

CALLERS = ["build/RetSaveRegionsWorldSave", "build/RetSaveRegionsBaseZones", "build/RetSaveRegionsBaseQuery"]


def quiet(fn, *args):
    out, err = io.StringIO(), io.StringIO()
    with redirect_stdout(out), redirect_stderr(err):
        rc = fn(*args)
    return rc, out.getvalue(), err.getvalue()


class TheList(unittest.TestCase):
    def test_the_save_region_callers_come_first_on_both_sides(self):
        entries = mp.load()
        self.assertEqual([e["name"] for e in entries[:3]], CALLERS)
        for e in entries[:3]:
            self.assertEqual((e["section"], e["sides"]), ("symbols", ["server", "client"]))
            self.assertIn("kiln.build", e["why"])
        # only the world save's call site stops a port; the base zones' and the base query's only warn
        self.assertEqual([e["level"] for e in entries[:3]], ["fail", "warn", "warn"])

    def test_the_level_defaults_to_fail(self):
        with tempfile.TemporaryDirectory() as d:
            p = Path(d) / "list.json"
            p.write_text(json.dumps({"format": 1, "names": [{"name": "a/B", "why": "x"},
                                                             {"name": "a/C", "level": "warn", "why": "y"}]}))
            self.assertEqual([e["level"] for e in mp.load(p)], ["fail", "warn"])
            self.assertEqual(mp.label(mp.load(p)[1]), "symbol a/C (warning only)")

    def test_a_malformed_list_is_refused(self):
        cases = {
            "no why": {"format": 1, "names": [{"name": "a/B"}]},
            "unknown side": {"format": 1, "names": [{"name": "a/B", "sides": ["mac"], "why": "x"}]},
            "listed twice": {"format": 1, "names": [{"name": "a/B", "why": "x"}, {"name": "a/B", "why": "y"}]},
            "bad section": {"format": 1, "names": [{"name": "a/B", "section": "probes", "why": "x"}]},
            "bad level": {"format": 1, "names": [{"name": "a/B", "level": "error", "why": "x"}]},
            "no format": {"names": []},
        }
        with tempfile.TemporaryDirectory() as d:
            for what, doc in cases.items():
                p = Path(d) / f"{len(what)}.json"
                p.write_text(json.dumps(doc))
                with self.assertRaises(mp.ListError, msg=what):
                    mp.load(p)
            p = Path(d) / "bad.json"
            p.write_text(json.dumps(cases["no why"]))
            rc, _out, err = quiet(mp.main, ["check", "--list", str(p)])
            self.assertEqual(rc, 2)
            self.assertIn("no why", err)


class Repository(unittest.TestCase):
    def test_every_listed_name_is_declared_and_resolved_on_the_newest_records(self):
        ok, lines = mp.check()
        self.assertTrue(ok, "\n".join(lines))
        self.assertTrue(all(line.startswith("ok ") for line in lines), lines)
        rc, out, _ = quiet(mp.main, ["check", "server", "client"])
        self.assertEqual(rc, 0, out)


class RecordCheck(Synthetic):
    """build_record.py check and must_port.py check on a fixture sigdb, build record and must-port list."""

    def setUp(self):
        super().setUp()
        self.list = self.d / "must_port.json"
        self.list.write_text(json.dumps({"format": 1, "names": [
            {"name": "extra/CallSite", "sides": ["server"], "why": "fixture: the caller Kiln tells apart"}]}))
        self.mp_args = ["--must-port", str(self.list)]

    def check_both(self):
        rc, out = run(["check", "server"] + self.common + self.mp_args)
        rc2, out2, _ = quiet(mp.main, ["check", "server", "--list", str(self.list), "--symbols", str(self.symbols),
                                       "--builds", str(self.builds)])
        return rc, out, rc2, out2

    def test_listed_names_declared_and_resolved_pass(self):
        self.new_record()
        rc, out, rc2, out2 = self.check_both()
        self.assertEqual((rc, rc2), (0, 0), out + out2)
        self.assertIn("1 must-port name(s) declared and resolved", out2)

    def test_a_listed_name_dropped_from_sigdb_fails(self):
        self.new_record()
        doc = copy.deepcopy(EXTRA)
        del doc["symbols"]["extra/CallSite"], doc["symbols"]["@extra/CallSite.fields"]
        self.write_extra(doc)
        # the record follows sigdb (the name is dropped from it too), so without a must-port list the check passes ...
        rc, out = run(["server", str(self.exe), "--refresh"] + self.common)
        self.assertEqual(rc, 0, out)
        self.assertIn("dropped  extra: symbol extra/CallSite", out)
        empty = self.d / "empty.json"
        empty.write_text(json.dumps({"format": 1, "names": []}))
        rc, out = run(["check", "server"] + self.common + ["--must-port", str(empty)])
        self.assertEqual(rc, 0, out)
        # ... and the must-port list is what catches it
        rc, out, rc2, out2 = self.check_both()
        self.assertEqual(rc, 1, out)
        self.assertIn("must-port symbol extra/CallSite: sigdb no longer declares it for the server. fixture: the "
                      "caller Kiln tells apart", out)
        self.assertEqual(rc2, 1, out2)
        self.assertIn("must-port symbol extra/CallSite: sigdb no longer declares it", out2)

    def test_a_listed_name_that_does_not_resolve_fails(self):
        doc = copy.deepcopy(EXTRA)
        doc["symbols"]["extra/CallSite"]["find"][0]["pattern"] = "DE AD BE EF 01 02"
        self.write_extra(doc)
        self.new_record()
        rc, out, rc2, out2 = self.check_both()
        self.assertEqual(rc, 1, out)
        self.assertIn("must-port symbol extra/CallSite: does not resolve on server", out)
        self.assertIn("0 matches", out)
        self.assertEqual(rc2, 1, out2)

    def test_a_warn_level_name_that_does_not_resolve_only_warns(self):
        self.list.write_text(json.dumps({"format": 1, "names": [
            {"name": "extra/CallSite", "sides": ["server"], "level": "warn", "why": "fixture: only a warning"}]}))
        doc = copy.deepcopy(EXTRA)
        doc["symbols"]["extra/CallSite"]["find"][0]["pattern"] = "DE AD BE EF 01 02"
        self.write_extra(doc)
        self.new_record()
        rc, out, rc2, out2 = self.check_both()
        self.assertEqual((rc, rc2), (0, 0), out + out2)
        for text in (out, out2):
            self.assertIn("warning: must-port symbol extra/CallSite (warning only): does not resolve on server", text)
            self.assertIn("fixture: only a warning", text)
            self.assertNotIn("FAIL", text)
        self.assertIn("0 of 1 must-port name(s) resolved, 1 warn-level one(s) not (a warning, not a failure)", out)

    def test_a_warn_level_name_dropped_from_sigdb_only_warns(self):
        self.list.write_text(json.dumps({"format": 1, "names": [
            {"name": "extra/CallSite", "sides": ["server"], "level": "warn", "why": "fixture: only a warning"}]}))
        self.new_record()
        doc = copy.deepcopy(EXTRA)
        del doc["symbols"]["extra/CallSite"], doc["symbols"]["@extra/CallSite.fields"]
        self.write_extra(doc)
        rc, out = run(["server", str(self.exe), "--refresh"] + self.common)
        self.assertEqual(rc, 0, out)
        rc, out, rc2, out2 = self.check_both()
        self.assertEqual((rc, rc2), (0, 0), out + out2)
        self.assertIn("warning: must-port symbol extra/CallSite (warning only): sigdb no longer declares it", out)
        self.assertIn("warning: must-port symbol extra/CallSite (warning only): sigdb no longer declares it", out2)

    def test_a_listed_name_for_the_other_side_is_not_checked_here(self):
        self.list.write_text(json.dumps({"format": 1, "names": [
            {"name": "client/Only", "sides": ["client"], "why": "fixture"}]}))
        self.new_record()
        rc, out, rc2, out2 = self.check_both()
        self.assertEqual((rc, rc2), (0, 0), out + out2)


class PortCheck(SyntheticPort):
    """port.py: a listed name the definitions declare must be ported, or the port is incomplete."""

    def must_port(self, *names):
        p = self.d / "must_port.json"
        p.write_text(json.dumps({"format": 1, "names": [{"name": n, "why": f"fixture {n}"} for n in names]}))
        return ["--must-port", str(p)]

    def test_a_listed_name_that_ports_passes(self):
        old, new, _t = self.exes(target(0xE179C0, 0x20, field_disp8=0x10))
        rc, r, out = self.run_port(old, new, *self.must_port("GetThing", "Caller", "Older/NotHere"))
        self.assertEqual(rc, 0, r["summary"])
        self.assertEqual(r["summary"]["must_port"], {"checked": ["GetThing", "Caller"], "not_declared": ["Older/NotHere"],
                                                     "failed": [], "warned": []})
        self.assertTrue(r["summary"]["complete"])

    def test_a_listed_name_lost_on_the_new_exe_fails_the_port(self):
        old, _n, _ = self.exes(target(0xE179C0, 0x20, field_disp8=0x10))
        other = self.d / "other.exe"
        build_pe(other, [(filler(k), True) for k in range(20, 30)])
        with redirect_stderr(io.StringIO()) as err:
            rc, r, out = self.run_port(old, other, *self.must_port("GetThing"))
        self.assertEqual(rc, 1)
        self.assertFalse(r["summary"]["complete"])
        failed = r["summary"]["must_port"]["failed"]
        self.assertEqual(len(failed), 1, failed)
        self.assertTrue(failed[0].startswith("must-port symbol GetThing: does not resolve on the new exe"), failed)
        self.assertIn("fixture GetThing", failed[0])
        self.assertIn("must-port symbol GetThing: does not resolve", err.getvalue())  # said with --quiet too
        self.assertIn("must-port symbol GetThing: does not resolve", (out / "report.txt").read_text())
        rec = json.loads(next((out / "builds/server").glob("*.json")).read_text())
        self.assertEqual(rec["sigport"]["must_port"], failed)
        self.assertFalse(rec["sigport"]["complete"])

    def test_a_warn_level_name_lost_on_the_new_exe_leaves_the_port_complete(self):
        old, _n, _ = self.exes(target(0xE179C0, 0x20, field_disp8=0x10))
        other = self.d / "other.exe"
        build_pe(other, [(filler(k), True) for k in range(20, 30)])
        p = self.d / "must_port.json"
        p.write_text(json.dumps({"format": 1, "names": [{"name": "GetThing", "level": "warn", "why": "fixture warn"}]}))
        with redirect_stderr(io.StringIO()) as err:
            rc, r, out = self.run_port(old, other, "--must-port", str(p))
        mpr = r["summary"]["must_port"]
        self.assertEqual(mpr["failed"], [])
        self.assertEqual(len(mpr["warned"]), 1, mpr)
        self.assertTrue(mpr["warned"][0].startswith("must-port symbol GetThing (warning only): does not resolve on the new exe"))
        self.assertIn("sigport: warning: must-port symbol GetThing (warning only)", err.getvalue())  # said with --quiet too
        self.assertIn("warning: must-port symbol GetThing (warning only)", (out / "report.txt").read_text())
        rec = json.loads(next((out / "builds/server").glob("*.json")).read_text())
        self.assertEqual((rec["sigport"]["must_port"], rec["sigport"]["must_port_warn"]), ([], mpr["warned"]))
        # GetThing itself is lost on this exe (it resolved on the old one), so the port is incomplete for that reason
        # alone, not for the must-port entry: the same port without any list is just as incomplete
        empty = self.d / "empty.json"
        empty.write_text(json.dumps({"format": 1, "names": []}))
        with redirect_stderr(io.StringIO()):
            rc0, r0, _ = self.run_port(old, other, "--must-port", str(empty))
        self.assertEqual((rc, r["summary"]["complete"]), (rc0, r0["summary"]["complete"]))

    def test_a_warn_level_name_neither_exe_resolves_leaves_the_port_complete(self):
        tree = json.loads((self.d / "tree/server.json").read_text())
        tree["symbols"]["Never"] = {"find": [{"pattern": "DE AD BE EF 01 02 03 04"}]}
        (self.d / "tree/server.json").write_text(json.dumps(tree, indent=2) + "\n")
        old, new, _t = self.exes(target(0xE179C0, 0x20, field_disp8=0x10))
        p = self.d / "must_port.json"
        p.write_text(json.dumps({"format": 1, "names": [{"name": "Never", "level": "warn", "why": "fixture warn"}]}))
        with redirect_stderr(io.StringIO()):
            rc, r, _ = self.run_port(old, new, "--must-port", str(p))
        self.assertEqual(rc, 0, r["summary"])
        self.assertTrue(r["summary"]["complete"])
        self.assertIn("must-port symbol Never (warning only): does not resolve", r["summary"]["must_port"]["warned"][0])

    def test_a_listed_name_neither_exe_resolves_fails_the_port(self):
        # the old exe does not resolve it either, so it is not "lost" and the port is otherwise complete
        tree = json.loads((self.d / "tree/server.json").read_text())
        tree["symbols"]["Never"] = {"find": [{"pattern": "DE AD BE EF 01 02 03 04"}]}
        (self.d / "tree/server.json").write_text(json.dumps(tree, indent=2) + "\n")
        old, new, _t = self.exes(target(0xE179C0, 0x20, field_disp8=0x10))
        rc, r, _ = self.run_port(old, new)
        self.assertEqual(rc, 0, r["summary"])  # the repository list names nothing of this fixture
        with redirect_stderr(io.StringIO()):
            rc, r, _ = self.run_port(old, new, *self.must_port("Never"))
        self.assertEqual(rc, 1)
        self.assertIn("must-port symbol Never: does not resolve on the new exe", r["summary"]["must_port"]["failed"][0])


class SigdbCheck(SyntheticPort):
    """sigdb_check.py: a listed name the definitions declare and the exe does not resolve gets a must-port line."""

    def test_a_listed_name_that_does_not_resolve_is_named(self):
        tree = json.loads((self.d / "tree/server.json").read_text())
        tree["symbols"]["Never"] = {"find": [{"pattern": "DE AD BE EF 01 02 03 04"}]}
        (self.d / "tree/server.json").write_text(json.dumps(tree, indent=2) + "\n")
        old, _new, _t = self.exes(target(0xE179C0, 0x20, field_disp8=0x10))
        p = self.d / "must_port.json"
        p.write_text(json.dumps({"format": 1, "names": [{"name": "Never", "why": "fixture never"},
                                                         {"name": "GetThing", "why": "fixture thing"},
                                                         {"name": "Other/NotDeclared", "why": "fixture other"}]}))
        argv = sys.argv
        try:
            sys.argv = ["sigdb_check.py", str(self.d / "tree/server.json"), str(old), "--must-port", str(p)]
            rc, out, _ = quiet(sigdb_check.main)
        finally:
            sys.argv = argv
        self.assertEqual(rc, 1)
        self.assertIn("FAIL  must-port symbol Never: does not resolve on this exe", out)
        self.assertIn("fixture never", out)
        self.assertNotIn("must-port symbol GetThing", out)
        self.assertNotIn("Other/NotDeclared", out)

    def test_a_warn_level_name_that_does_not_resolve_gets_a_warn_line(self):
        tree = json.loads((self.d / "tree/server.json").read_text())
        tree["symbols"]["Never"] = {"find": [{"pattern": "DE AD BE EF 01 02 03 04"}]}
        (self.d / "tree/server.json").write_text(json.dumps(tree, indent=2) + "\n")
        old, _new, _t = self.exes(target(0xE179C0, 0x20, field_disp8=0x10))
        p = self.d / "must_port.json"
        p.write_text(json.dumps({"format": 1, "names": [{"name": "Never", "level": "warn", "why": "fixture never"}]}))
        argv = sys.argv
        try:
            sys.argv = ["sigdb_check.py", str(self.d / "tree/server.json"), str(old), "--must-port", str(p)]
            rc, out, _ = quiet(sigdb_check.main)
        finally:
            sys.argv = argv
        self.assertIn("WARN  must-port symbol Never (warning only): does not resolve on this exe", out)
        self.assertNotIn("FAIL  must-port", out)
        self.assertEqual(rc, 1)  # an unresolved symbol fails sigdb_check as before, listed or not


def load_tests(loader, _tests, _pattern):
    """This module's own tests only: the fixtures it borrows (Synthetic, SyntheticPort) run in their own modules."""
    suite = unittest.TestSuite()
    for cls in (TheList, Repository, RecordCheck, PortCheck, SigdbCheck):
        for name in sorted(n for n in vars(cls) if n.startswith("test")):
            suite.addTest(cls(name))
    return suite


if __name__ == "__main__":
    unittest.main()
