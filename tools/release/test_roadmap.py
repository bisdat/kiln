#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""tools/release/roadmap.py, offline: the roadmap source (tools/release/roadmap/roadmap.json) round-trips byte-identically
into the site's src/data/kilnRoadmap.json, check/ship on minor patches and majors, idempotence, dates, and deploy's
refusal paths with every git, ssh, npm and curl call stubbed (nothing here touches a network, a site checkout or the
VPS).

    python3 -I tools/release/test_roadmap.py
"""
import copy
import importlib.util
import json
import shutil
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location("roadmap", HERE / "roadmap.py")
roadmap = importlib.util.module_from_spec(spec)
spec.loader.exec_module(roadmap)

SOURCE = HERE / "roadmap" / "roadmap.json"
LIVE = "a" * 40
OTHER = "b" * 40


def seeded():
    return json.loads(SOURCE.read_text(encoding="utf-8"))


class Format(unittest.TestCase):
    def test_seeded_source_round_trips_byte_identically(self):
        text = SOURCE.read_text(encoding="utf-8")
        self.assertEqual(roadmap.dump(json.loads(text)), text)
        roadmap.validate(json.loads(text))

    def test_site_writes_the_page_file_byte_identically(self):
        with tempfile.TemporaryDirectory() as d:
            site = Path(d)
            (site / "src/lib").mkdir(parents=True)
            (site / "src/lib/kilnRoadmap.ts").write_text("// stub\n")
            self.assertTrue(roadmap.write_site(seeded(), site))
            self.assertEqual((site / roadmap.SITE_FILE).read_bytes(), SOURCE.read_bytes())
            self.assertFalse(roadmap.write_site(seeded(), site), "a second write changes nothing")

    def test_site_refuses_a_folder_that_is_not_the_site(self):
        with tempfile.TemporaryDirectory() as d:
            with self.assertRaises(roadmap.RoadmapError):
                roadmap.write_site(seeded(), Path(d))

    def test_validation_catches_what_the_page_would_reject(self):
        bad = [
            lambda d: d["released"][0]["features"][0].update(area="nowhere"),
            lambda d: d["released"][0]["features"][0].update(kind="feature"),
            lambda d: d["released"][0]["features"][0].update(held="all"),  # held without gamePending
            lambda d: d["released"][0].update(date="2026-02-30"),
            lambda d: d["released"][0].pop("patch"),
            lambda d: d["released"][0].update(features=[]),
            lambda d: d["upcoming"][1].update(status="next"),
            lambda d: d["upcoming"].append(copy.deepcopy(d["upcoming"][0])),
            lambda d: d["released"].reverse(),
            lambda d: d["milestones"][0].update(after="0.2.0-dev.99"),
            lambda d: d["released"][0]["features"][0].update(title="  "),
        ]
        for i, change in enumerate(bad):
            d = seeded()
            change(d)
            with self.assertRaises(roadmap.RoadmapError, msg=i):
                roadmap.validate(d)


class Check(unittest.TestCase):
    def setUp(self):
        self.notes = Path(tempfile.mkdtemp(prefix="kiln-roadmap-notes-"))

    def tearDown(self):
        shutil.rmtree(self.notes, ignore_errors=True)

    def check(self, data, version):
        return roadmap.check(data, version, self.notes)

    def test_the_next_minor_patch_passes(self):
        self.assertEqual(self.check(seeded(), "0.2.0-dev.29.19"), [])

    def test_the_next_major_passes_once_it_is_first(self):
        d = seeded()
        roadmap.ship(d, "0.2.0-dev.29.19", "2026-11-20")
        self.assertEqual(self.check(d, "0.2.0-dev.30"), [])

    def test_a_major_behind_an_unshipped_patch_fails(self):
        with self.assertRaisesRegex(roadmap.RoadmapError, "0.2.0-dev.29.19"):
            self.check(seeded(), "0.2.0-dev.30")

    def test_a_missing_entry_says_how_to_add_it(self):
        with self.assertRaisesRegex(roadmap.RoadmapError, "no entry.*README"):
            self.check(seeded(), "0.2.0-dev.29.20")

    def test_not_a_version(self):
        for v in ("0.2.0-dev", "dev.30", "0.2.0-dev.29.19-beta.1", ""):
            with self.assertRaises(roadmap.RoadmapError, msg=v):
                self.check(seeded(), v)

    def test_a_released_version_passes_with_a_notice(self):
        warnings = self.check(seeded(), "0.2.0-dev.29.18")
        self.assertEqual(len(warnings), 1)
        self.assertIn("already released", warnings[0])

    def test_release_notes_items_missing_from_the_entry_warn(self):
        d = seeded()
        first = d["upcoming"][0]["features"][0]["title"]
        (self.notes / "0.2.0-dev.29.19.md").write_text(
            "# Kiln 0.2.0-dev.29.19\n\n## Players\n"
            f"- **{first}.** Some words.\n"
            "- **Teleporting llamas everywhere.** Not on the roadmap.\n")
        warnings = self.check(d, "0.2.0-dev.29.19")
        self.assertEqual(len(warnings), 1, warnings)
        self.assertIn("Teleporting llamas everywhere", warnings[0])


class Ship(unittest.TestCase):
    def test_shipping_a_minor_patch(self):
        d = seeded()
        planned = copy.deepcopy(d["upcoming"][0])
        self.assertTrue(roadmap.ship(d, "0.2.0-dev.29.19", "2026-11-20"))
        top = d["released"][0]
        self.assertEqual(list(top), ["version", "date", "title", "summary", "patch", "features"])
        self.assertEqual(top["version"], "0.2.0-dev.29.19")
        self.assertEqual(top["date"], "2026-11-20")
        self.assertTrue(top["patch"])
        self.assertEqual(top["features"], planned["features"])
        self.assertEqual(top["title"], planned["title"])
        self.assertEqual(d["upcoming"][0]["version"], "0.2.0-dev.30")
        self.assertEqual(d["upcoming"][0]["status"], "next")
        self.assertEqual(list(d["upcoming"][0])[:2], ["version", "status"])
        roadmap.validate(d)

    def test_shipping_a_major_makes_the_following_major_next(self):
        d = seeded()
        roadmap.ship(d, "0.2.0-dev.29.19", "2026-11-20")
        self.assertTrue(roadmap.ship(d, "0.2.0-dev.30", "2026-11-24"))
        top = d["released"][0]
        self.assertEqual(top["version"], "0.2.0-dev.30")
        self.assertNotIn("patch", top)
        self.assertNotIn("note", top)
        self.assertEqual(d["upcoming"][0]["version"], "0.2.0-dev.31")
        self.assertEqual(d["upcoming"][0]["status"], "next")
        self.assertFalse(any(r["version"] == "0.2.0-dev.30.1" for r in d["released"] + d["upcoming"]),
                         "no patch slot is invented")

    def test_a_patch_slot_the_source_lists_becomes_next_after_its_major(self):
        d = seeded()
        roadmap.ship(d, "0.2.0-dev.29.19", "2026-11-20")
        d["upcoming"].insert(1, {"version": "0.2.0-dev.30.1", "status": "planned", "title": "First fixes",
                                 "summary": "Fixes after dev.30.", "patch": True,
                                 "features": [{"title": "A fix", "text": "Something fixed.", "area": "core", "kind": "fix"}]})
        roadmap.validate(d)
        roadmap.ship(d, "0.2.0-dev.30", "2026-11-24")
        self.assertEqual(d["upcoming"][0]["version"], "0.2.0-dev.30.1")
        self.assertEqual(d["upcoming"][0]["status"], "next")
        self.assertEqual(d["upcoming"][1]["status"], "planned")

    def test_shipping_again_changes_nothing(self):
        d = seeded()
        roadmap.ship(d, "0.2.0-dev.29.19", "2026-11-20")
        before = roadmap.dump(d)
        self.assertFalse(roadmap.ship(d, "0.2.0-dev.29.19", "2026-11-21"))
        self.assertEqual(roadmap.dump(d), before)
        self.assertFalse(roadmap.ship(d, "0.2.0-dev.29.18", None))

    def test_missing_entry_and_out_of_order_refused(self):
        with self.assertRaisesRegex(roadmap.RoadmapError, "no entry"):
            roadmap.ship(seeded(), "0.2.0-dev.29.20", "2026-11-20")
        with self.assertRaisesRegex(roadmap.RoadmapError, "0.2.0-dev.29.19"):
            roadmap.ship(seeded(), "0.2.0-dev.30", "2026-11-20")

    def test_dates(self):
        with self.assertRaises(roadmap.RoadmapError):
            roadmap.ship(seeded(), "0.2.0-dev.29.19", "2026-11-31")
        with self.assertRaisesRegex(roadmap.RoadmapError, "older"):
            roadmap.ship(seeded(), "0.2.0-dev.29.19", "2026-11-18")
        d = seeded()
        roadmap.ship(d, "0.2.0-dev.29.19", "2026-11-19")  # the same day as 29.18: still newest first
        self.assertEqual(d["released"][0]["version"], "0.2.0-dev.29.19")
        # GitHub's publish time as a New York date
        self.assertEqual(roadmap.ny_date("2026-11-20T03:30:00Z"), "2026-11-19")
        self.assertEqual(roadmap.ny_date("2026-11-20T05:30:00Z"), "2026-11-20")
        self.assertEqual(roadmap.ny_date("2026-07-01T03:59:00Z"), "2026-06-30")  # daylight time, UTC-4

    def test_github_date(self):
        ok = Fake({("gh", "release", "view"): (0, '{"isDraft": false, "publishedAt": "2026-11-20T03:30:00Z"}')})
        self.assertEqual(roadmap.github_date("0.2.0-dev.29.19", ok), "2026-11-19")
        self.assertIn(["gh", "release", "view", "v0.2.0-dev.29.19", "--repo", "frostdev-ops/kiln", "--json",
                       "isDraft,publishedAt"], ok.calls)
        for answer in [(0, '{"isDraft": true, "publishedAt": null}'), (1, "release not found")]:
            with self.assertRaisesRegex(roadmap.RoadmapError, "--date"):
                roadmap.github_date("0.2.0-dev.29.19", Fake({("gh", "release", "view"): answer}))

    def test_cli_ship_writes_the_source_once(self):
        with tempfile.TemporaryDirectory() as d:
            src = Path(d) / "roadmap.json"
            shutil.copy(SOURCE, src)
            self.assertEqual(roadmap.main(["--source", str(src), "ship", "0.2.0-dev.29.19", "--date", "2026-11-20"]), 0)
            after = src.read_bytes()
            self.assertNotEqual(after, SOURCE.read_bytes())
            self.assertEqual(roadmap.main(["--source", str(src), "ship", "0.2.0-dev.29.19", "--date", "2026-11-22"]), 0)
            self.assertEqual(src.read_bytes(), after)
            self.assertEqual(roadmap.main(["--source", str(src), "check", "0.2.0-dev.29.21"]), 1)


class Fake:
    """Stands in for roadmap.Runner: answers by command prefix, records every call."""

    def __init__(self, answers=None):
        self.answers = answers or {}
        self.calls = []

    def run(self, cmd, cwd=None, check=True, stream=False):
        self.calls.append(list(cmd))
        best = None
        for prefix, answer in self.answers.items():
            if tuple(cmd[:len(prefix)]) == prefix and (best is None or len(prefix) > len(best[0])):
                best = (prefix, answer)
        code, out = best[1] if best else (0, "")
        if callable(out):
            out = out()
        if check and code != 0:
            raise roadmap.RoadmapError(f"failed: {' '.join(cmd)}")
        return code, out


class Deploy(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix="kiln-roadmap-site-"))
        self.site = self.tmp / "site"
        (self.site / "src/lib").mkdir(parents=True)
        (self.site / "src/lib/kilnRoadmap.ts").write_text("// stub\n")
        (self.site / "src/data").mkdir(parents=True)
        (self.site / roadmap.SITE_FILE).write_bytes(SOURCE.read_bytes())
        self.data = seeded()
        roadmap.ship(self.data, "0.2.0-dev.29.19", "2026-11-20")
        self.page = '<details class="kiln-rm-release" id="dev-29-19" data-release data-status="released" data-patch open>'

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def fake(self, over=None):
        answers = {
            ("git", "status", "--porcelain"): (0, ""),
            ("git", "rev-parse", "--abbrev-ref", "HEAD"): (0, "main\n"),
            ("git", "rev-parse", "HEAD"): (0, LIVE + "\n"),
            ("git", "rev-parse", "origin/main"): (0, LIVE + "\n"),
            ("ssh",): (0, f"/opt/frostdev-public/releases/{LIVE}\n"),
            ("curl",): (0, self.page),
        }
        answers.update(over or {})
        return Fake(answers)

    def deploy(self, fake, **kw):
        kw.setdefault("yes", True)
        return roadmap.deploy(self.data, self.site, "0.2.0-dev.29.19", fake, sleep=lambda s: None, **kw)

    def ran(self, fake, *prefix):
        return [c for c in fake.calls if tuple(c[:len(prefix)]) == prefix]

    def test_a_dirty_tree_is_refused_before_anything(self):
        fake = self.fake({("git", "status", "--porcelain"): (0, " M src/pages/kiln.astro\n")})
        with self.assertRaisesRegex(roadmap.RoadmapError, "dirty|clean"):
            self.deploy(fake)
        self.assertFalse(self.ran(fake, "git", "pull"))
        self.assertEqual((self.site / roadmap.SITE_FILE).read_bytes(), SOURCE.read_bytes())

    def test_another_branch_is_refused(self):
        fake = self.fake({("git", "rev-parse", "--abbrev-ref", "HEAD"): (0, "feature\n")})
        with self.assertRaisesRegex(roadmap.RoadmapError, "main"):
            self.deploy(fake)

    def test_live_ahead_of_or_different_from_origin_stops(self):
        cases = [
            # live is a revision origin/main does not have
            {("ssh",): (0, f"/opt/frostdev-public/releases/{OTHER}\n"), ("git", "cat-file"): (1, "")},
            # live is ahead of origin/main
            {("ssh",): (0, f"/opt/frostdev-public/releases/{OTHER}\n"), ("git", "cat-file"): (0, ""),
             ("git", "merge-base", "--is-ancestor", LIVE, OTHER): (0, ""),
             ("git", "merge-base", "--is-ancestor", OTHER, LIVE): (1, "")},
            # origin/main has commits that were never deployed
            {("ssh",): (0, f"/opt/frostdev-public/releases/{OTHER}\n"), ("git", "cat-file"): (0, ""),
             ("git", "merge-base", "--is-ancestor", LIVE, OTHER): (1, ""),
             ("git", "merge-base", "--is-ancestor", OTHER, LIVE): (0, ""),
             ("git", "rev-list", "--count"): (0, "2\n")},
            # the live link can not be read
            {("ssh",): (255, "ssh: connect to host loothing-vps port 22: No route to host")},
        ]
        for i, over in enumerate(cases):
            fake = self.fake(over)
            with self.assertRaises(roadmap.RoadmapError, msg=i):
                self.deploy(fake)
            self.assertFalse(self.ran(fake, "npm"), i)
            self.assertFalse(self.ran(fake, "git", "commit"), i)
            self.assertEqual((self.site / roadmap.SITE_FILE).read_bytes(), SOURCE.read_bytes(), i)

    def test_undeployed_roadmap_only_commits_are_deployed_on_a_retry(self):
        (self.site / roadmap.SITE_FILE).write_text(roadmap.dump(roadmap.site_view(self.data)))
        fake = self.fake({("ssh",): (0, f"/opt/frostdev-public/releases/{OTHER}\n"), ("git", "cat-file"): (0, ""),
                            ("git", "merge-base", "--is-ancestor", LIVE, OTHER): (1, ""),
                            ("git", "merge-base", "--is-ancestor", OTHER, LIVE): (0, ""),
                            ("git", "diff", "--name-only"): (0, roadmap.SITE_FILE + "\n"),
                            ("git", "rev-list", "--count"): (0, "1\n")})
        self.assertEqual(self.deploy(fake), 0)
        self.assertFalse([c for c in fake.calls if "commit" in c], "nothing new to commit")
        self.assertIn(["bash", "ops/deploy.sh"], fake.calls)

    def test_unpushed_local_commits_are_refused(self):
        fake = self.fake({("git", "rev-parse", "HEAD"): (0, OTHER + "\n")})
        with self.assertRaisesRegex(roadmap.RoadmapError, "origin/main"):
            self.deploy(fake)

    def test_unchanged_data_is_skipped(self):
        fake = self.fake()
        self.assertEqual(roadmap.deploy(seeded(), self.site, None, fake, yes=True, sleep=lambda s: None), 0)
        self.assertFalse(self.ran(fake, "npm"))
        self.assertFalse(self.ran(fake, "git", "commit"))

    def test_dry_run_stops_before_commit_and_restores_the_file(self):
        fake = self.fake()
        self.assertEqual(self.deploy(fake, dry_run=True, yes=False), 0)
        self.assertEqual([c[:2] for c in self.ran(fake, "npm")], [["npm", "ci"], ["npm", "run"], ["npm", "run"]])
        for never in (("git", "commit"), ("git", "push"), ("bash",), ("curl",)):
            self.assertFalse(self.ran(fake, *never), never)
        self.assertEqual((self.site / roadmap.SITE_FILE).read_bytes(), SOURCE.read_bytes())
        self.assertIn(["git", "pull", "--ff-only", "origin", "main"], fake.calls)

    def test_a_full_run_commits_pushes_deploys_and_verifies_in_that_order(self):
        fake = self.fake()
        self.assertEqual(self.deploy(fake), 0)
        names = [" ".join(c[:3]) for c in fake.calls]
        order = [names.index(n) for n in ("git pull --ff-only", "ssh -o BatchMode=yes", "npm ci", "npm run check",
                                         "npm run build")]
        self.assertEqual(order, sorted(order))
        commit = next(c for c in fake.calls if "commit" in c)
        self.assertIn("user.name=Frostdev", commit)
        self.assertIn("user.email=admin@frostdev.io", commit)
        message = commit[commit.index("-m") + 1]
        self.assertEqual(message, "Kiln roadmap: 0.2.0-dev.29.19 released")
        self.assertNotRegex(message, "(?i)co-authored|claude|generated")
        i_commit = fake.calls.index(commit)
        i_push = fake.calls.index(["git", "push", "origin", "HEAD:main"])
        i_deploy = fake.calls.index(["bash", "ops/deploy.sh"])
        i_curl = max(i for i, c in enumerate(fake.calls) if c[0] == "curl")
        self.assertLess(names.index("npm run build"), i_commit)
        self.assertEqual([i_commit, i_push, i_deploy, i_curl], sorted([i_commit, i_push, i_deploy, i_curl]))
        self.assertEqual(json.loads((self.site / roadmap.SITE_FILE).read_text())["released"][0]["version"], "0.2.0-dev.29.19")

    def test_the_build_touching_other_files_stops_before_commit(self):
        status = iter([(0, ""), (0, " M src/data/kilnRoadmap.json\n?? stray.txt\n")])
        fake = self.fake({("git", "status", "--porcelain"): (0, lambda: next(status)[1])})
        with self.assertRaisesRegex(roadmap.RoadmapError, "stray"):
            self.deploy(fake)
        self.assertFalse(self.ran(fake, "git", "commit"))

    def test_a_page_that_does_not_show_it_released_fails_after_retries(self):
        fake = self.fake({("curl",): (0, self.page.replace("released", "next"))})
        with self.assertRaisesRegex(roadmap.RoadmapError, "released"):
            self.deploy(fake)
        self.assertGreater(len(self.ran(fake, "curl")), 1)

    def test_without_yes_a_no_answer_commits_nothing(self):
        fake = self.fake()
        self.assertEqual(roadmap.deploy(self.data, self.site, "0.2.0-dev.29.19", fake, yes=False,
                                        ask=lambda q: False, sleep=lambda s: None), 1)
        self.assertFalse(self.ran(fake, "git", "commit"))
        self.assertEqual((self.site / roadmap.SITE_FILE).read_bytes(), SOURCE.read_bytes())


class Hooks(unittest.TestCase):
    """release.sh and publish.sh call roadmap.py at the right places (read from the scripts)."""

    def test_release_sh_checks_before_it_builds(self):
        text = (HERE / "release.sh").read_text(encoding="utf-8")
        check = text.index('"$here/roadmap.py" check "$version"')
        self.assertLess(check, text.index('mkdir -p "$out" "$work"'))
        self.assertGreater(check, text.index('"$here/version.sh" check "$version"'))

    def test_publish_sh_ships_after_github_and_never_fails_the_release_on_it(self):
        text = (HERE / "publish.sh").read_text(encoding="utf-8")
        self.assertIn('roadmap=(python3 -I "$here/roadmap.py")', text)
        ship = text.index('elif ! "${roadmap[@]}" ship "$version"')
        self.assertGreater(ship, text.index('gh release edit "$tag"'))
        self.assertGreater(ship, text.index("gh_published=1"))
        self.assertIn("KILN_SITE_DIR", text)
        self.assertIn("ROADMAP NOT UPDATED", text)

    def test_publish_sh_roadmap_failure_is_a_loud_warning_not_the_release_status(self):
        """publish.sh's roadmap block and closing lines, run in bash with a fake roadmap.py (nothing else of publish.sh)."""
        import subprocess
        text = (HERE / "publish.sh").read_text(encoding="utf-8")
        block = text[text.index("# 4. the roadmap: the version"):text.index("if ((nexus)); then")]
        closing = text[text.index("if [[ -n $roadmap_warning ]]; then\n  printf"):]
        for fails, published in (("ship", 1), ("deploy", 1), ("nothing", 0)):
            with tempfile.TemporaryDirectory() as d:
                (Path(d) / "roadmap.py").write_text(
                    f"import sys\nprint('fake', *sys.argv[1:])\nsys.exit(1 if sys.argv[1] == {fails!r} else 0)\n")
                script = ("set -euo pipefail\n"
                          f"channel=dev version=0.2.0-dev.29.19 name=0.2.0-dev.29.19 yes=1 dry=0 gh_published={published}\n"
                          f"here={d} repo_dir={d}\nr() {{ :; }}\nbeta_warning=''\n" + block + closing)
                r = subprocess.run(["bash", "-c", script], capture_output=True, text=True, timeout=60,
                                   env={"PATH": "/usr/bin:/bin", "HOME": d, "KILN_SITE_DIR": d + "/site"})
                self.assertEqual(r.returncode, 0, (fails, r.stdout, r.stderr))
                self.assertIn("ROADMAP NOT UPDATED", r.stderr, fails)
                self.assertIn("deploy " + d + "/site --version 0.2.0-dev.29.19 --yes", r.stderr, fails)
                if fails == "deploy":
                    self.assertIn("fake ship 0.2.0-dev.29.19", r.stdout)
                if not published:
                    self.assertNotIn("fake", r.stdout, "nothing runs before GitHub is published")
                    self.assertIn("roadmap.py ship 0.2.0-dev.29.19", r.stderr)


if __name__ == "__main__":
    unittest.main()
