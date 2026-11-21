#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""replinspect: an offline parser for the JSONL capture files of kiln.repl ("repl capture <peer> <seconds>").

Parses every message a peer was sent by the wire formats of the replication chapter (§2.3) and checks that the bit
header and the component blobs account for every payload byte, that the raw bytes agree with the record's JSON fields
and that component indices and sizes agree with the capture's registry. The file format is in README.md.

    python3 tools/replinspect/replinspect.py check <capture.jsonl> [--json]
    python3 tools/replinspect/replinspect.py show <capture.jsonl> [--entity N] [--kind K]
    python3 tools/replinspect/replinspect.py registry <capture.jsonl>

Exit codes: 0 every message accounts for every byte and agrees with its fields, 1 a problem was found (each one is
printed with its line number), 2 bad usage or a file that is not a capture (no header line).
Standard library only.
"""
import argparse
import json
import struct
import sys

# Registry flag bits (the header's "flags").
FLAG_REPLICATED = 1
FLAG_VERSIONED = 2
FLAG_CUSTOM_CODEC = 4
FLAG_CLIENT_CHANNEL = 8
FLAG_PREDICTION_CHANNEL = 16

# Entry types ("msg") and the kinds each one may carry.
MSG_KINDS = {0: ('frame',), 2: ('handshake',), 4: ('create', 'update'), 5: ('destroy',)}
# Header kind (low 3 bits of the first u32).
HEADER_KIND = {'create': 2, 'update': 3, 'destroy': 4}
CHANNELS = (4, 5)
BASE_ACKED = 0x80000000


class FormatError(Exception):
    """The bytes do not follow the wire format (cut short, a field past the end)."""


class CaptureError(Exception):
    """The file is not a capture (unreadable, or no header line)."""


# ---------------------------------------------------------------------------------------------------------------------
# Wire formats (chapter 11 §2.3)

class Reader:
    """Little-endian byte cursor over one message."""

    def __init__(self, data):
        self.data, self.pos = data, 0

    def take(self, n, what):
        if self.pos + n > len(self.data):
            raise FormatError(f'cut short in {what}: needs {n} bytes at byte {self.pos}, the message has {len(self.data)}')
        b = self.data[self.pos:self.pos + n]
        self.pos += n
        return b

    def u8(self, what):
        return self.take(1, what)[0]

    def u16(self, what):
        return struct.unpack('<H', self.take(2, what))[0]

    def u32(self, what):
        return struct.unpack('<I', self.take(4, what))[0]


class Bits:
    """Bit stream, least significant bit of each byte first."""

    def __init__(self, data):
        self.data, self.pos = data, 0

    def get(self, n, what):
        if self.pos + n > len(self.data) * 8:
            raise FormatError(f'bit header runs past the payload in {what} (bit {self.pos}, '
                              f'payload {len(self.data)} bytes)')
        v = 0
        for i in range(n):
            p = self.pos + i
            v |= ((self.data[p >> 3] >> (p & 7)) & 1) << i
        self.pos += n
        return v


def parse_payload(p):
    """The payload of an entity message. Returns a dict; raises FormatError when the bit header does not fit.

    Blob problems (running past the payload, bytes left over) are reported in the dict, not raised, so a caller can
    still show the header."""
    bt = Bits(p)
    flags = bt.get(3, 'the flag bits')
    n_rem = bt.get(6, 'the removed count') if flags & 1 else 0
    n_add = bt.get(6, 'the added count') if flags & 2 else 0
    n_chg = bt.get(8, 'the changed count') if flags & 4 else 0
    removed = [bt.get(10, f'removed entry {i}') for i in range(n_rem)]
    added = []
    for i in range(n_add):
        idx = bt.get(10, f'added entry {i}')
        ver = bt.get(8, f'added entry {i} version') if bt.get(1, f'added entry {i} version bit') else None
        added.append({'index': idx, 'version': ver})
    changed = []
    for i in range(n_chg):
        idx = bt.get(10, f'changed entry {i}')
        ver = bt.get(8, f'changed entry {i} version') if bt.get(1, f'changed entry {i} version bit') else None
        w = bt.get(16, f'changed entry {i} size')
        changed.append({'index': idx, 'version': ver, 'size': w >> 1, 'delta': bool(w & 1)})
    header_bytes = (bt.pos + 7) >> 3
    pad_bits = header_bytes * 8 - bt.pos
    pad_value = bt.get(pad_bits, 'the padding') if pad_bits else 0
    off, cut = header_bytes, None
    for i, c in enumerate(changed):
        if cut is None and off + c['size'] > len(p):
            cut = f"the blob of changed entry {i} (index {c['index']}, {c['size']} bytes at payload byte " \
                  f"{off}) runs past the payload ({len(p)} bytes)"
        c['blob'] = p[off:off + c['size']]
        off += c['size']
    return {'flags': flags, 'removed': removed, 'added': added, 'changed': changed, 'header_bytes': header_bytes,
            'pad_bits': pad_bits, 'pad_value': pad_value, 'blob_bytes': off - header_bytes, 'length': len(p),
            'cut': cut, 'left_over': max(0, len(p) - off)}


def parse_entity(raw):
    """An entity message (entry type 4): header, then the payload. Raises FormatError when the header is cut short."""
    r = Reader(raw)
    w0 = r.u32('the tick/kind word')
    m = {'kind_code': w0 & 7, 'tick': w0 >> 3, 'seq': r.u32('the seq'), 'entity': r.u32('the entity id')}
    if m['kind_code'] == 2:
        m['parent'] = r.u32('the parent')
        m['guid'] = r.take(16, 'the template GUID').hex()
        m['u8'] = r.u8('the create byte')
    elif m['kind_code'] == 3:
        b = r.u32('the baseline')
        m['base_word'] = b
        m['base'] = b & ~BASE_ACKED & 0xFFFFFFFF
        m['base_acked'] = bool(b & BASE_ACKED)
        if m['base'] == 0:
            m['guid'] = r.take(16, 'the template GUID').hex()
    else:
        raise FormatError(f"header kind {m['kind_code']} is not an entity message (2 create, 3 update)")
    m['plen'] = r.u16('the payload length')
    m['header_len'] = r.pos
    m['payload'] = r.take(m['plen'], 'the payload')
    m['trailing'] = len(raw) - r.pos
    return m


def parse_destroy(raw):
    """A destroy list (entry type 5): u32 (tick<<3)|4, u32 seq, u8 nA, u8 nB, (nA+nB) x u32 ids."""
    r = Reader(raw)
    w0 = r.u32('the tick/kind word')
    d = {'kind_code': w0 & 7, 'tick': w0 >> 3, 'seq': r.u32('the seq')}
    na, nb = r.u8('the list A count'), r.u8('the list B count')
    d['a'] = [r.u32(f'list A id {i}') for i in range(na)]
    d['b'] = [r.u32(f'list B id {i}') for i in range(nb)]
    d['trailing'] = len(raw) - r.pos
    return d


def read_count(blob, pos, what):
    """A generic delta count: 0xFF bytes (255 each) then one byte < 0xFF."""
    n = 0
    while True:
        if pos >= len(blob):
            raise FormatError(f'generic delta: {what} at blob byte {pos} runs past the blob ({len(blob)} bytes)')
        b = blob[pos]
        pos += 1
        n += b
        if b != 0xFF:
            return n, pos


def parse_generic_delta(blob):
    """GenericDelta runs: [unchanged count][changed count][changed bytes XOR baseline], repeated, nothing after the
    last changed byte. Returns [(component offset, unchanged, changed)] and the bytes of the component it covers."""
    runs, pos, covered = [], 0, 0
    while pos < len(blob):
        skip, pos = read_count(blob, pos, f'the unchanged count of run {len(runs)}')
        chg, pos = read_count(blob, pos, f'the changed count of run {len(runs)}')
        if chg == 0:
            raise FormatError(f'generic delta: run {len(runs)} changes no byte')
        if pos + chg > len(blob):
            raise FormatError(f'generic delta: run {len(runs)} has {chg} changed bytes at blob byte {pos}, '
                              f'the blob has {len(blob)}')
        runs.append((covered + skip, skip, chg))
        covered += skip + chg
        pos += chg
    return runs, covered


# ---------------------------------------------------------------------------------------------------------------------
# The capture file

class Capture:
    def __init__(self, path):
        self.path = path
        self.header = None
        self.registry = {}      # index -> entry
        self.by_name = {}       # name -> entry
        self.messages = []      # (line number, record)
        self.end = None
        self.problems = []      # (line number, text)
        self.warnings = []      # (line number, text)
        self.other_types = 0


def load(path):
    try:
        with open(path, encoding='utf-8', errors='replace') as f:
            lines = f.read().split('\n')
    except OSError as e:
        raise CaptureError(f'cannot read {path}: {e.strerror}')
    if lines and lines[-1] == '':
        lines.pop()
    cap = Capture(path)
    for no, line in enumerate(lines, 1):
        if not line.strip():
            continue
        try:
            rec = json.loads(line)
        except ValueError as e:
            if no == len(lines) and cap.end is None:
                cap.warnings.append((no, f'the last line is not whole JSON ({e.msg}): the capture was cut off'))
            else:
                cap.problems.append((no, f'not JSON: {e.msg}'))
            continue
        if not isinstance(rec, dict):
            cap.problems.append((no, 'not a JSON object'))
            continue
        t = rec.get('type')
        if cap.header is None:
            if t != 'header':
                raise CaptureError(f'{path}: line {no} is not the header line (type "header")')
            _read_header(cap, no, rec)
            continue
        if cap.end is not None:
            cap.warnings.append((no, 'a line after the end line'))
        if t == 'msg':
            cap.messages.append((no, rec))
        elif t == 'end':
            cap.end = rec
            cap.end_line = no
        elif t == 'header':
            cap.problems.append((no, 'a second header line'))
        else:
            cap.other_types += 1
    if cap.header is None:
        raise CaptureError(f'{path}: empty, no header line')
    if cap.end is None:
        cap.warnings.append((len(lines), 'no end line: the server stopped before the capture ended'))
    elif isinstance(cap.end.get('messages'), int) and cap.end['messages'] != len(cap.messages):
        cap.warnings.append((cap.end_line, f"the end line counts {cap.end['messages']} messages, the file has "
                                           f"{len(cap.messages)}"))
    return cap


def _read_header(cap, no, rec):
    cap.header = rec
    if rec.get('version') != 1:
        cap.warnings.append((no, f"header version {rec.get('version')!r}, this tool reads version 1"))
    reg = rec.get('registry')
    if not isinstance(reg, list):
        cap.problems.append((no, 'the header has no registry list'))
        return
    for i, e in enumerate(reg):
        if not isinstance(e, dict) or not all(isinstance(e.get(k), int) for k in ('index', 'size', 'flags')) \
                or not isinstance(e.get('name'), str):
            cap.problems.append((no, f'registry entry {i} lacks index, name, size or flags'))
            continue
        if e['index'] in cap.registry:
            cap.warnings.append((no, f"registry index {e['index']} appears twice"))
        cap.registry[e['index']] = e
        cap.by_name[e['name']] = e


def comp_name(cap, idx):
    e = cap.registry.get(idx)
    return e['name'] if e else f'#{idx}'


# ---------------------------------------------------------------------------------------------------------------------
# Checking one message

def check_message(cap, rec):
    """Returns (problems, warnings, parsed): lists of texts and the parsed message (None when not parsed)."""
    probs, warns = [], []
    for k in ('peer', 'ch', 'msg', 'kind', 'tick', 'seq', 'bytes', 'raw'):
        if k not in rec:
            probs.append(f'missing field "{k}"')
    if probs:
        return probs, warns, None
    try:
        raw = bytes.fromhex(rec['raw'])
    except (TypeError, ValueError):
        return ['"raw" is not hex'], warns, None
    kind, msg = rec['kind'], rec['msg']
    if rec['ch'] not in CHANNELS:
        probs.append(f"channel {rec['ch']!r} is not 4 or 5")
    if msg not in MSG_KINDS:
        probs.append(f'unknown entry type {msg!r}')
    elif kind not in MSG_KINDS[msg]:
        probs.append(f'kind "{kind}" does not go with entry type {msg} ({" or ".join(MSG_KINDS[msg])})')
    if rec['bytes'] != len(raw):
        probs.append(f"\"bytes\" is {rec['bytes']}, the raw message has {len(raw)}")
    if isinstance(cap.header.get('peer'), int) and rec['peer'] != cap.header['peer']:
        warns.append(f"peer {rec['peer']} is not the header's peer {cap.header['peer']}")
    if msg == 4 and kind in ('create', 'update'):
        parsed = _check_entity(cap, rec, raw, probs, warns)
    elif msg == 5 and kind == 'destroy':
        parsed = _check_destroy(rec, raw, probs)
    else:
        parsed = None  # frames and handshakes are counted, not parsed
    return probs, warns, parsed


def _agree(probs, field, json_value, raw_value):
    if json_value != raw_value:
        probs.append(f'"{field}" is {json_value!r}, the raw header says {raw_value!r}')


def _check_entity(cap, rec, raw, probs, warns):
    try:
        m = parse_entity(raw)
    except FormatError as e:
        probs.append(str(e))
        return None
    want = HEADER_KIND[rec['kind']]
    if m['kind_code'] != want:
        probs.append(f"kind \"{rec['kind']}\" but the raw header kind is {m['kind_code']}")
    _agree(probs, 'tick', rec['tick'], m['tick'])
    _agree(probs, 'seq', rec['seq'], m['seq'])
    _agree(probs, 'entity', rec.get('entity'), m['entity'])
    if m['kind_code'] == 3:
        if rec.get('base') not in (m['base'], m['base_word']):
            probs.append(f"\"base\" is {rec.get('base')!r}, the raw baseline is {m['base']}"
                         f"{' (acked state A)' if m['base_acked'] else ''}")
    elif rec.get('base', 0) != 0:
        probs.append(f"\"base\" is {rec.get('base')!r} on a create (expected 0)")
    if m['kind_code'] == 2 and m['u8'] != 0:
        warns.append(f"the create header's byte after the GUID is {m['u8']}, not 0")
    if m['trailing']:
        probs.append(f"{m['trailing']} bytes after the payload (u16 length {m['plen']})")
    try:
        p = parse_payload(m['payload'])
    except FormatError as e:
        probs.append(str(e))
        return m
    m['p'] = p
    if p['cut']:
        probs.append(p['cut'])
    if p['left_over']:
        probs.append(f"{p['left_over']} payload bytes left over: header {p['header_bytes']} + blobs {p['blob_bytes']} "
                     f"!= length {p['length']}")
    if p['pad_value']:
        warns.append(f"the {p['pad_bits']} padding bits are not zero ({p['pad_value']:#x})")
    comps = rec.get('components') if isinstance(rec.get('components'), dict) else {}
    for name in comps:
        if name not in cap.by_name:
            warns.append(f'"components" names "{name}", which is not in the registry')
    for idx in p['removed']:
        if idx not in cap.registry:
            probs.append(f'removed index {idx} is not in the registry')
    for a in p['added']:
        _check_index(cap, a, 'added', probs, warns)
    for i, c in enumerate(p['changed']):
        e = _check_index(cap, c, 'changed', probs, warns)
        what = f"changed {comp_name(cap, c['index'])} (entry {i})"
        if not c['delta']:
            if e and c['size'] != e['size']:
                probs.append(f"{what}: raw blob of {c['size']} bytes, the registry size is {e['size']}")
            if e and e['name'] in comps and len(c['blob']) == c['size']:
                try:
                    want_v = bytes.fromhex(comps[e['name']])
                except (TypeError, ValueError):
                    probs.append(f"\"components\" value of {e['name']} is not hex")
                else:
                    if want_v != c['blob']:
                        probs.append(f'{what}: the raw blob differs from its "components" value')
        elif e and not e['flags'] & FLAG_CUSTOM_CODEC and len(c['blob']) == c['size']:
            try:
                runs, covered = parse_generic_delta(c['blob'])
            except FormatError as err:
                probs.append(f'{what}: {err}')
            else:
                c['runs'] = runs
                if covered > e['size']:
                    probs.append(f"{what}: the generic delta covers {covered} bytes, the component has {e['size']}")
                if c['size'] >= e['size']:
                    warns.append(f"{what}: a generic delta of {c['size']} bytes is not smaller than the raw "
                                 f"{e['size']} (the writer sends those raw)")
    return m


def _check_index(cap, c, what, probs, warns):
    e = cap.registry.get(c['index'])
    if e is None:
        probs.append(f"{what} index {c['index']} is not in the registry")
        return None
    if c['version'] is not None and not e['flags'] & FLAG_VERSIONED:
        warns.append(f"{what} {e['name']} carries a version, the registry does not flag it versioned")
    return e


def _check_destroy(rec, raw, probs):
    try:
        d = parse_destroy(raw)
    except FormatError as e:
        probs.append(str(e))
        return None
    if d['kind_code'] != 4:
        probs.append(f"a destroy list with header kind {d['kind_code']} (expected 4)")
    _agree(probs, 'tick', rec['tick'], d['tick'])
    _agree(probs, 'seq', rec['seq'], d['seq'])
    for k in ('a', 'b'):
        if k not in rec:
            probs.append(f'missing field "{k}" on a destroy message')
        else:
            _agree(probs, k, rec[k], d[k])
    if d['trailing']:
        probs.append(f"{d['trailing']} bytes after the destroy list's ids")
    return d


# ---------------------------------------------------------------------------------------------------------------------
# Commands

def run_check(cap):
    """Checks every message; returns the summary document."""
    by_kind, by_channel, entities = {}, {}, set()
    total = gaps = clean = 0
    problems = [{'line': n, 'text': t} for n, t in cap.problems]
    warnings = [{'line': n, 'text': t} for n, t in cap.warnings]
    for no, rec in cap.messages:
        probs, warns, _ = check_message(cap, rec)
        problems += [{'line': no, 'text': t} for t in probs]
        warnings += [{'line': no, 'text': t} for t in warns]
        clean += not probs
        kind = str(rec.get('kind'))
        k = by_kind.setdefault(kind, {'messages': 0, 'bytes': 0})
        k['messages'] += 1
        n = rec.get('bytes') if isinstance(rec.get('bytes'), int) else 0
        k['bytes'] += n
        total += n
        ch = str(rec.get('ch'))
        c = by_channel.setdefault(ch, {'messages': 0, 'bytes': 0})
        c['messages'] += 1
        c['bytes'] += n
        if rec.get('gap'):
            gaps += 1
        if kind in ('create', 'update') and rec.get('entity'):
            entities.add(rec['entity'])
    if cap.other_types:
        warnings.append({'line': 0, 'text': f'{cap.other_types} lines of an unknown type were ignored'})
    problems.sort(key=lambda p: p['line'])
    warnings.sort(key=lambda p: p['line'])
    h, e = cap.header, cap.end or {}
    return {'file': cap.path, 'peer': h.get('peer'), 'seconds': h.get('seconds'), 'utc': h.get('utc'),
            'registry': len(cap.registry), 'complete': cap.end is not None, 'messages': len(cap.messages),
            'clean': clean, 'bytes': total, 'by_kind': by_kind, 'by_channel': by_channel, 'entities': len(entities),
            'gaps': gaps, 'dropped': e.get('dropped'), 'problems': problems, 'warnings': warnings,
            'ok': not problems}


def print_check(s, out):
    out.write(f"{s['file']}: peer {s['peer']}, {s['seconds']} s from {s['utc']}, {s['registry']} registry entries"
              f"{'' if s['complete'] else ' (no end line)'}\n")
    out.write(f"messages {s['messages']} ({s['bytes']} bytes), {s['clean']} account for every byte; "
              f"{s['entities']} entities; {s['gaps']} gaps; dropped {s['dropped']}\n")
    for title, d in (('kind', s['by_kind']), ('channel', s['by_channel'])):
        for k in sorted(d):
            out.write(f"  {title} {k}: {d[k]['messages']} messages, {d[k]['bytes']} bytes\n")
    for w in s['warnings']:
        out.write(f"line {w['line']}: warning: {w['text']}\n")
    for p in s['problems']:
        out.write(f"line {p['line']}: {p['text']}\n")
    out.write(f"replinspect: {'OK' if s['ok'] else 'FAILED'}: {len(s['problems'])} problems, "
              f"{len(s['warnings'])} warnings\n")


def show_line(cap, rec, parsed, probs):
    kind = rec.get('kind')
    head = (f"tick {rec.get('tick')} seq {rec.get('seq')} ch{rec.get('ch')} {kind} entity {rec.get('entity', 0)} "
            f"{rec.get('bytes')}B{' GAP' if rec.get('gap') else ''}")
    parts = []
    if parsed and kind in ('create', 'update'):
        if 'base' in parsed:
            parts.append(f"base {parsed['base']}{' (A)' if parsed['base_acked'] else ''}"
                         f"{' template' if 'guid' in parsed else ''}")
        p = parsed.get('p')
        if p:
            if p['removed']:
                parts.append('removed ' + ', '.join(comp_name(cap, i) for i in p['removed']))
            if p['added']:
                parts.append('added ' + ', '.join(comp_name(cap, a['index']) +
                                                  ('' if a['version'] is None else f" v{a['version']}")
                                                  for a in p['added']))
            if p['changed']:
                parts.append('changed ' + ', '.join(
                    f"{comp_name(cap, c['index'])} {c['size']}B {'delta' if c['delta'] else 'raw'}"
                    + ('' if c['version'] is None else f" v{c['version']}") for c in p['changed']))
    elif parsed and kind == 'destroy':
        parts.append(f"A {parsed['a']} B {parsed['b']}")
    line = head + (' | ' + ' | '.join(parts) if parts else '')
    if probs:
        line += ' !! ' + '; '.join(probs)
    return line


def main(argv=None):
    ap = argparse.ArgumentParser(prog='replinspect.py', description=__doc__.split('\n')[0])
    sub = ap.add_subparsers(dest='cmd', required=True)
    c = sub.add_parser('check', help='parse every message and account for every byte')
    c.add_argument('capture')
    c.add_argument('--json', action='store_true', help='print one JSON summary document')
    s = sub.add_parser('show', help='one readable line per message')
    s.add_argument('capture')
    s.add_argument('--entity', type=int)
    s.add_argument('--kind', choices=('frame', 'handshake', 'create', 'update', 'destroy'))
    r = sub.add_parser('registry', help="the header's registry as TSV")
    r.add_argument('capture')
    a = ap.parse_args(argv)
    try:
        cap = load(a.capture)
    except CaptureError as e:
        sys.stderr.write(f'replinspect: {e}\n')
        return 2
    out = sys.stdout
    if a.cmd == 'check':
        summary = run_check(cap)
        if a.json:
            json.dump(summary, out, indent=1)
            out.write('\n')
        else:
            print_check(summary, out)
        return 0 if summary['ok'] else 1
    if a.cmd == 'registry':
        out.write('index\tname\thash\tsize\tflags\n')
        for idx in sorted(cap.registry):
            e = cap.registry[idx]
            h = e.get('hash')
            out.write(f"{idx}\t{e['name']}\t{f'0x{h:08x}' if isinstance(h, int) else ''}\t{e['size']}\t{e['flags']}\n")
        return 0
    for no, rec in cap.messages:
        if a.kind and rec.get('kind') != a.kind:
            continue
        if a.entity is not None and rec.get('entity') != a.entity and a.entity not in (rec.get('a') or []) \
                and a.entity not in (rec.get('b') or []):
            continue
        probs, _, parsed = check_message(cap, rec)
        out.write(f'{no}: {show_line(cap, rec, parsed, probs)}\n')
    return 0


if __name__ == '__main__':
    sys.exit(main())
