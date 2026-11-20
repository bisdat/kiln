# SPDX-License-Identifier: MIT
"""Kiln's gem trigger programs (managed/services/Kiln.Abilities/Gems/programs, gem wave G2): each .kimp assembles to
the committed .bin that Kiln.Abilities embeds, passes the linter kiln.abilities' verifier follows, keeps an
impact.DamageSetup as its first such variable (the game writes the used gem's power there), and lays its kiln.Signal
payload out as Kiln.Abilities' GemHit.Parse reads it. No game files.

    python3 -m unittest discover -s tools/impact/tests -p 'test_gem_programs.py' -v
"""
import sys
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent))
sys.path.insert(0, str(HERE.parent.parent / "kfc"))

from impact import asm, catalog, lint, program as P  # noqa: E402

PROGRAMS = HERE.parents[2] / "managed" / "services" / "Kiln.Abilities" / "Gems" / "programs"
CAT = catalog.Catalog.default()
DAMAGE_SETUP = P.fnv1a32(b"impact.DamageSetup")
SIGNAL = P.fnv1a32(b"kiln.Signal")
HIT = {  # GemHit.Parse (GemRegistry.cs): offset in $hit, size
    "hAttacker": (0, 4), "hTarget": (4, 4), "hPart": (8, 4), "hHealth": (12, 4), "hPos": (16, 12), "hWeapon": (28, 4),
    "hSource": (32, 4), "hCrit": (36, 1), "hKill": (37, 1), "hBackstab": (38, 1), "hBlocked": (39, 1), "hEnemy": (40, 1),
}
# GemRegistry.Programs: GUID and kiln.Signal tag of each stem
EXPECTED = {
    "on_hit": ("6b696c6e-6765-6d73-8000-000000000001", 0x6B470001),
    "on_crit": ("6b696c6e-6765-6d73-8000-000000000002", 0x6B470002),
    "on_kill": ("6b696c6e-6765-6d73-8000-000000000003", 0x6B470003),
    "on_backstab": ("6b696c6e-6765-6d73-8000-000000000004", 0x6B470004),
    "on_hit_taken": ("6b696c6e-6765-6d73-8000-000000000005", 0x6B470005),
    "passive": ("6b696c6e-6765-6d73-8000-000000000006", 0x6B470006),
}


def build(stem):
    src = (PROGRAMS / f"{stem}.kimp").read_text()
    return asm.assemble(src, CAT, src=f"{stem}.kimp")


class GemPrograms(unittest.TestCase):
    def test_every_program_is_there(self):
        self.assertEqual(sorted(EXPECTED), sorted(p.stem for p in PROGRAMS.glob("*.kimp")))

    def test_the_committed_blob_is_the_assembled_source(self):
        for stem in EXPECTED:
            with self.subTest(stem):
                self.assertEqual(P.encode(build(stem)), (PROGRAMS / f"{stem}.bin").read_bytes(),
                                 f"{stem}.bin is stale: python3 tools/impact/impact.py asm {stem}.kimp -o {stem}.bin")

    def test_lint_passes_with_the_guid_and_id_kiln_expects(self):
        for stem, (guid, _) in EXPECTED.items():
            with self.subTest(stem):
                p = build(stem)
                self.assertEqual(guid, str(p.guid).lower())
                self.assertEqual(P.program_id(p.guid), p.id)
                rep = lint.lint(p, CAT)
                self.assertTrue(rep.ok, [f"{f.code} {f.msg}" for f in rep.errors])
                self.assertLessEqual(max(rep.max_depth.values()), 31)

    def test_the_first_damage_setup_is_the_power_placeholder(self):
        for stem in EXPECTED:
            with self.subTest(stem):
                first = next(v for v in build(stem).variables if v.type == DAMAGE_SETUP)
                self.assertEqual("power", first.dbg)
                self.assertEqual(0, first.config)

    def test_the_signal_payload_is_laid_out_as_GemHit_reads_it(self):
        for stem in EXPECTED:
            if stem == "passive":
                continue
            with self.subTest(stem):
                vs = {v.dbg: v for v in build(stem).variables}
                hit = vs["hit"]
                self.assertEqual(48, hit.size)
                for name, (off, size) in HIT.items():
                    self.assertEqual((hit.offset + off, size), (vs[name].offset, vs[name].size), name)

    def test_each_program_signals_with_its_own_tag(self):
        for stem, (_, tag) in EXPECTED.items():
            with self.subTest(stem):
                text = (PROGRAMS / f"{stem}.kimp").read_text()
                self.assertIn(f"kiln.Signal(", text)
                self.assertIn(f"{tag:#010x}", text.lower())
                self.assertEqual(1, sum(1 for line in text.splitlines() if "ecall kiln.Signal" in line))


if __name__ == "__main__":
    unittest.main()
