# SPDX-License-Identifier: MIT
"""Offline checks of the lab peers' camera Hello; no lab or game needed."""
import os
import re
import struct
import unittest

import camnet

HERE = os.path.dirname(os.path.abspath(__file__))


class CamnetHelloTests(unittest.TestCase):
    def test_hello_names_the_player_camera_test_maps_the_peer_to(self):
        # ServerBinding binds a Hello only when its LocalEntity is the connection's player (LabPlayerOf in the lab)
        with open(os.path.join(HERE, "CameraTest.cs"), encoding="utf-8") as f:
            mapped = {n: int(e) for n, e in re.findall(r'\["(camtest-\w)"\]\s*=\s*(\d+)', f.read())}
        self.assertEqual(mapped, camnet.LAB_PLAYERS)
        for name in ("camtest-A", "camtest-N"):
            peer = camnet.Peer(1, name, camnet.POSE)
            try:
                _, _, _, local_entity, _, _, _ = struct.unpack("<HQIIQHI", peer.hello_body())
            finally:
                peer.s.close()
            self.assertEqual(mapped[name], local_entity)


if __name__ == "__main__":
    unittest.main()
