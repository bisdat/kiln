#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Generator rules = ShroudForge's (select_resource_profile.cpp, parse_resource_layout.cpp, configure_engine_layout.cpp)."""
import copy
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parent))
import generate as g  # noqa: E402

ROOT = Path(__file__).resolve().parents[2]
PROFILES = {p.parent.name: json.loads(p.read_text()) for p in sorted((ROOT / 'sigdb/engine').glob('*/*.json'))}
TARGET = {'client': 'enshrouded.exe', 'server': 'enshrouded_server.exe'}


def run(profile, side):
    size = profile['image']['size']
    g.parse_resource_layout(profile, size)
    return g.configure(profile, size, TARGET[side])


class Rules(unittest.TestCase):
    def mutated(self, side, edit):
        p = copy.deepcopy(PROFILES[side])
        edit(p)
        return p

    def refuses(self, side, edit, message):
        with self.assertRaises(g.ProfileError) as caught:
            run(self.mutated(side, edit), side)
        self.assertIn(message, str(caught.exception))

    def test_ported_profiles_configure(self):
        for side, p in PROFILES.items():
            layout, sections = run(p, side)
            self.assertTrue(layout['ecs'] and layout['authority'] and layout['write']['enabled'], side)
            self.assertEqual(sections & 0x6f, 0x6f)  # resource_read is set by load() after parse_resource_layout
            self.assertEqual(layout['dedicated_server'], side == 'server')

    def test_eligibility(self):
        for side, p in PROFILES.items():
            self.assertTrue(g.eligible(p, TARGET[side]))
            self.assertFalse(g.eligible(p, TARGET['server' if side == 'client' else 'client']))
            for edit in (lambda q: q['provenance'].update(status='approved'),
                         lambda q: q['provenance'].update(resourceReadValidated=False),
                         lambda q: q['provenance'].pop('resourceReadValidated'),
                         lambda q: q.update(kfcFormat=3), lambda q: q.update(schemaVersion=3),
                         lambda q: q['provenance'].update(resourceReadValidated='yes')):
                self.assertFalse(g.eligible(self.mutated(side, edit), TARGET[side]))

    def test_provenance_flags_gate_sections(self):
        cases = [('ecsReadValidated', 'ECS owner layout lacks read validation'),
                 ('ecsAuthorityReadValidated', 'local authority owner layout lacks read validation'),
                 ('ecsWriteGateValidated', 'ECS write gate lacks original authority/lifetime validation'),
                 ('ecsChangeTrackingValidated', 'ECS writes lack change tracking validation'),
                 ('resourceAtomicScalarWriteValidated', 'resource scalar writes lack original worker lifetime validation'),
                 ('servicePlainDataValidated', 'named service data lacks original mutable/lifetime validation'),
                 ('systemDependenciesValidated', 'system dependencies lack original scheduler validation'),
                 ('itemIdMapValidated', 'unproven original ItemRegistry map contract')]
        for side in PROFILES:
            for flag, message in cases:
                self.refuses(side, lambda q, f=flag: q['provenance'].pop(f), message)

    def test_offset_bounds_and_types(self):
        for side in PROFILES:
            size = PROFILES[side]['image']['size']
            self.refuses(side, lambda q: q['ecsWriteLayout'].update(threadCallbackRva=size),
                         'engine owner offset out of range: threadCallbackRva')
            self.refuses(side, lambda q: q['ecsWriteLayout'].update(sceneLockOffset=65537),
                         'engine owner offset out of range: sceneLockOffset')
            self.refuses(side, lambda q: q['ecsWriteLayout'].update(sceneLockOffset=-1),
                         'engine owner offset out of range: sceneLockOffset')
            self.refuses(side, lambda q: q['ecsWriteLayout'].update(sceneLockOffset='1496'), 'type must be number')
            self.refuses(side, lambda q: q['ecsWriteLayout'].pop('sceneLockOffset'), "key 'sceneLockOffset' not found")
            self.refuses(side, lambda q: q['ecsWriteLayout'].update(threadRegistryLockRva=q['ecsWriteLayout']['threadRegistryLockRva'] + 2),
                         'invalid native-thread ECS write gate layout')
            self.refuses(side, lambda q: q['ecsWriteLayout'].update(changeTrackerBytes=q['ecsWriteLayout']['changeTrackerBytes'].upper()),
                         'invalid change tracker guard bytes')
            self.refuses(side, lambda q: q['ecsWriteLayout'].update(changeTrackerBytes='00'), 'invalid original ECS change tracker')
            self.refuses(side, lambda q: q['ecsWriteLayout'].update(threadName=''), 'invalid native-thread ECS write gate layout')
            self.refuses(side, lambda q: q['ecsWriteLayout'].update(phasePolicy='all-components'),
                         'ECS write phase policy must be untracked-components')
            self.refuses(side, lambda q: q['ecsWriteLayout'].update(enabled='true'), 'type must be boolean')
            self.refuses(side, lambda q: q['ecsReadLayout'].update(maximumEntities=0), 'engine entity limit is zero')
            self.refuses(side, lambda q: q['ecsReadLayout'].pop('entityManagerKeys'),
                         'ECS entity hash membership needs both occupancy and keys offsets')
            self.refuses(side, lambda q: q['ecsReadLayout'].update(sceneEntryStride=16), 'invalid active scene entry layout')
            self.refuses(side, lambda q: q.pop('buildingPropSpawnLayout'), 'version 2 profile requires explicit queue and spawn layouts')
            self.refuses(side, lambda q: q['entityCommandQueueLayout'].update(extra=1), 'invalid original entity command queue layout')
            self.refuses(side, lambda q: q['entityCommandQueueLayout'].update(taskRecordStride=8),
                         'invalid original entity command queue extent')
            self.refuses(side, lambda q: q['buildingPropSpawnLayout'].update(sourceEntityIdsOffset=0),
                         'invalid building prop spawn value: sourceEntityIdsOffset')
            self.refuses(side, lambda q: q['resourceWriteLayout'].update(serviceInitializedValue=2),
                         'invalid resource content worker layout')
            self.refuses(side, lambda q: q['resourceLayout']['map'].update(keyStride=16),
                         'unsupported ResourceService map record layout')
            self.refuses(side, lambda q: q['resourceLayout'].update(resourceServiceIdCallbackRva=0),
                         'incomplete ResourceService profile layout')
            self.refuses(side, lambda q: q.update(schemaVersion=1),
                         'explicit queue and spawn layouts require profile format version 2')

    def test_dedicated_server_needs_registered_scene(self):
        self.refuses('server', lambda q: q['ownerLayout'].update(rootObjectRva=16),
                     'dedicated server requires a validated registered Scene owner layout')

    def test_version1_legacy_queue(self):
        p = copy.deepcopy(PROFILES['client'])
        p['schemaVersion'] = 1
        p.pop('entityCommandQueueLayout')
        p.pop('buildingPropSpawnLayout')
        layout, _ = run(p, 'client')
        self.assertEqual(layout['queue']['task_state'], 0x538)
        self.assertEqual(layout['queue']['execution_mode'], layout['entity_manager_offset'] + 8)
        self.assertEqual(layout['spawn'], {'source_entity_ids': 2088, 'allocation_failure_marker': 0})

    def test_check_mode_is_clean(self):
        self.assertEqual(g.generate(ROOT), (ROOT / g.OUTPUT).read_text(encoding='utf-8'))


