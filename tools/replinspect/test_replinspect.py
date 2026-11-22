#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Rule tests for replinspect: one test per wire-format rule of the replication chapter (§2.3), on hand-made vectors.

    python3 -m unittest discover -s tools/replinspect -p 'test_*.py'
"""
import contextlib
import io
import json
import os
import struct
import tempfile
import unittest

import replinspect as ri

GUID = bytes(range(16))
REGISTRY = [
    {'index': 0, 'name': 'Position', 'hash': 0x11111111, 'size': 12, 'flags': 1 | 8},
    {'index': 1, 'name': 'Health', 'hash': 0x22222222, 'size': 4, 'flags': 1 | 2 | 8},
    {'index': 2, 'name': 'Custom', 'hash': 0x33333333, 'size': 8, 'flags': 1 | 4 | 16},
    {'index': 5, 'name': 'Inventory', 'hash': 0x44444444, 'size': 300, 'flags': 1},
]


class BitWriter:
    def __init__(self):
        self.bits = []

    def put(self, v, n):
        self.bits += [(v >> i) & 1 for i in range(n)]

    def bytes(self, pad=0):
        bits = self.bits + [(pad >> i) & 1 for i in range(-len(self.bits) % 8)]
        return bytes(sum(bits[i + j] << j for j in range(8)) for i in range(0, len(bits), 8))


def payload(removed=(), added=(), changed=(), pad=0):
    """added: (idx, version|None); changed: (idx, version|None, blob, is_delta)."""
    w = BitWriter()
    w.put((1 if removed else 0) | (2 if added else 0) | (4 if changed else 0), 3)
    if removed:
        w.put(len(removed), 6)
    if added:
        w.put(len(added), 6)
    if changed:
        w.put(len(changed), 8)
    for i in removed:
        w.put(i, 10)
    for i, v in added:
        w.put(i, 10)
        w.put(v is not None, 1)
        if v is not None:
            w.put(v, 8)
    for i, v, blob, delta in changed:
        w.put(i, 10)
        w.put(v is not None, 1)
        if v is not None:
            w.put(v, 8)
        w.put(len(blob) << 1 | int(delta), 16)
    return w.bytes(pad) + b''.join(c[2] for c in changed)


def create_msg(tick, seq, entity, p, parent=0, u8=0):
    return struct.pack('<IIII', tick << 3 | 2, seq, entity, parent) + GUID + bytes([u8]) + struct.pack('<H', len(p)) + p


def update_msg(tick, seq, entity, base_word, p):
    guid = GUID if base_word & 0x7FFFFFFF == 0 else b''
    return struct.pack('<IIII', tick << 3 | 3, seq, entity, base_word) + guid + struct.pack('<H', len(p)) + p


def destroy_msg(tick, seq, a, b):
    return struct.pack('<IIBB', tick << 3 | 4, seq, len(a), len(b)) + b''.join(struct.pack('<I', i) for i in a + b)


def rec(kind, raw, tick, seq, entity=0, base=0, ch=4, **extra):
    msg = {'create': 4, 'update': 4, 'destroy': 5, 'frame': 0, 'handshake': 2}[kind]
    r = {'type': 'msg', 't': 1700000000000, 'peer': 7, 'ch': ch, 'msg': msg, 'kind': kind, 'entity': entity,
         'tick': tick, 'seq': seq, 'base': base, 'bytes': len(raw), 'gap': False, 'raw': raw.hex()}
    r.update(extra)
    return r


def capture():
    cap = ri.Capture('mem')
    cap.header = {'type': 'header', 'version': 1, 'peer': 7, 'seconds': 5, 'utc': '2026-11-20T12:00:00Z',
                  'registry': REGISTRY}
    ri._read_header(cap, 1, cap.header)
    return cap


def check(r):
    return ri.check_message(capture(), r)


class EntityHeader(unittest.TestCase):
    def test_create_header(self):
        raw = create_msg(100, 9, 4242, b'\x00', parent=77)
        m = ri.parse_entity(raw)
        self.assertEqual((m['kind_code'], m['tick'], m['seq'], m['entity'], m['parent']), (2, 100, 9, 4242, 77))
        self.assertEqual((m['guid'], m['u8'], m['plen'], m['header_len'], m['trailing']), (GUID.hex(), 0, 1, 35, 0))

    def test_create_header_cut_short(self):
        with self.assertRaises(ri.FormatError):
            ri.parse_entity(create_msg(100, 9, 4242, b'\x00')[:20])

    def test_update_header_baseline_zero_has_guid(self):
        m = ri.parse_entity(update_msg(5, 1, 3, 0, b'\x00'))
        self.assertEqual((m['base'], m['base_acked'], m['guid'], m['header_len']), (0, False, GUID.hex(), 34))

    def test_update_header_without_guid(self):
        m = ri.parse_entity(update_msg(5, 1, 3, 90, b'\x00'))
        self.assertEqual((m['base'], m['base_acked'], 'guid' in m, m['header_len']), (90, False, False, 18))

    def test_update_header_bit31_is_acked_state(self):
        m = ri.parse_entity(update_msg(5, 1, 3, 0x80000000 | 90, b'\x00'))
        self.assertEqual((m['base'], m['base_acked'], 'guid' in m), (90, True, False))
        m = ri.parse_entity(update_msg(5, 1, 3, 0x80000000, b'\x00'))  # acked bit, baseline 0: still a GUID
        self.assertEqual((m['base'], m['base_acked'], 'guid' in m), (0, True, True))

    def test_update_base_field_masked_or_whole(self):
        raw = update_msg(5, 1, 3, 0x80000000 | 90, b'\x00')
        self.assertEqual(check(rec('update', raw, 5, 1, 3, base=90))[0], [])
        self.assertEqual(check(rec('update', raw, 5, 1, 3, base=0x80000000 | 90))[0], [])
        self.assertTrue(check(rec('update', raw, 5, 1, 3, base=91))[0])

    def test_trailing_bytes_after_payload(self):
        probs = check(rec('create', create_msg(1, 1, 1, b'\x00') + b'\x00', 1, 1, 1))[0]
        self.assertTrue(any('after the payload' in p for p in probs), probs)


class PayloadBits(unittest.TestCase):
    def test_flags_only(self):
        p = ri.parse_payload(b'\x00')
        self.assertEqual((p['flags'], p['header_bytes'], p['removed'], p['added'], p['changed']), (0, 1, [], [], []))

    def test_removed_list_literal_bits(self):
        # flags 001 (bits 0-2), count 1 (bits 3-8), idx 5 (bits 9-18): 19 bits, LSB first
        p = ri.parse_payload(bytes([0x09, 0x0A, 0x00]))
        self.assertEqual((p['flags'], p['removed'], p['header_bytes'], p['pad_bits'], p['left_over']), (1, [5], 3, 5, 0))

    def test_added_list_with_and_without_version(self):
        p = ri.parse_payload(payload(added=[(1, 200), (0, None)]))
        self.assertEqual(p['added'], [{'index': 1, 'version': 200}, {'index': 0, 'version': None}])
        self.assertEqual(p['header_bytes'], (3 + 6 + 19 + 11 + 7) // 8)

    def test_changed_list_versions(self):
        p = ri.parse_payload(payload(changed=[(1, 3, b'\x01\x02\x03\x04', False), (0, None, bytes(12), False)]))
        self.assertEqual([(c['index'], c['version']) for c in p['changed']], [(1, 3), (0, None)])
        self.assertEqual(p['left_over'], 0)

    def test_all_three_lists(self):
        p = ri.parse_payload(payload(removed=[2], added=[(5, None)], changed=[(1, None, bytes(4), False)]))
        self.assertEqual((p['flags'], p['removed'], [a['index'] for a in p['added']]), (7, [2], [5]))
        self.assertEqual((p['blob_bytes'], p['left_over']), (4, 0))

    def test_size_field_is_size_shl1_or_delta(self):
        w = BitWriter()
        w.put(4, 3), w.put(1, 8), w.put(5, 10), w.put(0, 1), w.put(300 << 1 | 1, 16)
        p = ri.parse_payload(w.bytes() + bytes(300))
        c = p['changed'][0]
        self.assertEqual((c['index'], c['size'], c['delta'], p['header_bytes']), (5, 300, True, 5))

    def test_padding_to_a_byte(self):
        # flags 100, count 1 (8 bits), idx 10, v 0, size 16: 38 bits -> 5 bytes, 2 pad bits
        raw = payload(changed=[(1, None, bytes(4), False)])
        p = ri.parse_payload(raw)
        self.assertEqual((p['header_bytes'], p['pad_bits'], p['pad_value'], p['blob_bytes']), (5, 2, 0, 4))
        bad = ri.parse_payload(payload(changed=[(1, None, bytes(4), False)], pad=3))
        self.assertEqual(bad['pad_value'], 3)
        msg = create_msg(1, 1, 1, payload(changed=[(1, None, bytes(4), False)], pad=3))
        probs, warns, _ = check(rec('create', msg, 1, 1, 1))
        self.assertEqual(probs, [])
        self.assertTrue(any('padding' in w for w in warns))

    def test_bit_header_past_payload(self):
        with self.assertRaises(ri.FormatError):
            ri.parse_payload(b'\x04')  # changed list present, count needs more bits

    def test_blob_past_payload(self):
        p = ri.parse_payload(payload(changed=[(1, None, bytes(4), False)])[:-1])
        self.assertIsNotNone(p['cut'])

    def test_leftover_byte_fails(self):
        raw = create_msg(1, 1, 1, payload(changed=[(1, None, bytes(4), False)]) + b'\x00')
        probs = check(rec('create', raw, 1, 1, 1))[0]
        self.assertTrue(any('left over' in p for p in probs), probs)


class Blobs(unittest.TestCase):
    def test_raw_blob_equals_components(self):
        raw = create_msg(1, 1, 1, payload(changed=[(1, 0, b'\x0a\x00\x00\x00', False)]))
        self.assertEqual(check(rec('create', raw, 1, 1, 1, components={'Health': '0a000000'}))[0], [])
        probs = check(rec('create', raw, 1, 1, 1, components={'Health': '0b000000'}))[0]
        self.assertTrue(any('differs' in p for p in probs), probs)

    def test_raw_blob_size_against_registry(self):
        raw = create_msg(1, 1, 1, payload(changed=[(1, None, bytes(3), False)]))
        probs = check(rec('create', raw, 1, 1, 1))[0]
        self.assertTrue(any('registry size is 4' in p for p in probs), probs)

    def test_raw_fallback_is_clean(self):
        # a 4-byte change: the generic delta would not be smaller, so the writer sends it raw
        raw = update_msg(9, 2, 1, 8, payload(changed=[(1, None, b'\x01\x02\x03\x04', False)]))
        probs, warns, parsed = check(rec('update', raw, 9, 2, 1, base=8))
        self.assertEqual((probs, warns, parsed['p']['changed'][0]['delta']), ([], [], False))

    def test_delta_not_smaller_than_raw_warns(self):
        blob = bytes([0, 3, 1, 2, 3])  # 5 bytes for a 4-byte component
        raw = update_msg(9, 2, 1, 8, payload(changed=[(1, None, blob, True)]))
        probs, warns, _ = check(rec('update', raw, 9, 2, 1, base=8))
        self.assertEqual(probs, [])
        self.assertTrue(any('not smaller' in w for w in warns), warns)

    def test_custom_codec_blob_is_only_sized(self):
        raw = update_msg(9, 2, 1, 8, payload(changed=[(2, None, b'\xff\xff\xff', True)]))
        self.assertEqual(check(rec('update', raw, 9, 2, 1, base=8))[0], [])


class GenericDelta(unittest.TestCase):
    def test_runs(self):
        runs, covered = ri.parse_generic_delta(bytes([2, 1, 0xAA, 3, 2, 0xBB, 0xCC]))
        self.assertEqual((runs, covered), ([(2, 2, 1), (6, 3, 2)], 8))

    def test_ff_continuation(self):
        runs, covered = ri.parse_generic_delta(bytes([0xFF, 0x00, 1, 0xAA]))  # 255 + 0
        self.assertEqual(runs, [(255, 255, 1)])
        runs, covered = ri.parse_generic_delta(bytes([0xFF, 0xFF, 0x02, 0x01, 0xAA]))  # 255 + 255 + 2
        self.assertEqual((runs, covered), ([(512, 512, 1)], 513))
        runs, _ = ri.parse_generic_delta(bytes([0, 0xFF, 0x01]) + bytes(256))  # a changed count of 256
        self.assertEqual(runs, [(0, 0, 256)])

    def test_run_ending_at_the_blob_end_and_component_end(self):
        blob = bytes([0xFF, 0x2B, 2, 0xAA, 0xBB])  # skip 298, change 2: ends at byte 300 of Inventory
        self.assertEqual(ri.parse_generic_delta(blob), ([(298, 298, 2)], 300))
        raw = update_msg(9, 2, 1, 8, payload(changed=[(5, None, blob, True)]))
        self.assertEqual(check(rec('update', raw, 9, 2, 1, base=8))[0], [])

    def test_run_past_the_component(self):
        blob = bytes([0xFF, 0x2C, 2, 0xAA, 0xBB])  # skip 299 + 2 = 301 > 300
        raw = update_msg(9, 2, 1, 8, payload(changed=[(5, None, blob, True)]))
        probs = check(rec('update', raw, 9, 2, 1, base=8))[0]
        self.assertTrue(any('covers 301' in p for p in probs), probs)

    def test_count_cut_short(self):
        with self.assertRaises(ri.FormatError):
            ri.parse_generic_delta(bytes([2, 0xFF]))

    def test_changed_bytes_past_blob(self):
        with self.assertRaises(ri.FormatError):
            ri.parse_generic_delta(bytes([0, 3, 0xAA, 0xBB]))

    def test_trailing_unchanged_count_fails(self):
        # "nothing after the last changed byte": a lone trailing count is a cut-short run
        with self.assertRaises(ri.FormatError):
            ri.parse_generic_delta(bytes([0, 1, 0xAA, 5]))

    def test_empty_changed_run_fails(self):
        with self.assertRaises(ri.FormatError):
            ri.parse_generic_delta(bytes([3, 0]))


class DestroyList(unittest.TestCase):
    def test_lists_a_and_b(self):
        d = ri.parse_destroy(destroy_msg(2552, 871, [2542], [10, 11]))
        self.assertEqual((d['kind_code'], d['tick'], d['seq'], d['a'], d['b'], d['trailing']),
                         (4, 2552, 871, [2542], [10, 11], 0))
        r = rec('destroy', destroy_msg(2552, 871, [2542], [10, 11]), 2552, 871, a=[2542], b=[10, 11])
        self.assertEqual(check(r)[0], [])

    def test_cut_short(self):
        with self.assertRaises(ri.FormatError):
            ri.parse_destroy(destroy_msg(1, 1, [1, 2], [])[:-2])
        r = rec('destroy', destroy_msg(1, 1, [1, 2], [])[:-2], 1, 1, a=[1, 2], b=[])
        self.assertTrue(any('cut short' in p for p in check(r)[0]))

    def test_json_lists_must_agree(self):
        r = rec('destroy', destroy_msg(1, 1, [1], [2]), 1, 1, a=[2], b=[1])
        self.assertEqual(len(check(r)[0]), 2)


class Fields(unittest.TestCase):
    def test_unknown_registry_index(self):
        raw = create_msg(1, 1, 1, payload(added=[(9, None)]))
        self.assertTrue(any('index 9' in p for p in check(rec('create', raw, 1, 1, 1))[0]))

    def test_kind_and_entry_type_must_agree(self):
        raw = create_msg(1, 1, 1, b'\x00')
        r = rec('create', raw, 1, 1, 1)
        r['msg'] = 5
        self.assertTrue(check(r)[0])
        r = rec('update', raw, 1, 1, 1)  # header kind 2 under kind "update"
        self.assertTrue(any('header kind' in p for p in check(r)[0]))

    def test_frames_are_counted_not_parsed(self):
        self.assertEqual(check(rec('frame', b'\x01\x02\x03', 1, 1)), ([], [], None))
        self.assertTrue(check(rec('frame', b'\x01\x02\x03', 1, 1, bytes=4))[0])


class Files(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.TemporaryDirectory()
        self.addCleanup(self.dir.cleanup)

    def write(self, records, end=True, tail=''):
        path = os.path.join(self.dir.name, 'repl-7-20261120T120000Z.jsonl')
        lines = [{'type': 'header', 'version': 1, 'peer': 7, 'seconds': 5, 'utc': '2026-11-20T12:00:00Z',
                  'registry': REGISTRY, 'future': 'ignored'}] + records
        if end:
            lines.append({'type': 'end', 'messages': len(records), 'dropped': 0, 'utc': '2026-11-20T12:00:05Z'})
        with open(path, 'w') as f:
            f.write(''.join(json.dumps(x) + '\n' for x in lines) + tail)
        return path

    def run_main(self, *argv):
        out = io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(io.StringIO()):
            code = ri.main(list(argv))
        return code, out.getvalue()

    def records(self):
        return [
            rec('handshake', b'\x02\x00', 100, 1),
            rec('frame', b'\x00\x01\x02\x03', 101, 2),
            rec('create', create_msg(101, 3, 40, payload(changed=[(0, None, bytes(range(12)), False),
                                                                  (1, 4, b'\x64\x00\x00\x00', False)]), parent=0),
                101, 3, 40, components={'Position': bytes(range(12)).hex(), 'Health': '64000000'}, newfield=1),
            rec('update', update_msg(105, 4, 40, 0x80000000 | 101, payload(changed=[(0, None, bytes([4, 1, 0x7F]), True)])),
                105, 4, 40, base=101, ch=5),
            rec('destroy', destroy_msg(110, 5, [40], []), 110, 5, a=[40], b=[], gap=True),
        ]

    def test_small_capture_checks_clean(self):
        path = self.write(self.records())
        code, out = self.run_main('check', path)
        self.assertEqual(code, 0, out)
        self.assertIn('replinspect: OK', out)
        code, out = self.run_main('check', path, '--json')
        s = json.loads(out)
        self.assertEqual((code, s['ok'], s['messages'], s['clean'], s['entities'], s['gaps'], s['complete']),
                         (0, True, 5, 5, 1, 1, True))
        self.assertEqual(s['by_channel']['5']['messages'], 1)

    def test_mismatching_json_field_fails(self):
        records = self.records()
        records[3]['seq'] = 99
        code, out = self.run_main('check', self.write(records))
        self.assertEqual(code, 1)
        self.assertIn('line 5: "seq" is 99, the raw header says 4', out)

    def test_no_end_line_is_read_with_a_warning(self):
        code, out = self.run_main('check', self.write(self.records(), end=False, tail='{"type":"msg","t":'))
        self.assertEqual(code, 0, out)
        self.assertIn('no end line', out)
        self.assertIn('cut off', out)

    def test_not_a_capture(self):
        path = os.path.join(self.dir.name, 'x.jsonl')
        with open(path, 'w') as f:
            f.write('{"type":"msg"}\n')
        self.assertEqual(self.run_main('check', path)[0], 2)

    def test_registry_dump(self):
        code, out = self.run_main('registry', self.write([]))
        lines = out.splitlines()
        self.assertEqual(lines[0], 'index\tname\thash\tsize\tflags')
        self.assertEqual(lines[2], '1\tHealth\t0x22222222\t4\t11')
        self.assertEqual(len(lines), 5)

    def test_show(self):
        path = self.write(self.records())
        code, out = self.run_main('show', path, '--entity', '40')
        lines = out.splitlines()
        self.assertEqual(len(lines), 3)  # create, update, destroy (40 in list A)
        self.assertIn('changed Position 12B raw, Health 4B raw v4', lines[0])
        self.assertIn('base 101 (A)', lines[1])
        self.assertIn('Position 3B delta', lines[1])
        code, out = self.run_main('show', path, '--kind', 'frame')
        self.assertEqual(len(out.splitlines()), 1)


if __name__ == '__main__':
    unittest.main()
