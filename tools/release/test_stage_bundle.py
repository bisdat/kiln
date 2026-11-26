#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""The stage/bundle ownership gate: tools/package/stage.sh's sigdb + regmap step, run for real on a minimal stage,
then the bundle rule kilnctl bundle create applies (crates/kiln-manage/src/bundle.rs manifest_for_stage).

  python3 -I tools/release/test_stage_bundle.py         (tools/check.sh runs it as its "bundle" stage)

Offline and without any native or managed build (well under a second). What it runs of stage.sh: the "# sigdb per
side" block through the regmap install, and the "# kiln-runtime.json without files[]" heredoc, both cut from the
script as it is, over the repo's real sigdb/ and regmap/ folders; core is a two-file fixture (dinput8.dll, kiln.json).
It asserts
  (a) every bundle path is owned by exactly one component (kilnctl bundle create refuses "<path> is in two components");
  (b) regmap/common/*.json lands in core only, and each side's regmap/<side>/*.json sits beside its sigdb, in
      sigdb-<side>, next to kiln/sigdb/<side>.json;
  (c) a per-side install is complete: the components whose kiln-runtime.json "sides" name the side hold, together,
      kiln/regmap/common/*.json, kiln/regmap/<side>/*.json and kiln/sigdb/<side>.json.
EveryComponent adds a marker file for each other component stage.sh knows (admin, emf, samples, eml, shroudforge,
enshwick, sfi) and judges their manifest entries the same way; their real files need the builds, so their paths are
judged by the real kilnctl bundle create release.sh runs on the full stage.
KILNCTL=<path to a built kilnctl>: also runs the real kilnctl bundle create on both stages (skipped otherwise; the
same rule runs in Python either way). tools/check.sh sets it when it finds a built kilnctl; release.sh always does. KILN_STAGE_SH=<file>: gate another stage.sh (for example the one before
d76a2aba, to see the gate fail). If a regmap line of stage.sh moves out of the cut blocks, the gate fails and says so.
"""
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

HERE = Path(__file__).resolve().parent
REPO = HERE.parent.parent
STAGE_SH = Path(os.environ.get("KILN_STAGE_SH") or REPO / "tools" / "package" / "stage.sh")
KILNCTL = os.environ.get("KILNCTL")
MANIFEST = "kiln-runtime.json"
SIDES = ("server", "client")

# the cut: [start marker, end marker) of the sigdb + regmap step; the runtime-manifest heredoc up to its PY line
SIGDB_START = "# sigdb per side"
SIGDB_END = "# a mod folder the way deploy-server.sh"
RUNTIME_START = "# kiln-runtime.json without files[]"


def cut(text):
    """The two blocks of stage.sh the gate runs, and the line ranges they cover (1-based, inclusive)."""
    lines = text.split("\n")

    def index(pred, start=0, what=""):
        for i in range(start, len(lines)):
            if pred(lines[i]):
                return i
        raise AssertionError(f"stage.sh: no {what} line (the gate cuts stage.sh at it; update test_stage_bundle.py)")

    a = index(lambda l: l.strip() == SIGDB_START, what=repr(SIGDB_START))
    b = index(lambda l: l.startswith(SIGDB_END), a, repr(SIGDB_END))
    c = index(lambda l: l.strip() == RUNTIME_START, b, repr(RUNTIME_START))
    d = index(lambda l: l == "PY", c, "PY (end of the kiln-runtime.json heredoc)")
    return "\n".join(lines[a:b]), "\n".join(lines[c:d + 1]), [(a + 1, b), (c + 1, d + 1)]


def known_components(stage_sh):
    """The component names stage.sh's kiln-runtime.json writer knows (its `known` table)."""
    _, runtime, _ = cut(Path(stage_sh).read_text(encoding="utf-8"))
    return re.findall(r'^\s*"([a-z][a-z0-9-]*)": \{"sides"', runtime, re.M)


def run_stage(stage_sh, stage, scratch, extra=()):
    """A minimal stage: core fixture + stage.sh's own sigdb/regmap step + its kiln-runtime.json writer. extra: component
    folders to add, each with one marker file of its own (their real files need the native, managed and Rust builds)."""
    sigdb, runtime, _ = cut(Path(stage_sh).read_text(encoding="utf-8"))
    dotnet = Path(scratch) / "dotnet"
    (dotnet / "shared" / "Microsoft.NETCore.App" / "10.0.0").mkdir(parents=True, exist_ok=True)
    markers = [f'mkdir -p "$stage/{c}/kiln/gate" && printf "{c}" >"$stage/{c}/kiln/gate/{c}.txt"' for c in extra]
    script = "\n".join([
        "set -euo pipefail",
        'repo=$1 stage=$2 dotnet_root=$3',
        'rm -rf "$stage" && mkdir -p "$stage/core/kiln"',
        "printf 'MZ proxy' >\"$stage/core/dinput8.dll\"",
        "printf '{\"policy\": \"verified-only\"}\\n' >\"$stage/core/kiln/kiln.json\"",
        *markers,
        sigdb,
        runtime,
    ]) + "\n"
    p = subprocess.run(["bash", "-c", script, "stage-cut", str(REPO), str(stage), str(dotnet)],
                       capture_output=True, text=True, timeout=120)
    if p.returncode != 0:
        raise AssertionError(f"the cut of {stage_sh} failed (exit {p.returncode}):\n{p.stdout}{p.stderr}")


def bundle_files(stage):
    """kilnctl bundle create's view of a stage (bundle.rs manifest_for_stage + validate), without hashing or zipping:
    {path: [components]}, the manifest, and the problems that make create refuse the stage."""
    stage = Path(stage)
    m = json.loads((stage / MANIFEST).read_text(encoding="utf-8"))
    comps = m.get("components", {})
    owners, problems = {}, []
    for e in sorted(stage.iterdir(), key=lambda p: p.name):
        if e.name == MANIFEST:
            continue
        if not e.is_dir() or e.is_symlink():
            problems.append(f"unexpected file {e.name} (components are folders)")
            continue
        if e.name not in comps:
            problems.append(f"folder {e.name} is not a component in {MANIFEST}")
            continue
        for f in sorted(e.rglob("*")):
            if f.is_symlink():
                problems.append(f"{f}: symlinks are not bundled")
            elif f.is_file():
                owners.setdefault(f.relative_to(e).as_posix(), []).append(e.name)
    for path, cs in sorted(owners.items()):
        if len(cs) > 1:
            problems.append(f"{path} is in two components ({', '.join(cs)})")
    for c in m.get("config", []):
        if c not in owners:
            problems.append(f"config file {c} is not in any component")
    for name, c in comps.items():
        for r in c.get("requires", []):
            if r not in comps:
                problems.append(f"component {name} requires unknown component {r}")
        if not c.get("sides"):
            problems.append(f"component {name} has no sides")
    return owners, m, problems


def repo_names(sub):
    return sorted(p.name for p in (REPO / "regmap" / sub).glob("*.json"))


class RuleSelfTest(unittest.TestCase):
    """The Python rule is not vacuous: a path in two components and a missing config file are refused."""

    def test_refuses_a_path_in_two_components(self):
        with tempfile.TemporaryDirectory(prefix="kiln-stage-gate-") as t:
            s = Path(t)
            for comp in ("core", "sigdb-server"):
                (s / comp / "kiln").mkdir(parents=True)
                (s / comp / "kiln" / "x.json").write_text("{}")
            (s / MANIFEST).write_text(json.dumps({"components": {
                "core": {"sides": ["server"]}, "sigdb-server": {"sides": ["server"], "requires": ["core"]}},
                "config": ["kiln/kiln.json"]}))
            _, _, problems = bundle_files(s)
            self.assertIn("kiln/x.json is in two components (core, sigdb-server)", problems)
            self.assertIn("config file kiln/kiln.json is not in any component", problems)

    def test_regmap_lines_are_inside_the_cut(self):
        """Every stage.sh line that installs regmap/ rows is in the blocks the gate runs (else update the cut)."""
        text = STAGE_SH.read_text(encoding="utf-8")
        _, _, ranges = cut(text)
        outside = [f"{n}: {l.strip()}" for n, l in enumerate(text.split("\n"), 1)
                   if "regmap" in l and not l.lstrip().startswith("#") and not any(a <= n <= b for a, b in ranges)]
        self.assertEqual(outside, [], f"{STAGE_SH}: regmap lines outside the gate's cut")


class StageBundle(unittest.TestCase):
    """stage.sh's sigdb/regmap step, judged the way kilnctl bundle create judges a stage."""

    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory(prefix="kiln-stage-gate-")
        cls.stage = Path(cls.tmp.name) / "stage"
        run_stage(STAGE_SH, cls.stage, cls.tmp.name)
        cls.owners, cls.manifest, cls.problems = bundle_files(cls.stage)
        cls.common = repo_names("common")
        cls.side = {s: repo_names(s) for s in SIDES}

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def test_fixture_inputs(self):
        self.assertTrue(self.common, "regmap/common holds no *.json: the gate would prove nothing")
        for s in SIDES:
            self.assertIn(f"sigdb-{s}", self.manifest["components"])

    def test_a_every_path_has_one_component(self):
        self.assertEqual(self.problems, [], "kilnctl bundle create would refuse this stage")

    def test_b_common_regmap_in_core_only(self):
        got = {p: cs for p, cs in self.owners.items() if p.startswith("kiln/regmap/common/")}
        self.assertEqual(sorted(got), [f"kiln/regmap/common/{n}" for n in self.common])
        wrong = {p: cs for p, cs in got.items() if cs != ["core"]}
        self.assertEqual(wrong, {}, "regmap/common belongs to core alone")

    def test_b_side_regmap_beside_its_sigdb(self):
        for s in SIDES:
            with self.subTest(side=s):
                self.assertEqual(self.owners.get(f"kiln/sigdb/{s}.json"), [f"sigdb-{s}"])
                got = {p: cs for p, cs in self.owners.items() if p.startswith(f"kiln/regmap/{s}/")}
                self.assertEqual(sorted(got), [f"kiln/regmap/{s}/{n}" for n in self.side[s]])
                wrong = {p: cs for p, cs in got.items() if cs != [f"sigdb-{s}"]}
                self.assertEqual(wrong, {}, f"regmap/{s} belongs to sigdb-{s} alone")

    def test_c_each_side_install_is_complete(self):
        for s in SIDES:
            with self.subTest(side=s):
                comps = {n for n, c in self.manifest["components"].items() if s in c.get("sides", [])}
                have = {p for p, cs in self.owners.items() if set(cs) & comps}
                need = {f"kiln/regmap/common/{n}" for n in self.common} | \
                       {f"kiln/regmap/{s}/{n}" for n in self.side[s]} | {f"kiln/sigdb/{s}.json"}
                self.assertEqual(sorted(need - have), [], f"a {s} install ({', '.join(sorted(comps))}) lacks these")

    @unittest.skipUnless(KILNCTL, "KILNCTL is not set (the same rule ran in Python above)")
    def test_kilnctl_bundle_create(self):
        with tempfile.TemporaryDirectory(prefix="kiln-stage-gate-dist-") as out:
            p = subprocess.run([KILNCTL, "bundle", "create", str(self.stage), "--out", out,
                                "--git", "0123abc", "--epoch", "1794072134"],
                               capture_output=True, text=True, timeout=120)
            self.assertEqual(p.returncode, 0, f"kilnctl bundle create refused the stage:\n{p.stdout}{p.stderr}")


class EveryComponent(unittest.TestCase):
    """The other components (admin, emf, samples, eml, shroudforge, enshwick, sfi): their manifest entries, judged on a
    stage that has every one of them. Their real files come from the native, managed, Rust and GPL builds, so which paths
    they hold is not judged here: release.sh runs the real kilnctl bundle create on the full stage before the runtime
    zip, and lib/plugins.py check then requires every compatibility plugin in a default component for both sides."""

    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory(prefix="kiln-stage-gate-all-")
        cls.stage = Path(cls.tmp.name) / "stage"
        cls.known = known_components(STAGE_SH)
        cls.extra = [c for c in cls.known if c != "core" and not c.startswith("sigdb-")]
        run_stage(STAGE_SH, cls.stage, cls.tmp.name, cls.extra)
        cls.owners, cls.manifest, cls.problems = bundle_files(cls.stage)

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def test_the_cut_knows_the_components(self):
        for c in ("admin", "emf", "samples", "eml", "shroudforge", "enshwick", "sfi"):
            self.assertIn(c, self.extra, f"stage.sh's kiln-runtime.json writer no longer knows {c}: update this gate")

    def test_every_component_is_declared_and_well_formed(self):
        self.assertEqual(sorted(self.manifest["components"]), sorted(self.known))
        self.assertEqual(self.problems, [], "kilnctl bundle create would refuse a stage with every component")
        for name, c in self.manifest["components"].items():
            with self.subTest(component=name):
                self.assertTrue(set(c["sides"]) <= set(SIDES), c["sides"])
                if name != "core":
                    self.assertIn("core", c.get("requires", []), f"{name} needs core")
                self.assertIsInstance(c.get("default"), bool)

    def test_requires_are_on_the_same_sides(self):
        comps = self.manifest["components"]
        for name, c in comps.items():
            for r in c.get("requires", []):
                with self.subTest(component=name, requires=r):
                    self.assertTrue(set(c["sides"]) <= set(comps[r]["sides"]), f"{name} runs on a side {r} does not")

    @unittest.skipUnless(KILNCTL, "KILNCTL is not set (the same rule ran in Python above)")
    def test_kilnctl_bundle_create(self):
        with tempfile.TemporaryDirectory(prefix="kiln-stage-gate-dist-") as out:
            p = subprocess.run([KILNCTL, "bundle", "create", str(self.stage), "--out", out,
                                "--git", "0123abc", "--epoch", "1794072134"],
                               capture_output=True, text=True, timeout=120)
            self.assertEqual(p.returncode, 0, f"kilnctl bundle create refused a stage with every component:\n{p.stdout}{p.stderr}")


if __name__ == "__main__":
    unittest.main(verbosity=2)
