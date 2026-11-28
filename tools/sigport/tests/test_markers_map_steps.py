# SPDX-License-Identifier: MIT
"""Read-only exact-build checks for kiln.markers' map rows at every client step (GitHub issue 159; sigdb feature
markers.client_map_steps in sigdb/symbols/client.d/dev28-04-markers.json; docs/re/map.md "The list in a frame"). These
tests never execute a game binary.

The game clears the native map list and fills it again at every client SimStep, once per frame, while Kiln's Post runs
only at the SimSteps that advance the client clock. kiln.markers therefore writes its rows again after every client
SimStep returns, from a link around SimStep. The checks pin what that rests on in client 26430625: the client world
update calls SimStep on frames without a new step too (ticked 0), SimStep runs the programs whether ticked or not, and
StepPrograms waits for the world's jobs before it returns.

    KILN_MARKERS_CLIENT_EXE=<read-only copy of enshrouded.exe 26430625> \\
        python3 -I -m unittest tools/sigport/tests/test_markers_map_steps.py
"""
import hashlib
import json
import os
from pathlib import Path
import struct
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from peimg import Image
from sides import merged
from sigdb_check import Resolver

ROOT = Path(__file__).resolve().parents[3]
ENV = "KILN_MARKERS_CLIENT_EXE"
SHA = "5d9ef2f140b8ed49e30dbee655841b89d08baec20f799a1817e171a18a850c77"  # client 26430625
FRAGMENT = ROOT / "sigdb/symbols/client.d/dev28-04-markers.json"
CATALOG = ROOT / "sigdb/programs/client" / f"{SHA}.json"
FEATURE = "markers.client_map_steps"
# client 26430625
UPDATE = 0xA1FAE0         # the client world update (0xa1fad0 jumps here for a world in state 2)
UPDATE_CALL = 0xA1FC9E    # its SimStep call
SIM_STEP = 0xA2C660
INPUT_APPLY = 0x9653E0    # the only use of SimStep's ticked argument (a server-world function: it returns for kind != 0)
STEP_PROGRAMS = 0x9ADE50
STEP_WAIT = 0x9AE686      # StepPrograms' wait for the world's task queue, before its only return
WAIT_TASK_GROUP = 0x8F0B30
CLIENT_CONFIG = 0x84D064  # the client world's creation config: +0x44 (run programs) = 1
CONFIG_COPY = 0x983213    # createWorld copies config +0x44 to world +0xe17959
MAP_CACHE = 0xDA4770      # the map canvas's marker layer: rebuilt when a list count changes, keeps entry pointers


class MapStepsDefinitions(unittest.TestCase):
    """What needs no exe: the fragment's shape and the programs' catalog."""

    def test_the_feature_names_the_step_function_and_its_two_constants(self):
        doc = json.loads(FRAGMENT.read_text())
        self.assertEqual("client", doc["side"])
        feature = doc["features"][FEATURE]
        self.assertEqual(["SimStep"], feature["symbols"])
        self.assertEqual(["storage.kind", "world.storage"], sorted(feature["constants"]))

    def test_the_client_composition_declares_what_the_feature_names(self):
        doc = merged(ROOT / "sigdb/symbols/client.json")
        self.assertIn(FEATURE, doc["features"])
        self.assertIn("SimStep", doc["symbols"])
        # the core's tick feature rests on the same names: the link sits on the function the core already hooks
        self.assertIn("SimStep", doc["features"]["tick"]["symbols"])
        for name in ("storage.kind", "world.storage"):
            self.assertIn(name, doc["features"]["tick"]["constants"])

    def test_the_server_composition_has_no_map_steps(self):
        doc = merged(ROOT / "sigdb/symbols/server.json")
        self.assertNotIn(FEATURE, doc.get("features", {}))

    def test_clear_and_replicate_are_client_programs_of_every_step(self):
        programs = {p["name"]: p for p in json.loads(CATALOG.read_text())["programs"]}
        clear, replicate = programs["clear_map_markers_for_ui"], programs["replicate_map_markers_for_ui"]
        self.assertEqual(0x5, clear["flags"])        # CLIENT | SYSTEM: one run per step
        self.assertEqual(0x201, replicate["flags"])  # CLIENT | CHUNKED: slices on the world's task queue
        for p in (clear, replicate):
            self.assertEqual(1, p["interval"])
            self.assertEqual(["G38UiPlayData"], p["lists"]["sys_write"])


