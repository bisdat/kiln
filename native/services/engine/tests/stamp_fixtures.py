#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""The Wine release check's two foreign kiln-engine.dll copies (tests/wine.sh), derived from this build's own metadata:
the KILN_BUILD of generated/kiln_build.h (whatever native/cmake/kiln_build_stamp.sh wrote: a commit date and hash,
any SOURCE_DATE_EPOCH/KILN_GIT override, or a nonce) must occur exactly once in the DLL, followed by its NUL.
- stale: the same length with the metadata's last character changed (always another build's stamp);
- unstamped: the metadata cut off (NULs), as a DLL built without build metadata (version only).
  stamp_fixtures.py <generated/kiln_build.h> <kiln-engine.dll> <stale out> <unstamped out>
  stamp_fixtures.py --self-test"""
import re
import sys


def build_metadata(header_text):
    found = re.findall(r'^#define KILN_BUILD "([^"]*)"$', header_text, re.M)
    if len(found) != 1 or not found[0]:
        raise ValueError('kiln_build.h: expected one non-empty KILN_BUILD, found %r' % (found,))
    return found[0].encode('ascii')


def different(meta):
    last = meta[-1:]
    return meta[:-1] + (b'1' if last == b'0' else b'0')


def derive(dll, meta):
    needle = meta + b'\x00'
    count = dll.count(needle)
    if count != 1:
        raise ValueError('kiln-engine.dll: expected this build\'s stamp %s once, found it %d times' % (meta.decode(), count))
    stale = dll.replace(needle, different(meta) + b'\x00')
    unstamped = dll.replace(needle, b'\x00' * len(needle))
    return stale, unstamped


def self_test():
    failures = 0

    def check(ok, what):
        nonlocal failures
        print(('ok   ' if ok else 'FAIL ') + what)
        failures += not ok

    for meta in (b'+20261116.9a6038a', b'+19700102.abc1234', b'+19700101.0000000', b'+nogit.0123456789abcdef'):
        dll = b'MZ\x00code' + b'0.2.0-dev' + meta + b'\x00' + b'tail\x000.2.0-dev\x00'
        stale, unstamped = derive(dll, meta)
        check(len(stale) == len(dll) and meta + b'\x00' not in stale and different(meta) + b'\x00' in stale,
              '%s: stale copy has another stamp of the same length' % meta.decode())
        check(len(unstamped) == len(dll) and b'0.2.0-dev\x00' in unstamped and meta not in unstamped,
              '%s: unstamped copy keeps the version only' % meta.decode())
    for bad, what in ((b'MZ no stamp here', 'absent'), (b'+19700102.abc1234\x00x+19700102.abc1234\x00', 'twice')):
        try:
            derive(bad, b'+19700102.abc1234')
            check(False, 'a stamp %s is refused' % what)
        except ValueError:
            check(True, 'a stamp %s is refused' % what)
    check(build_metadata('#define KILN_BUILD "+19700102.abc1234"\n') == b'+19700102.abc1234',
          'the metadata comes from kiln_build.h (the 1970 override)')
    for header in ('#define KILN_BUILD ""\n', '/* nothing */\n'):
        try:
            build_metadata(header)
            check(False, 'an unstamped build header is refused: %r' % header)
        except ValueError:
            check(True, 'an unstamped build header is refused: %r' % header.strip())
    print('stamp fixtures: %d failure(s)' % failures)
    return 1 if failures else 0


def main(argv):
    if argv[1:] == ['--self-test']:
        return self_test()
    if len(argv) != 5:
        print(__doc__, file=sys.stderr)
        return 2
    try:
        meta = build_metadata(open(argv[1], encoding='utf-8').read())
        stale, unstamped = derive(open(argv[2], 'rb').read(), meta)
    except (OSError, ValueError) as error:
        print(error, file=sys.stderr)
        return 2
    open(argv[3], 'wb').write(stale)
    open(argv[4], 'wb').write(unstamped)
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
