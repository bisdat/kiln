#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Tests of tools/registry-compat/compat.py on made-up dumps (no game data).

    python3 -I tools/registry-compat/tests/test_compat.py
"""
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import compat  # noqa: E402

BAL = "82706b40-61b1-4b8f-8b23-dcec6971bda1"


def res(guid, sha, t=0x1234):
    return {"type": "t", "typeHash": t, "guid": guid, "part": 0, "size": 4, "sha256": sha}


def dump(resources, changed=()):
    return {"resources": resources, "store": {"changed": [{"registry": r, "entry": e, "field": f, "value": v} for r, e, f, v in changed]}}


def archive(pairs):
    return {"resources": {compat.key_of(res(g, s)): ({"size": 4, "sha256": s} if s else None) for g, s in pairs}}


class Compat(unittest.TestCase):
    def test_untouched_resources_match_the_archive(self):
        d = dump([res("a", "1"), res(BAL, "2")])
        same, allowed, other, new = compat.check(d, archive([("a", "1"), (BAL, "9")]), [BAL])
        self.assertEqual((same, len(allowed), other, new), (1, 1, [], []))

    def test_a_changed_resource_the_registry_does_not_touch_is_a_failure(self):
        same, allowed, other, _ = compat.check(dump([res("a", "1x")]), archive([("a", "1")]), [BAL])
        self.assertEqual(len(other), 1)

    def test_a_resource_the_game_lacks_is_counted_apart(self):
        _, _, other, new = compat.check(dump([res("n", "5")]), archive([]), [])
        self.assertEqual((other, len(new)), ([], 1))

    def test_diff_allows_only_the_named_fields_and_resources(self):
        before = dump([res("a", "1"), res(BAL, "2")], [("items", "Wood", "maxStackSize", {"original": 10, "current": 20})])
        after = dump([res("a", "1"), res(BAL, "3")], [("items", "Wood", "maxStackSize", {"original": 10, "current": 80}),
                                                      ("items", "Fibre", "maxStackSize", {"original": 50, "current": 500})])
        self.assertEqual(compat.diff(before, after, [BAL], ["items:Wood.maxStackSize", "items:Fibre.maxStackSize"]), ([], []))
        res_d, fl = compat.diff(before, after, [], ["items:Wood.maxStackSize"])
        self.assertEqual(len(res_d), 1)
        self.assertEqual(fl, ["items:Fibre.maxStackSize"])

    def test_no_residue_is_an_empty_diff(self):
        d = dump([res("a", "1")], [("items", "Wood", "maxStackSize", {"original": 10, "current": 20})])
        self.assertEqual(compat.diff(d, d, [], []), ([], []))


if __name__ == "__main__":
    unittest.main()
