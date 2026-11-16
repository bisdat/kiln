#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Static check of a ported engine profile against a read-only COPY of its executable (never the installed game, never
run). Reads the file only; prints one line per check and exits non-zero on any failure.

  python3 -I tools/engine/validate.py sigdb/engine/server/26430253.json /path/to/copy/enshrouded_server.exe
"""
import hashlib
import json
from pathlib import Path
import struct
import sys


def pe(data):
    pe_at = struct.unpack_from('<I', data, 0x3c)[0]
    if data[:2] != b'MZ' or data[pe_at:pe_at + 4] != b'PE\0\0':
        raise SystemExit('not a PE image')
    machine, count, timestamp = struct.unpack_from('<HHI', data, pe_at + 4)
    optional_size = struct.unpack_from('<H', data, pe_at + 20)[0]
    optional = pe_at + 24
    if struct.unpack_from('<H', data, optional)[0] != 0x20b or machine != 0x8664:
        raise SystemExit('not an AMD64 PE32+ image')
    base = struct.unpack_from('<Q', data, optional + 24)[0]
    size_of_image = struct.unpack_from('<I', data, optional + 56)[0]
    sections = {}
    for i in range(count):
        s = optional + optional_size + 40 * i
        name = data[s:s + 8].rstrip(b'\0').decode()
        vsize, va, rsize, raw = struct.unpack_from('<IIII', data, s + 8)
        characteristics = struct.unpack_from('<I', data, s + 36)[0]
        sections[name] = (va, max(vsize, rsize), raw, rsize, characteristics)
    return timestamp, size_of_image, base, sections


def main():
    profile_path, binary_path = Path(sys.argv[1]), Path(sys.argv[2])
    if binary_path.is_symlink() or not binary_path.is_file():
        raise SystemExit('%s: not a regular file (copies only, never a link into a game install)' % binary_path)
    profile = json.loads(profile_path.read_text())
    data = binary_path.read_bytes()
    timestamp, size_of_image, base, sections = pe(data)
    failures = 0

    def ok(cond, text):
        nonlocal failures
        print(('ok   ' if cond else 'FAIL ') + text)
        failures += not cond

    def section_of(rva):
        for name, (va, size, raw, rsize, ch) in sections.items():
            if va <= rva < va + size:
                return name
        return None

    def file_bytes(rva, n):
        for name, (va, size, raw, rsize, ch) in sections.items():
            if va <= rva and rva + n <= va + min(size, rsize):
                return data[raw + rva - va: raw + rva - va + n]
        return None
    pdata = sections.get('.pdata')
    starts = set()
    if pdata:
        va, size, raw, rsize, ch = pdata
        for off in range(raw, raw + min(size, rsize), 12):
            b, e, u = struct.unpack_from('<III', data, off)
            if b:
                starts.add(b)
    image = profile['image']
    ok(hashlib.sha256(data).hexdigest() == image['sha256'], 'sha256 of the file = image.sha256')
    ok(timestamp == image['timestamp'], 'PE TimeDateStamp %d = image.timestamp' % timestamp)
    ok(size_of_image == image['size'], 'PE SizeOfImage %d = image.size (file length %d)' % (size_of_image, len(data)))
    ok(binary_path.name == profile['target'], 'file name = target %s' % profile['target'])
    w = profile['ecsWriteLayout']
    ok(file_bytes(w['changeTrackerRva'], 33) == bytes.fromhex(w['changeTrackerBytes']),
       'changeTrackerRva 0x%x holds changeTrackerBytes' % w['changeTrackerRva'])
    code = [('ecsWriteLayout.threadCallbackRva', w['threadCallbackRva']),
            ('ecsWriteLayout.changeTrackerRva', w['changeTrackerRva']),
            ('resourceWriteLayout.threadCallbackRva', profile['resourceWriteLayout']['threadCallbackRva']),
            ('resourceLayout.resourceServiceIdCallbackRva', profile['resourceLayout']['resourceServiceIdCallbackRva']),
            ('resourceLayout.resourceServiceHandleCallbackRva', profile['resourceLayout']['resourceServiceHandleCallbackRva'])]
    sd = profile.get('systemDependencyLayout', {})
    for key in ('recordBuilderRva', 'accessSetPackRva', 'compatibilityPredicateRva'):
        if key in sd:
            code.append(('systemDependencyLayout.' + key, sd[key]))
    for name, rva in code:
        ok(rva in starts and section_of(rva) == '.text', '%s 0x%x is a .pdata function start in .text' % (name, rva))
    for key in ('threadRegistryLockRva', 'threadRegistryEntriesRva', 'threadRegistryCountRva'):
        ok(section_of(w[key]) == '.data', 'ecsWriteLayout.%s 0x%x lies in .data' % (key, w[key]))
    ok(w['threadRegistryEntriesRva'] + w['threadRegistryCapacity'] * w['threadRegistryEntryStride'] <=
       sections['.data'][0] + sections['.data'][1], 'thread registry rows fit in .data')
    owners = profile['ownerLayout']
    text_va, text_size = sections['.text'][0], sections['.text'][1]
    for key in ('rootVtableRva', 'clientVtableRva', 'controllerVtableRva'):
        rva = owners[key]
        if not rva:
            ok(profile['target'] == 'enshrouded_server.exe' and key == 'rootVtableRva', 'ownerLayout.%s 0 (dedicated server)' % key)
            continue
        first = file_bytes(rva, 8)
        target = struct.unpack('<Q', first)[0] - base if first else -1
        ok(section_of(rva) == '.rdata' and text_va <= target < text_va + text_size,
           'ownerLayout.%s 0x%x is a vtable in .rdata whose first slot points into .text' % (key, rva))
    if owners['rootObjectRva']:
        ok(section_of(owners['rootObjectRva']) == '.data', 'ownerLayout.rootObjectRva 0x%x lies in .data' % owners['rootObjectRva'])
    for name in (w['threadName'], profile['resourceWriteLayout']['threadName']):
        ok(name.encode() + b'\0' in data, 'thread name "%s" is a string in the image' % name)
    print('%s: %d failure(s)' % (profile_path.name, failures))
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
