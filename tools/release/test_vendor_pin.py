#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Tests for tools/release/vendor_pin.py on fake files: it pins a copy of the right size and git blob, refuses a copy
that is missing, of another size or another blob, never replaces a pinned hash, --check, --write and its layout, and
that the shipped manifest is in the tool's layout (a pin changes only the sha256 lines). Nothing is downloaded.

  python3 -I tools/release/test_vendor_pin.py
"""
import contextlib
import hashlib
import importlib.util
import io
import json
from pathlib import Path
import tempfile
import unittest

HERE = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location("vendor_pin", HERE / "vendor_pin.py")
vp = importlib.util.module_from_spec(spec)
spec.loader.exec_module(vp)


def blob(data):
    return hashlib.sha1(b"blob %d\0" % len(data) + data).hexdigest()


A = b"fake loader " * 100
B = bytes(range(256)) * 40
C = b"fake xess" * 333


def manifest():
    def f(name, data, sha="not-pinned", with_blob=True):
        e = {"name": name, "url": f"https://raw.githubusercontent.com/o/r/v1.0.0/{name}", "size": len(data), "sha256": sha}
        if with_blob:
            e["gitBlob"] = blob(data)
        return e
    return {"format": 1, "components": [
        {"id": "amd-fsr", "vendor": "amd", "version": "1.0.0", "files": [f("a.dll", A), f("b.dll", B, with_blob=False)]},
        {"id": "intel-xess", "vendor": "intel", "version": "1.0.0", "files": [f("libxess.dll", C)]},
    ]}


class Pin(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.dir = Path(self.tmp.name)

    def tearDown(self):
        self.tmp.cleanup()

    def put(self, rel, data):
        p = self.dir / rel
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_bytes(data)
        return p

    def run_pin(self, m, components=None, check=False):
        lines = []
        changed, problems = vp.pin(m, self.dir, components, check=check, out=lines.append)
        return changed, problems, lines

    def test_copies_side_by_side_or_per_component_are_pinned(self):
        self.put("a.dll", A)
        self.put("amd-fsr/b.dll", B)
        self.put("libxess.dll", C)
        m = manifest()
        changed, problems, lines = self.run_pin(m)
        self.assertEqual(problems, [])
        self.assertEqual(changed, 3)
        files = [f for c in m["components"] for f in c["files"]]
        self.assertEqual([f["sha256"] for f in files], [hashlib.sha256(d).hexdigest() for d in (A, B, C)])
        self.assertTrue(any("amd-fsr/a.dll: sha256" in l for l in lines))
        # again: nothing to pin, everything matches
        changed, problems, lines = self.run_pin(m)
        self.assertEqual((changed, problems), (0, []))
        self.assertTrue(all("matches" in l for l in lines))
        self.assertEqual(self.run_pin(m, check=True)[1], [])

    def test_a_folder_per_component_and_version(self):
        self.put("amd-fsr-1.0.0/a.dll", A)
        self.put("amd-fsr-1.0.0/b.dll", B)
        self.put("intel-xess-1.0.0/libxess.dll", C)
        # another version's folder is not this one's copy
        self.put("intel-xess-0.9.0/libxess.dll", b"old")
        m = manifest()
        changed, problems, _ = self.run_pin(m)
        self.assertEqual((changed, problems), (3, []))

    def test_one_component_alone(self):
        self.put("libxess.dll", C)
        m = manifest()
        changed, problems, _ = self.run_pin(m, ["intel-xess"])
        self.assertEqual((changed, problems), (1, []))
        self.assertEqual(m["components"][0]["files"][0]["sha256"], "not-pinned")
        changed, problems, _ = self.run_pin(m, ["nvidia-dlssg"])
        self.assertEqual(changed, 0)
        self.assertIn("no component nvidia-dlssg", problems[0])

    def test_a_copy_that_is_not_the_vendors_file_is_refused(self):
        m = manifest()
        self.put("libxess.dll", C[:-1])
        _, problems, _ = self.run_pin(m, ["intel-xess"])
        self.assertIn("the copy has", problems[0])
        # the right size, another content: the git blob tells
        self.put("libxess.dll", C[:-1] + b"X")
        _, problems, _ = self.run_pin(m, ["intel-xess"])
        self.assertIn("git blob", problems[0])
        # missing
        (self.dir / "libxess.dll").unlink()
        _, problems, _ = self.run_pin(m, ["intel-xess"])
        self.assertIn("no copy", problems[0])
        self.assertIn("https://raw.githubusercontent.com/o/r/v1.0.0/libxess.dll", problems[0])
        self.assertEqual(m["components"][1]["files"][0]["sha256"], "not-pinned")
        # a link is not a copy
        target = self.put("elsewhere/libxess.dll", C)
        (self.dir / "libxess.dll").symlink_to(target)
        _, problems, _ = self.run_pin(m, ["intel-xess"])
        self.assertIn("no copy", problems[0])

    def test_a_pinned_hash_is_never_replaced(self):
        m = manifest()
        m["components"][1]["files"][0]["sha256"] = "ab" * 32
        self.put("libxess.dll", C)
        changed, problems, _ = self.run_pin(m, ["intel-xess"])
        self.assertEqual(changed, 0)
        self.assertIn("changed under the same tag", problems[0])
        self.assertEqual(m["components"][1]["files"][0]["sha256"], "ab" * 32)

    def test_check_fails_on_what_is_not_pinned(self):
        self.put("libxess.dll", C)
        m = manifest()
        changed, problems, _ = self.run_pin(m, ["intel-xess"], check=True)
        self.assertEqual(changed, 0)
        self.assertEqual(problems, ["intel-xess/libxess.dll: not pinned"])

    def test_main_writes_only_with_write_and_only_when_everything_matched(self):
        mf = self.dir / "m.json"
        mf.write_text(vp.dump(manifest()), encoding="utf-8")
        files = self.dir / "dl"
        files.mkdir()
        (files / "libxess.dll").write_bytes(C)
        quiet = lambda: contextlib.redirect_stdout(io.StringIO())
        noerr = lambda: contextlib.redirect_stderr(io.StringIO())
        before = mf.read_text(encoding="utf-8")
        with quiet():
            self.assertEqual(vp.main(["--dir", str(files), "--manifest", str(mf), "--component", "intel-xess"]), 0)
        self.assertEqual(mf.read_text(encoding="utf-8"), before, "a dry run writes nothing")
        with quiet(), noerr():
            # AMD's copies are missing: an error, nothing written
            self.assertEqual(vp.main(["--dir", str(files), "--manifest", str(mf), "--write"]), 1)
        self.assertEqual(mf.read_text(encoding="utf-8"), before)
        with quiet():
            self.assertEqual(vp.main(["--dir", str(files), "--manifest", str(mf), "--component", "intel-xess", "--write"]), 0)
        after = json.loads(mf.read_text(encoding="utf-8"))
        self.assertEqual(after["components"][1]["files"][0]["sha256"], hashlib.sha256(C).hexdigest())
        # only the sha256 line changed
        diff = [(x, y) for x, y in zip(before.splitlines(), mf.read_text(encoding="utf-8").splitlines()) if x != y]
        self.assertEqual(len(diff), 1)
        self.assertIn('"sha256"', diff[0][1])
        with quiet():
            self.assertEqual(vp.main(["--check", "--dir", str(files), "--manifest", str(mf), "--component", "intel-xess"]), 0)
        with noerr():
            self.assertEqual(vp.main(["--check", "--write", "--dir", str(files), "--manifest", str(mf)]), 2)
            self.assertEqual(vp.main(["--dir", str(self.dir / "none"), "--manifest", str(mf)]), 2)

    def test_the_shipped_manifest_is_in_the_tools_layout(self):
        text = vp.MANIFEST.read_text(encoding="utf-8")
        self.assertEqual(vp.dump(json.loads(text)), text, "a pin would rewrite more than the sha256 lines")
        m = json.loads(text)
        self.assertEqual([c["id"] for c in m["components"]], ["amd-fsr", "intel-xess"])
        for c in m["components"]:
            for f in c["files"]:
                self.assertRegex(f["gitBlob"], r"^[0-9a-f]{40}$")
                self.assertTrue(f["url"].startswith("https://raw.githubusercontent.com/"))


if __name__ == "__main__":
    unittest.main()
