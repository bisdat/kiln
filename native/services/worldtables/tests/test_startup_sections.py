#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Keep the startup whitelist in step with all of read_blocks' section readers.

The Windows plugin cannot run in the host harness. Check its actual gate here;
run.sh also exercises singleton tool documents with the portable native reader.
"""
from pathlib import Path
import re
import unittest


root = Path(__file__).resolve().parent.parent
merge = (root / "merge.cpp").read_text()
terrain = (root / "terrain.cpp").read_text()
startup = (root / "worldtables.cpp").read_text()
reader = startup.split("void read_mod(", 1)[1].split("\n// The per-world record:", 1)[0]
gate = reader.split("if (!doc.is_object() ||", 1)[1].split("read_blocks(doc, &mf);", 1)[0]
accepted = set(re.findall(r'doc\.contains\("([^"]+)"\)', gate))
handled = set(re.findall(r'doc\.find\("([^"]+)"\)', merge + terrain))
# read_crafting_props also iterates the prop-kind sections through kSections.
for sections in re.findall(r'kSections\[\]\s*=\s*\{([^}]+)\}', merge):
    handled.update(re.findall(r'"([^"]+)"', sections))


class StartupSections(unittest.TestCase):
    pass


def singleton_section_test(section):
    def test(self):
        document = {section: {}}
        self.assertTrue(
            accepted.intersection(document),
            f"startup skips a {section}-only document handled by read_blocks; no sentinel section should be needed",
        )
    return test


for section in sorted(handled):
    setattr(StartupSections, f"test_{section}_only", singleton_section_test(section))


if __name__ == "__main__":
    unittest.main()
