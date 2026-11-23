# SPDX-License-Identifier: MIT
"""Offline checks of the lab peer's NPC-body properties; no lab or game needed."""
import hashlib
import unittest

import handshake


class HandshakePropertiesTests(unittest.TestCase):
    def test_multiple_bodies_use_comma_list_but_newline_hash(self):
        lines = ["m:bram:" + "a" * 64, "m:tobin:" + "b" * 64]
        props = handshake.props(lines)
        self.assertEqual(",".join(lines), props["kiln.npcbodies.list"])
        self.assertEqual(hashlib.sha256("\n".join(lines).encode()).hexdigest(), props["kiln.npcbodies"])
        self.assertEqual("2", props["kiln.npcbodies.count"])

    def test_no_bodies_has_no_list_and_an_empty_hash(self):
        props = handshake.props([])
        self.assertNotIn("kiln.npcbodies.list", props)
        self.assertEqual("", props["kiln.npcbodies"])
        self.assertEqual("0", props["kiln.npcbodies.count"])


if __name__ == "__main__":
    unittest.main()