def export_tree(root):
    """An exported copy of the generator's inputs and output under `root`."""
    for source in sorted((ROOT / 'sigdb/engine').glob('*/*.json')):
        target = root / source.relative_to(ROOT)
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(source, target)
    (root / g.OUTPUT).parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(ROOT / g.OUTPUT, root / g.OUTPUT)


class AlternateRoot(unittest.TestCase):
    """--root selects the whole tree: generated path comments are relative to it, never to the script's checkout."""

    def test_sibling_root_checks_clean(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp) / 'exported'
            export_tree(root)
            self.assertEqual(g.generate(root), (ROOT / g.OUTPUT).read_text(encoding='utf-8'))
            run = subprocess.run([sys.executable, '-I', str(Path(g.__file__).resolve()), '--check', '--root', str(root)],
                                 capture_output=True, text=True)
            self.assertEqual(run.returncode, 0, run.stderr)

    def test_nested_root_checks_clean(self):
        # A root below the script's own root: the path comments must not gain the nesting prefix.
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp) / 'outer' / 'inner'
            export_tree(root)
            with mock.patch.object(g, 'ROOT', Path(temp) / 'outer'):
                self.assertEqual(g.generate(root), (ROOT / g.OUTPUT).read_text(encoding='utf-8'))


if __name__ == '__main__':
    unittest.main(verbosity=1)