class MapStepsSignatures(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        exe = os.environ.get(ENV)
        if not exe:
            raise unittest.SkipTest(f"set {ENV} to a read-only copy of enshrouded.exe 26430625")
        cls.exe = Path(exe)
        cls.image = Image(cls.exe)
        cls.resolver = Resolver(cls.image, merged(ROOT / "sigdb/symbols/client.json"))
        cls.syms, cls.consts, cls.features = cls.resolver.run()

    def at(self, rva, n):
        return bytes(self.image.img[rva:rva + n])

    def target(self, call):
        self.assertIn(self.image.img[call], (0xE8, 0xE9))
        return call + 5 + struct.unpack_from("<i", self.image.img, call + 1)[0]

    def test_exact_build(self):
        self.assertEqual(SHA, hashlib.sha256(self.exe.read_bytes()).hexdigest())

    def test_the_feature_resolves_on_the_core_names(self):
        self.assertTrue(self.features.get(FEATURE))
        self.assertTrue(self.features.get("tick"))
        self.assertEqual(SIM_STEP, self.syms["SimStep"]["rva"])
        self.assertEqual(8, self.consts["storage.kind"]["value"])
        self.assertEqual(0xE179C0, self.consts["world.storage"]["value"])

    def test_the_client_world_update_steps_every_frame(self):
        # ticked = step counter [world+0xe17e9c] != previous [world+0xe17ea0]
        self.assertEqual(bytes.fromhex("8b a9 9c 7e e1 00"), self.at(UPDATE + 0xB, 6))
        self.assertEqual(bytes.fromhex("44 8b b9 a0 7e e1 00 41 3b ef 41 0f 95 c4"), self.at(0xA1FB24, 14))
        # no new step: SimStep all the same unless byte [world+0x15f55cc] is set, with ticked = r12b (0)
        self.assertEqual(bytes.fromhex("41 80 be cc 55 5f 01 00 75 43 41 0f b6 d4 48 8b cb"), self.at(0xA1FC8D, 17))
        self.assertEqual(SIM_STEP, self.target(UPDATE_CALL))

    def test_sim_step_runs_the_programs_whether_ticked_or_not(self):
        # ticked (dl) goes to ebx and is tested once, around the input apply; ebx is a loop counter after that
        self.assertEqual(bytes.fromhex("0f b6 da"), self.at(0xA2C683, 3))
        self.assertEqual(bytes.fromhex("84 db 74 08 48 8b ce"), self.at(0xA2C6A5, 7))
        self.assertEqual(INPUT_APPLY, self.target(0xA2C6AC))
        self.assertEqual(bytes.fromhex("41 8b dd"), self.at(0xA2C703, 3))  # mov ebx, r13d
        # the programs' gate is the world's byte +0xe17959 (storage R = world + 0x928), not ticked
        self.assertEqual(bytes.fromhex("45 38 a9 31 70 e1 00 0f 84"), self.at(0xA2C6C7, 9))
        self.assertEqual(0x928 + 0xE17031, 0xE17959)
        self.assertEqual(bytes.fromhex("41 0f b6 46 44 88 87 59 79 e1 00"), self.at(CONFIG_COPY, 11))
        self.assertEqual(bytes.fromhex("66 c7 45 44 01 00"), self.at(CLIENT_CONFIG, 6))
        self.assertEqual(bytes.fromhex("c6 45 00 02"), self.at(CLIENT_CONFIG + 0xA, 4))  # kind byte 2: the client world
        # StepPrograms until it says done
        self.assertEqual(bytes.fromhex("48 8d 95 38 00 02 00 48 8b ce"), self.at(0xA2C870, 10))
        self.assertEqual(STEP_PROGRAMS, self.target(0xA2C87A))
        self.assertEqual(bytes.fromhex("84 c0 74 ed"), self.at(0xA2C87F, 4))

    def test_step_programs_waits_for_the_worlds_jobs_before_it_returns(self):
        # [rsp+0x28] = R + 0xe17038 = world + 0xe17960 (world.jobs), read at the start
        self.assertEqual(bytes.fromhex("48 8b 80 38 70 e1 00"), self.at(0x9ADEF2, 7))
        self.assertEqual(bytes.fromhex("48 89 44 24 28"), self.at(0x9ADF00, 5))
        self.assertEqual(0x928 + 0xE17038, 0xE17960)
        self.assertEqual(bytes.fromhex("48 8b 4c 24 28"), self.at(STEP_WAIT - 5, 5))
        self.assertEqual(WAIT_TASK_GROUP, self.target(STEP_WAIT))
        self.assertEqual(WAIT_TASK_GROUP, self.syms["WaitTaskGroup"]["rva"])  # the core's WaitWorldJobs jumps there
        self.assertEqual(self.syms["WaitTaskGroup"]["rva"], self.target(self.syms["WaitWorldJobs"]["rva"] + 7))

    def test_the_map_reads_the_list_by_its_count(self):
        # the canvas's marker layer compares the normal list's count with the one it built from, and keeps entry pointers
        count = self.consts["markers/main.count"]["value"]
        self.assertEqual(0x36EA08, count)
        self.assertEqual(bytes.fromhex("4d 8b 94 24") + struct.pack("<I", count), self.at(MAP_CACHE + 0x34, 8))
        self.assertEqual(bytes.fromhex("4c 39 91 c0 c1 00 00"), self.at(MAP_CACHE + 0x58, 7))


if __name__ == "__main__":
    unittest.main()
