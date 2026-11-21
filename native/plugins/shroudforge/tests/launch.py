#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""kiln-shroudforge (and kiln-sfi, given its DLL) against the ShroudForge backend selection, with the fake DLLs of
run.py under Wine. No game binary is loaded.

  launch.py <tests bin> <kiln-shroudforge.dll> [<kiln-sfi.dll>]

- legacy-*: no kiln.launch (an older core): the adapter reads kiln.json "shroudforge" itself, backend > useInstalled >
  hosted, KILN_SHROUDFORGE_RUNTIME between hosted and installed.
- frozen-*, exactly-one-*, unreadable-*, idle-*: the core's frozen kiln.launch record (host.cpp KILN_SF_LAUNCH) decides
  which of hosted, installed and Kiln's interpreter starts, never more than one. The kiln-sfi cases need its
  DLL; without one they are skipped and named.

On Linux the fake host runs under Wine: the caller gives the private, headless WINEPREFIX (WINEPREFIX=... python3 -I
launch.py ...). On Windows it runs natively. KILN_SF_ONLY=<prefix> runs the cases whose name starts with it.
"""
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile

build, adapter = (Path(p).resolve() for p in sys.argv[1:3])
sfi_dll = Path(sys.argv[3]).resolve() if len(sys.argv) > 3 else None
native = os.name == 'nt'
if not native and not os.environ.get('WINEPREFIX'):
    sys.exit('set WINEPREFIX to a private prefix')
only = os.environ.get('KILN_SF_ONLY', '')
base_env = dict(os.environ, WINEDEBUG='-all', WAYLAND_DISPLAY='kiln-no-display', WINEDLLOVERRIDES='winedbg.exe=d')
base_env.pop('DISPLAY', None)
for k in [k for k in base_env if k.startswith('KILN_')]:
    base_env.pop(k)


def wine_path(p):
    return str(p) if native else 'Z:' + str(p).replace('/', '\\')


class Run:
    """One start of the fake host: the adapter's final shroudforge.json, the fake runtimes' call log, kiln.log."""

    def __init__(self, game, status, calls, out):
        self.game, self.status, self.calls, self.out = game, status, calls, out

    @property
    def hosted(self):  # kiln-shroudforge ran a ShroudForge runtime this start: which one, or None
        return self.status.get('source') if 'prepare' in self.calls and 'create' in self.calls else None

    @property
    def interpreter(self):  # how often kiln-sfi took the start for Kiln's interpreter
        # Under kiln it then reads the frozen launch plan; this fake host serves none (no profile reference,
        # no build_info), so kiln-sfi refuses there with its loud warning: one per start it took.
        return self.out.count("kiln-sfi: WARNING: Kiln's interpreter has this start")

    @property
    def eml_aside(self):  # kiln-shroudforge asked kiln-eml to stand aside (kiln_eml.h stand_aside)
        return 'reserve' in self.calls


def start(root, name, kiln_json=None, env=None, launch=None, installed=True, bundled=True, sfi=False, self_hosted=False):
    game = root / name
    for kind, present in [('shroudforge', installed), ('kiln/plugins/shroudforge', bundled)]:
        dest = game / kind
        dest.mkdir(parents=True)
        if not present:
            continue
        (dest / 'version.json').write_text(json.dumps({'version': '1.7.9'}))
        (dest / 'profiles').mkdir()
        shutil.copyfile(build / 'good_kfc.dll', dest / 'kfc-runtime.dll')
        shutil.copyfile(build / 'runtime.dll', dest / 'shroudforge-runtime.dll')
    if kiln_json is not None:
        (game / 'kiln/kiln.json').write_text(kiln_json if isinstance(kiln_json, str) else json.dumps(kiln_json))
    shutil.copyfile(build / 'eml.dll', game / 'kiln/plugins/kiln-eml.dll')
    if self_hosted:
        shutil.copyfile(build / 'bootstrap.dll', game / 'bootstrap.dll')
    log = game / 'calls.txt'
    e = dict(base_env, KILN_SF_TEST_LOG=wine_path(log), KILN_SF_REAL_ORDER='1', KILN_SHROUDFORGE='1')
    if launch is not None:
        e['KILN_SF_LAUNCH'] = launch
    if sfi:
        e['KILN_SF_SFI'] = wine_path(sfi_dll)
    if self_hosted:
        e['KILN_SF_SELF_HOSTED'] = '1'
    for k, v in (env or {}).items():
        if v is None:
            e.pop(k, None)
        else:
            e[k] = v
    host = [str(build / 'sfhost.exe')] if native else ['wine', str(build / 'sfhost.exe')]
    result = subprocess.run(host + [str(game), str(adapter), 'early'], env=e,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=60)
    out = result.stdout.decode(errors='replace')
    if result.returncode:
        raise RuntimeError(f'{name}: host exit {result.returncode}\n{out}')
    status = json.loads((game / 'kiln/state/shroudforge.json').read_text())
    calls = log.read_text().splitlines() if log.exists() else []
    return Run(game, status, calls, out)


def check(name, fn, needs_sfi=False):
    if only and not name.startswith(only):
        return
    if needs_sfi and not sfi_dll:
        print('SKIP', name, '(no kiln-sfi.dll given)')
        return
    fn(name)
    print('PASS', name)


# The comparison of two starts that must behave alike: what differs between any two runs (process id, times, the
# game folder's name, loop and timing counts) is taken out; everything else must be equal.
VOLATILE = {'pid', 'updated', 'turns'}


def same_start(a, b):
    def status(r):
        return {k: v for k, v in r.status.items() if k not in VOLATILE and k != 'modsDir' and k != 'runtimeDir'}

    def calls(r):  # the loop's tick/update turns (their number is timing) count as one "loop"
        out = [c for c in r.calls if c not in ('tick', 'update')]
        return out + (['loop'] if 'update' in r.calls else [])

    def log(r):
        lines = []
        for line in r.out.splitlines():
            if line.startswith(('MESA', 'pci id', '[host] pid', '[host] status', 'slow-io', 'kiln-sfi', '[fake kiln.caps]')):
                continue
            line = line.replace(wine_path(r.game), '<game>').replace(str(r.game), '<game>')
            lines.append(re.sub(r'\(\d+ ms', '(N ms', line))
        return lines
    assert status(a) == status(b), (status(a), status(b))
    assert calls(a) == calls(b), (calls(a), calls(b))
    assert log(a) == log(b), '\n'.join(log(a)) + '\n----\n' + '\n'.join(log(b))


with tempfile.TemporaryDirectory(prefix='kiln-sf-launch-', dir=build.parent) as tmp:
    root = Path(tmp)

    # ---- kiln.json read by the adapter itself (no kiln.launch: an older core) ----
    def legacy_backend_over_use_installed(name):
        r = start(root, name, {'shroudforge': {'backend': 'installed', 'useInstalled': False}})
        assert r.hosted == 'installed' and r.status['useInstalled'] is True, (r.status, r.out)
        r = start(root, name + '-hosted', {'shroudforge': {'backend': 'hosted', 'useInstalled': True}})
        assert r.hosted == 'bundled' and r.status['useInstalled'] is False, (r.status, r.out)
    check('legacy-backend-over-use-installed', legacy_backend_over_use_installed)

    def legacy_use_installed_without_backend(name):  # main's rule, unchanged
        assert start(root, name, {'shroudforge': {'useInstalled': True}}).hosted == 'installed'
        assert start(root, name + '-default', {}).hosted == 'bundled'
        assert start(root, name + '-none').hosted == 'bundled'
    check('legacy-use-installed-without-backend', legacy_use_installed_without_backend)

    def legacy_env_runtime_between_hosted_and_installed(name):
        r = start(root, name, {'shroudforge': {'backend': 'installed'}}, env={'KILN_SHROUDFORGE_RUNTIME': 'bundled'})
        assert r.hosted == 'bundled', (r.status, r.out)
        r = start(root, name + '-installed', {'shroudforge': {'backend': 'hosted'}}, env={'KILN_SHROUDFORGE_RUNTIME': 'installed'})
        assert r.hosted == 'installed', (r.status, r.out)
    check('legacy-env-runtime-between-hosted-and-installed', legacy_env_runtime_between_hosted_and_installed)

    def legacy_kiln_stands_aside(name):
        # Kiln's interpreter asked for, and no core decision: kiln-shroudforge never runs a ShroudForge runtime instead,
        # not with KILN_SHROUDFORGE=1 (set by start()) and not with KILN_SHROUDFORGE_RUNTIME
        for suffix, env in [('', None), ('-env', {'KILN_SHROUDFORGE_RUNTIME': 'installed'})]:
            r = start(root, name + suffix, {'shroudforge': {'backend': 'kiln', 'useInstalled': True}}, env=env)
            assert r.calls == [] and r.hosted is None and not r.eml_aside, (r.calls, r.status)
            assert r.status['mode'] == 'off' and 'shroudforge.backend is "kiln"' in r.status['reason'], r.status
            assert r.out.count('ShroudForge mods are off for this start: ' + r.status['reason']) == 1, r.out
    check('legacy-kiln-stands-aside', legacy_kiln_stands_aside)

    def legacy_invalid_backend_is_named(name):
        # a typo in kiln.json: the log line and the status say which value and why, so no mod goes missing silently
        for suffix, value, shown in [('', 'Kiln', '"Kiln"'), ('-number', 1, '1'), ('-bundled', 'bundled', '"bundled"')]:
            r = start(root, name + suffix, {'shroudforge': {'backend': value, 'useInstalled': True}},
                      env={'KILN_SHROUDFORGE_RUNTIME': 'installed'})
            assert r.calls == [] and r.hosted is None and not r.eml_aside, (r.calls, r.status)
            want = f'kiln.json shroudforge.backend {shown} is not hosted, installed or kiln'
            assert r.status['mode'] == 'off' and r.status['reason'].startswith(want), r.status
            line = [l for l in r.out.splitlines() if want in l and not l.startswith('[host] status')]
            assert len(line) == 1 and line[0] == 'ShroudForge mods are off for this start: ' + r.status['reason'], r.out
    check('legacy-invalid-backend-is-named', legacy_invalid_backend_is_named)

    def legacy_deep_or_wide_backend_is_named_briefly(name):
        # a value nested 100000 levels deep or a megabyte wide is named as the core's brief() names
        # it (its kind, or a string cut to 64 bytes), never copied or dumped whole: no stack overflow at boot (main's
        # adapter read the same file without one), no megabyte reason in shroudforge.json
        for suffix, value, shown in [('-deep', '[' * 100000 + ']' * 100000, 'an array'),
                                     ('-wide', '"' + 'x' * 1000000 + '"', 'a long string "' + 'x' * 64 + '"')]:
            r = start(root, name + suffix, '{"shroudforge": {"backend": ' + value + '}}')
            assert r.calls == [] and r.hosted is None and not r.eml_aside, (r.calls, r.status.get('mode'))
            want = f'kiln.json shroudforge.backend {shown} is not hosted, installed or kiln'
            assert r.status['mode'] == 'off' and r.status['reason'].startswith(want), r.status['reason'][:200]
            assert len(r.status['reason']) < 300, len(r.status['reason'])
    check('legacy-deep-or-wide-backend-is-named-briefly', legacy_deep_or_wide_backend_is_named_briefly)

    def legacy_shroudforge_zero_keeps_mains_note(name):
        r = start(root, name, {'shroudforge': {'backend': 'installed'}}, env={'KILN_SHROUDFORGE': '0'})
        assert r.calls == [] and r.status['mode'] == 'off' and r.status['reason'] == 'KILN_SHROUDFORGE=0', r.status
    check('legacy-shroudforge-zero-keeps-mains-note', legacy_shroudforge_zero_keeps_mains_note)

    def legacy_comments_as_main_reads_them(name):  # kiln.json with comments, read as before
        r = start(root, name, '{\n  // mine\n  "shroudforge": {"backend": "installed"}\n}\n')
        assert r.hosted == 'installed', (r.status, r.out)
    check('legacy-comments-as-main-reads-them', legacy_comments_as_main_reads_them)

    # ---- the core's frozen kiln.launch decides ----
    HOSTED, INSTALLED, KILN, NONE = 1, 2, 3, 0
    WORDS = {HOSTED: 'hosted', INSTALLED: 'installed', KILN: 'kiln', NONE: 'none'}

    def rec(backend, code):
        return f'{backend},{code:#x}'

    def frozen_hosted_and_installed_over_kiln_json(name):
        # the record wins over a kiln.json that says the opposite, and over KILN_SHROUDFORGE_RUNTIME
        for suffix, backend, code, kiln_json, env, source in [
            ('-hosted', HOSTED, 0x002, {'shroudforge': {'backend': 'installed'}}, {'KILN_SHROUDFORGE_RUNTIME': 'installed'}, 'bundled'),
            ('-installed', INSTALLED, 0x003, {'shroudforge': {'backend': 'kiln'}}, None, 'installed'),
        ]:
            r = start(root, name + suffix, kiln_json, env=env, launch=rec(backend, code))
            assert r.hosted == source and r.eml_aside, (r.status, r.out)
            assert r.status['launch'] == {'backend': WORDS[backend], 'reasonCode': code, 'reasonClass': 'selected',
                                          'reason': f'fake kiln.launch {code}', 'launchId': '0123456789abcdef'}, r.status
            assert r.status['useInstalled'] is (backend == INSTALLED), r.status
            want = f'ShroudForge backend for this start (kiln.launch): {WORDS[backend]}: fake kiln.launch {code}'
            assert r.out.count(want) == 1, r.out
    check('frozen-hosted-and-installed-over-kiln-json', frozen_hosted_and_installed_over_kiln_json)

    def frozen_kiln_stands_aside(name):
        # Kiln's interpreter has the start: no runtime, no stand-aside call to kiln-eml (it is the data writer), not with
        # KILN_SHROUDFORGE=1 (start() sets it) nor with a kiln.json and an environment that ask for installed
        r = start(root, name, {'shroudforge': {'useInstalled': True}}, env={'KILN_SHROUDFORGE_RUNTIME': 'installed'},
                  launch=rec(KILN, 0x005))
        assert r.calls == [] and r.hosted is None and not r.eml_aside, (r.calls, r.status)
        assert r.status['mode'] == 'off' and r.status['reason'] == (
            "Kiln's interpreter has this start (kiln.launch: fake kiln.launch 5): kiln-shroudforge stands aside"), r.status
        assert r.status['launch']['backend'] == 'kiln' and r.status['launch']['reasonClass'] == 'selected', r.status
        assert r.out.count('ShroudForge mods are off for this start: ' + r.status['reason']) == 1, r.out
    check('frozen-kiln-stands-aside', frozen_kiln_stands_aside)

    def frozen_refused_every_backend_stands_aside(name):
        for code in (0x100, 0x101, 0x102, 0x106):
            r = start(root, f'{name}-{code:x}', {'shroudforge': {'backend': 'hosted'}}, launch=rec(NONE, code))
            assert r.calls == [] and r.hosted is None and not r.eml_aside, (r.calls, r.status)
            assert r.status['mode'] == 'off' and r.status['reason'] == (
                f"Kiln's core refused every ShroudForge backend this start (kiln.launch: fake kiln.launch {code})"), r.status
            assert r.status['launch'] == {'backend': 'none', 'reasonCode': code, 'reasonClass': 'refused',
                                          'reason': f'fake kiln.launch {code}', 'launchId': '0123456789abcdef'}, r.status
    check('frozen-refused-every-backend-stands-aside', frozen_refused_every_backend_stands_aside)

    def frozen_shroudforge_zero_keeps_mains_note(name):
        r = start(root, name, {}, env={'KILN_SHROUDFORGE': '0'}, launch=rec(INSTALLED, 0x002))
        assert r.calls == [] and r.status['mode'] == 'off' and r.status['reason'] == 'KILN_SHROUDFORGE=0', r.status
    check('frozen-shroudforge-zero-keeps-mains-note', frozen_shroudforge_zero_keeps_mains_note)

    def frozen_keeps_the_runtime_fallback(name):
        # main's fallback inside kiln-shroudforge stays: the freeze is between backends, not between its two runtimes
        r = start(root, name, {}, launch=rec(HOSTED, 0x001), bundled=False)
        assert r.hosted == 'installed', (r.status, r.out)
        r = start(root, name + '-installed', {}, launch=rec(INSTALLED, 0x002), installed=False)
        assert r.hosted == 'bundled', (r.status, r.out)
    check('frozen-keeps-the-runtime-fallback', frozen_keeps_the_runtime_fallback)

    def self_hosted_wins_over_every_record(name):
        # ShroudForge's own loader in the process: both Kiln backends stand aside, as today
        for backend, code in [(HOSTED, 0x001), (KILN, 0x005)]:
            r = start(root, f'{name}-{WORDS[backend]}', {}, launch=rec(backend, code), sfi=bool(sfi_dll), self_hosted=True)
            assert r.status['mode'] == 'self-hosted' and r.calls == [], (r.status, r.calls)
            if sfi_dll:
                assert r.interpreter == 0, r.out
                if backend == KILN:
                    assert "ShroudForge's own loader (" in r.out and 'kiln-sfi stands aside' in r.out, r.out
    check('self-hosted-wins-over-every-record', self_hosted_wins_over_every_record)

    # unreadable tables, the idle class and no table: today's behaviour, byte for byte (the same start without a table)
    def todays_behaviour(name, launch):
        for suffix, kiln_json in [('', {'shroudforge': {'useInstalled': True}}), ('-default', None)]:
            without = start(root, f'{name}{suffix}-none', kiln_json)
            with_table = start(root, f'{name}{suffix}-table', kiln_json, launch=launch)
            assert 'launch' not in with_table.status, with_table.status
            same_start(without, with_table)
    for launch in ['short', 'v0', 'nostatus', 'unfrozen', rec(7, 0x005), rec(NONE, 0x001), rec(NONE, 0x300)]:
        check(f'unreadable-table-{launch.replace(",", "-")}', lambda name, launch=launch: todays_behaviour(name, launch))
    for code in (0x200, 0x201, 0x202, 0x203, 0x204):
        check(f'idle-{code:x}-is-todays-behaviour', lambda name, code=code: todays_behaviour(name, rec(NONE, code)))

    def trailing_comma_follows_the_table(name):
        # review I1: the core reads kiln.json as JSONC (trailing commas dropped), so for this file its record says
        # installed; the adapter follows the record, never its own re-read. Without a record it reads the file as before
        # (nlohmann, no trailing commas): the file does not parse and the bundled runtime runs, as today.
        text = '{"shroudforge": {"useInstalled": true,},}'
        r = start(root, name, text, launch=rec(INSTALLED, 0x003))
        assert r.hosted == 'installed' and r.status['launch']['backend'] == 'installed', (r.status, r.out)
        r = start(root, name + '-legacy', text)
        assert r.hosted == 'bundled' and 'launch' not in r.status and r.status['useInstalled'] is False, (r.status, r.out)
    check('trailing-comma-follows-the-table', trailing_comma_follows_the_table)

    def shroudforge_zero_stands_every_backend_aside(name):
        # review I2: KILN_SHROUDFORGE=0 stands every ShroudForge backend aside, Kiln's interpreter included, with one
        # reason; for hosted and installed it is main's off note as it was
        rows = [(rec(KILN, 0x005), {}), (rec(HOSTED, 0x001), {}), (rec(INSTALLED, 0x002), {}), (rec(NONE, 0x101), {}),
                (None, {'shroudforge': {'backend': 'kiln'}}), (None, {'shroudforge': {'backend': 'Kiln'}}), (None, {})]
        for i, (launch, kiln_json) in enumerate(rows):
            r = start(root, f'{name}-{i}', kiln_json, env={'KILN_SHROUDFORGE': '0'}, launch=launch, sfi=bool(sfi_dll))
            assert r.calls == [] and not r.eml_aside, (launch, kiln_json, r.calls)
            assert r.status['mode'] == 'off' and r.status['reason'] == 'KILN_SHROUDFORGE=0', (launch, kiln_json, r.status)
            assert 'ShroudForge mods are off for this start: KILN_SHROUDFORGE=0' in r.out, r.out
            if sfi_dll:
                assert r.interpreter == 0, r.out
                caps = [l for l in r.out.splitlines() if l.startswith('[fake kiln.caps] report')]
                if launch is not None:
                    assert len(caps) == 2 and all(l.endswith(': KILN_SHROUDFORGE=0') for l in caps), caps
    check('shroudforge-zero-stands-every-backend-aside', shroudforge_zero_stands_every_backend_aside)

    # ---- exactly one of hosted, installed and Kiln's interpreter per record (kiln-sfi.dll in the same host) ----
    def exactly_one(name):
        rows = [  # record, kiln.json, which one starts
            (rec(HOSTED, 0x001), {'shroudforge': {'useInstalled': True}}, 'bundled'),
            (rec(INSTALLED, 0x002), {}, 'installed'),
            (rec(KILN, 0x005), {'shroudforge': {'useInstalled': True}}, 'interpreter'),
            (rec(NONE, 0x101), {}, None),
            (rec(NONE, 0x100), {}, None),
            # the idle class: kiln-shroudforge's own rules (KILN_SHROUDFORGE=1 hosts), kiln-sfi does nothing
            (rec(NONE, 0x200), {'shroudforge': {'backend': 'installed'}}, 'installed'),
            (None, {}, 'bundled'),
        ]
        for i, (launch, kiln_json, which) in enumerate(rows):
            r = start(root, f'{name}-{i}', kiln_json, launch=launch, sfi=True)
            started = ([r.hosted] if r.hosted else []) + ['interpreter'] * r.interpreter
            assert started == ([which] if which else []), (launch, started, r.status, r.out)
            assert r.eml_aside == (r.hosted is not None), (launch, r.calls)
            caps = [l for l in r.out.splitlines() if l.startswith('[fake kiln.caps] report')]
            assert len(caps) == 2 and all('kiln-sfi.interpreter state=0 reason=6' in l for l in caps), r.out
            if which == 'interpreter':
                assert 'this start has no profile kiln.profile can give: no ShroudForge mod runs this start' in r.out, r.out
                assert 'interpreter start (stub)' not in r.out, r.out
            if launch is None:
                assert all(l.endswith(': backend negotiation not integrated') for l in caps), caps
    check('exactly-one-per-record', exactly_one, needs_sfi=True)
