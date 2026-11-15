# SPDX-License-Identifier: MIT
"""The Python fingerprint pass (abifp.py) against the shared vector file, whose answers are the core's
(fingerprint-vectors.json, written by make_vectors.py; native/core/tests/hooks_fingerprint_test.cpp checks the same
file against the core's pass):

    python3 -m unittest discover -s tools/sigport/tests -v
The game exes' functions run when SIGPORT_SERVER / SIGPORT_CLIENT name the exes (their sha256 must be the file's);
KILN_FPTOOL=<kiln-fptool> also runs the core's pass on them (tools/sigport/fptool/build.sh)."""
import json
import hashlib
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent))

import abifp  # noqa: E402
from port import descriptors  # noqa: E402

VECTORS = json.loads((HERE / "fingerprint-vectors.json").read_text())
REPO = HERE.parents[2]


def snippet(v):
    code = bytes.fromhex(v["hex"])
    base = int(v["base"], 16)
    return abifp.compute(lambda a, n: code[a - base:a - base + n] if base <= a < base + len(code) else b"", base)


class Snippets(unittest.TestCase):
    def test_every_snippet_has_the_cores_fingerprint(self):
        self.assertGreaterEqual(len(VECTORS["snippets"]), 30)
        for v in VECTORS["snippets"]:
            with self.subTest(v["name"]):
                self.assertEqual(abifp.to_dict(snippet(v)), v["expect"], "\n".join(v["asm"]))

    def test_every_descriptor_case_has_the_cores_verdict_and_text(self):
        snips = {v["name"]: v for v in VECTORS["snippets"]}
        for c in VECTORS["sigs"]:
            with self.subTest(c["name"]):
                try:
                    sig = abifp.parse_sig(c["sig"], VECTORS["structs"], allow_trust=c.get("trust_allowed", False))
                except ValueError:
                    self.assertEqual(c["expect"]["verdict"], "bad")  # the texts of malformed descriptors are the core's own
                    continue
                self.assertEqual(dict(zip(("verdict", "text"), abifp.check_sig(sig, snippet(snips[c["snippet"]])))), c["expect"])

    def test_the_lab_refusal_text_names_ecsnow(self):
        c = next(c for c in VECTORS["sigs"] if c["name"] == "time_names_ecsnow")
        self.assertEqual(c["expect"], {"verdict": "mismatch", "text": "descriptor arg 2 'ecsNow' is rdx, the function reads [rcx, xmm1, r8]"})

    def test_a_fingerprint_survives_its_json(self):
        for v in VECTORS["snippets"]:
            self.assertEqual(abifp.to_dict(abifp.from_dict(v["expect"])) | {"insns": v["expect"]["insns"]}, v["expect"], v["name"])


class Exes(unittest.TestCase):
    def exe(self, side):
        path = os.environ.get({"server": "SIGPORT_SERVER", "client": "SIGPORT_CLIENT"}[side])
        if not path:
            self.skipTest(f"SIGPORT_{side.upper()} is not set")
        rows = [e for e in VECTORS["exes"] if e["side"] == side]
        if hashlib.sha256(Path(path).read_bytes()).hexdigest() != rows[0]["sha256"]:
            self.skipTest(f"{path} is not the {side} build of the vector file")
        return path, rows

    def test_the_described_functions_have_the_cores_fingerprints(self):
        for side in ("server", "client"):
            with self.subTest(side):
                path, rows = self.exe(side)
                code = abifp.ExeCode(path)
                for e in rows:
                    self.assertEqual(code.fingerprint(int(e["rva"], 16)), e["expect"], e["symbol"])

    def test_kiln_fptool_agrees(self):
        tool = os.environ.get("KILN_FPTOOL")
        if not tool:
            self.skipTest("KILN_FPTOOL is not set")
        for side in ("server", "client"):
            with self.subTest(side):
                path, rows = self.exe(side)
                with tempfile.NamedTemporaryFile("w", suffix=".json") as t:
                    json.dump({"rvas": [int(e["rva"], 16) for e in rows]}, t)
                    t.flush()
                    got = json.loads(subprocess.run([tool, path, t.name], capture_output=True, text=True, check=True).stdout)["rvas"]
                for e in rows:
                    self.assertEqual(got[e["rva"]], e["expect"], e["symbol"])

    def test_the_cores_descriptors_fit_both_builds(self):
        # the sigdb descriptors against the recorded fingerprints of the builds in the file: what the core's load check
        # says, without the exes
        for side in ("server", "client"):
            descs, structs = descriptors(REPO / "sigdb" / "symbols" / f"{side}.json")
            rows = [e for e in VECTORS["exes"] if e["side"] == side]
            self.assertEqual(sorted(descs), sorted(e["symbol"] for e in rows), side)
            for e in rows:
                v, t = abifp.check_sig(abifp.parse_sig(descs[e["symbol"]], structs), abifp.from_dict(e["expect"]))
                self.assertEqual(v, "match", f"{side} {e['symbol']}: {t}")


if __name__ == "__main__":
    unittest.main()
