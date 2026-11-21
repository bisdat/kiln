#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# the two-DLL game-free integration of kiln.eml.startup v1.
"""The REAL kiln-eml.dll and kiln-sfi.dll loaded together by a test host (host.cpp: kiln-core's boot order with fake
core services, no game), under headless Wine and natively on Windows. Every kiln-backend step also runs kiln.boot's
bound() (the core's boot_archive_eml.h over kiln.eml.startup's published_identity) and one OnBoot read, and kiln.progress
is served as the core serves it (check_every_step).

  run.py build   --out DIR                       host.exe and the package DLLs (clang-cl + lld-link, the xwin SDK)
  run.py prepare --out DIR --eml DLL --sfi DLL --gen BIN [--case NAME...]
                 one game folder per case below DIR/cases/<case>/template (BIN: kiln-eml's m6_two_dll_fixture test
                 binary, which writes the synthetic archive, the packages, the frozen launch plan and the profile)
  run.py wine    --out DIR [--case NAME...] [--repeat N]   every step of every case under Wine (G14D_WINE, else the
                 newest GE-Proton's own Wine when installed, else PATH's; private prefix DIR/wineprefix, headless),
                 results below DIR/runs/<case>/<n>
  run.py check   --out DIR --gen BIN [--runs DIR/runs] [--case NAME...]   the assertions, on Wine's or Windows' results
  run.py payload --out DIR --to PAYLOAD          the cases and run-native.ps1 laid out for a native Windows runner

The DLLs are built the way the release builds them (gpl/kiln-eml/build.sh, crates/sfi/kiln-sfi/build.sh); the host and
the package DLLs are synthetic. Nothing here loads a game binary or touches anything outside DIR.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import threading
import time

HERE = Path(__file__).resolve().parent
LAUNCH_ID = '14d0000000000042'
PACKAGE_DLLS = ['dllpack']  # one build each, its own G14D_DLL_ID (so its own bytes)


def P(id, caps, data, lua, requires=(), dlls=(), kind='shroudforge', lua_src=None):
    p = {'id': id, 'kind': kind, 'capabilities': list(caps), 'data': data, 'lua': lua, 'requires': list(requires),
         'dlls': [{'path': f'{d}.dll', 'dll': d} for d in dlls]}
    if lua_src is not None:
        p['lua_src'] = lua_src
    return p


ALPHA = P('alpha', ['patch'], True, False)
BETA = P('beta', ['patch'], True, False, ['alpha'])
DLLPACK = P('dllpack', ['patch'], True, False, dlls=['dllpack'])
RT_ALPHA = P('rt-alpha', ['runtime'], False, True, ['alpha'])
RT_DLL = P('rt-dll', ['runtime'], False, True, ['dllpack'])
# a data package that registers DLLs from its Lua (runtime-register-dll) and a Lua package that needs it
REGDLL = P('regdll', ['patch', 'runtime-register-dll'], True, True)
RT_REGDLL = P('rt-regdll', ['runtime'], False, True, ['regdll'])
BASIC = [ALPHA, DLLPACK, RT_ALPHA, RT_DLL]
# A data package whose startup Lua writes the synthetic archive's resource part 0 (child.extra), the value
# kiln.boot's OnBoot read must then answer from the committed generation (never the BASE's)
ONBOOT_VALUE = 7514
EDIT = P('edit', ['patch'], True, False, lua_src=(
    'game.assets.get_resource("01234567-89ab-cdef-0123-456789abcdef", "m6.Root", 0).data.child.extra = %d\n'
    'game.assets.create_content(buffer.wrap("edit blob"))\nshroudforge.log.info("edit blob ran")' % ONBOOT_VALUE))

# Each case: the packages (and extras) of its game folder, and its starts. Case keys: xhl_receipt (an EML Library copy
# the plan does not list, with a Linux launcher's placement receipt), dropins (the profile lets kiln-eml discover the EML
# drop-in folders, so kiln mode's discovery runs in full), shroudforge_decoy (ShroudForge's folder with an applied
# record), plugins. A step's keys: dll (package -> what its DLL's initializer does, package_dll.c), touch (the file the
# touch DLL appends to), policy (kiln.json native.policy as the core froze it), kill_at (the host ends itself there),
# wait_dllmain (kiln-eml's early on_ready starts only once the DLL's initializer has begun, so it runs under the loader
# lock), worker_wait (ms), timeout (s), hold_lock (another process holds the install lock), watch_sf (ShroudForge's
# folder is watched: Wine inotify; Windows a deny-all ACL for the start).
DISCOVERY = {'xhl_receipt': True, 'dropins': True}
CASES = {
    'happy': {'packages': [ALPHA, BETA, DLLPACK, RT_ALPHA, RT_DLL, EDIT], **DISCOVERY, 'steps': [{}, {}]},
    'input-changed': {'packages': BASIC, 'steps': [{'dll': {'dllpack': 'touch'}, 'touch': 'enshrouded.kfc_resources'}]},
    # kiln-eml's real on_ready (discovery included) while a slow initializer holds the loader lock;
    # it returns before ShroudForge's 15 s deadline, so its package loads
    'slow-dllmain': {'packages': BASIC, **DISCOVERY,
                     'steps': [{'dll': {'dllpack': 'sleep:8000'}, 'wait_dllmain': True, 'timeout': 180}]},
    # the same past the deadline: the deadline thread pushes the terminal at 15 s, the package is withdrawn
    'slow-past-deadline': {'packages': BASIC, **DISCOVERY,
                           'steps': [{'dll': {'dllpack': 'sleep:25000'}, 'wait_dllmain': True, 'timeout': 180}]},
    # the same with kiln-eml's discovery of drop-ins off (the profile's default): only the barrier, pass, publication
    # and grant run under the held loader lock
    'past-deadline-plain': {'packages': BASIC,
                            'steps': [{'dll': {'dllpack': 'sleep:25000'}, 'wait_dllmain': True, 'timeout': 180}]},
    # an initializer that never returns: the terminal still comes at 15 s and kiln-eml's barrier ends; the host is
    # stopped at the step's timeout (the start cannot go on: the loader lock is never released)
    'hung-dllmain': {'packages': BASIC, **DISCOVERY,
                     'steps': [{'dll': {'dllpack': 'hang'}, 'wait_dllmain': True, 'worker_wait': 6000, 'timeout': 60}]},
    'not-armed-nodll': {'packages': [ALPHA, REGDLL, RT_ALPHA, RT_REGDLL], 'steps': [{}]},
    'not-armed-never': {'packages': [ALPHA, DLLPACK, REGDLL, RT_ALPHA, RT_DLL, RT_REGDLL], 'steps': [{'policy': 'never'}]},
    'cached-rewrite': {'packages': [ALPHA, BETA, DLLPACK, RT_ALPHA],
                       'steps': [{}, {'dll': {'dllpack': 'rewrite-highest'}}, {}]},
    'sf-untouched': {'packages': BASIC, 'shroudforge_decoy': True, 'steps': [{'watch_sf': True}]},
    'lock-busy': {'packages': BASIC, 'steps': [{'hold_lock': True}]},
    'no-sfi': {'packages': BASIC, 'plugins': ['kiln-eml.dll'], 'steps': [{}]},
    'kill-dll-window': {'packages': BASIC, 'steps': [{'dll': {'dllpack': 'sleep:3000'}, 'kill_at': 'after-early-main'}, {}]},
    'kill-after-record': {'packages': BASIC, 'steps': [{'kill_at': 'after-early-ready'}, {}]},
}


def sha256(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def xwin_paths(xwin):
    include = [xwin / 'crt/include', xwin / 'sdk/include/ucrt', xwin / 'sdk/include/um', xwin / 'sdk/include/shared']
    libs = [xwin / 'crt/lib/x86_64', xwin / 'sdk/lib/ucrt/x86_64', xwin / 'sdk/lib/um/x86_64']
    if not all(p.is_dir() for p in include + libs):
        raise SystemExit(f'no xwin SDK under {xwin}; no downloads attempted')
    return include, libs


def run_logged(command, log, cwd):
    result = subprocess.run(command, cwd=cwd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    log.write_text(result.stdout)
    if result.returncode:
        raise SystemExit(f'{command[0]} failed:\n{result.stdout}')


def build(args):
    out = args.out.resolve() / 'bin'
    out.mkdir(parents=True, exist_ok=True)
    include, libs = xwin_paths(args.xwin)
    inc = [a for p in include for a in ['-imsvc', str(p)]]
    abi = HERE.parent.parent / 'abi'
    src = HERE.parent.parent / 'src'  # kiln.boot's committed view (services/boot_archive_eml.h) and its JSON library
    third_party = HERE.parents[3] / 'native' / 'third_party'
    run_logged(['clang-cl', '--target=x86_64-pc-windows-msvc', '/nologo', '/std:c++20', '/EHsc', '/W4', '/WX', '/O1', '/MT',
                '-Wno-msvc-not-found', '-fuse-ld=lld', '/DNOMINMAX', *inc, f'-I{abi}', f'-I{src}', f'-I{src / "services"}',
                f'-I{third_party}', str(HERE / 'host.cpp'), f'/Fo{out}/host.obj', f'/Fe{out}/enshrouded.exe', '/link',
                *[f'/libpath:{p}' for p in libs], 'advapi32.lib', 'user32.lib'], out / 'host.log', out)
    for n, name in enumerate(PACKAGE_DLLS, 1):
        run_logged(['clang-cl', '--target=x86_64-pc-windows-msvc', '/nologo', '/c', '/W4', '/WX', '/O1', '/GS-', *inc,
                    f'/DG14D_DLL_ID={n}', str(HERE / 'package_dll.c'), f'/Fo{out}/{name}.obj'], out / f'{name}.cc.log', out)
        run_logged(['lld-link', '/nologo', *[f'/LIBPATH:{p}' for p in libs], str(out / f'{name}.obj'), f'/OUT:{out}/{name}.dll',
                    '/DLL', '/ENTRY:DllMain', '/NODEFAULTLIB', 'kernel32.lib'], out / f'{name}.link.log', out)
    print('built', out)


def gen_env(out):
    tmp = out / 'tmp'
    tmp.mkdir(parents=True, exist_ok=True)
    return dict(os.environ, TMPDIR=str(tmp))


def prepare(args):
    out = args.out.resolve()
    binary = out / 'bin'
    # what every case's starts share, copied into each run's game folder by the runners: the two real plugin DLLs (as
    # build.sh built them) and the lock holder (the host, under another name and outside the game folder)
    shared = out / 'shared'
    shared.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(args.eml, shared / 'kiln-eml.dll')
    shutil.copyfile(args.sfi, shared / 'kiln-sfi.dll')
    shutil.copyfile(binary / 'enshrouded.exe', shared / 'lockholder.exe')
    (shared / 'sha256.json').write_text(json.dumps({n: sha256(shared / n) for n in ['kiln-eml.dll', 'kiln-sfi.dll']}))
    for name in selected(args):
        case = CASES[name]
        folder = out / 'cases' / name
        if folder.exists():
            shutil.rmtree(folder)
        folder.mkdir(parents=True)
        packages = []
        for p in case['packages']:
            q = dict(p)
            q['dlls'] = [{'path': d['path'], 'from': str(binary / f"{d['dll']}.dll")} for d in p['dlls']]
            packages.append(q)
        spec = {'out': str(folder / 'template'), 'host': str(binary / 'enshrouded.exe'), 'packages': packages,
                'xhl_receipt': case.get('xhl_receipt', False), 'shroudforge_decoy': case.get('shroudforge_decoy', False),
                'dropins': case.get('dropins', False)}
        (folder / 'template').mkdir()
        (folder / 'spec.json').write_text(json.dumps(spec, indent=1))
        result = subprocess.run([str(args.gen), '--exact', 'g14d_fixture_from_spec', '--ignored', '--nocapture',
                                 '--test-threads=1'], env=dict(gen_env(out), G14D_FIXTURE_SPEC=str(folder / 'spec.json')),
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        if result.returncode:
            raise SystemExit(f'{name}: fixture failed\n{result.stdout}')
        meta = json.loads((folder / 'template' / 'fixture.json').read_text())
        steps = []
        for n, step in enumerate(case['steps'], 1):
            e = {'G14D_GAME': '{game}', 'G14D_LOG': '{run}\\host-%d.log' % n, 'G14D_DLL_LOG': '{run}\\dll-%d.log' % n,
                 'G14D_BASELINE': '{game}\\enshrouded.kfc.bak', 'G14D_EXE_SHA256': meta['exe_sha256'],
                 'G14D_PLAN_SHA256': meta['plan_sha256'], 'G14D_LAUNCH_ID': LAUNCH_ID,
                 'G14D_POLICY': step.get('policy', 'always'), 'G14D_WORKER_WAIT_MS': str(step.get('worker_wait', 60000))}
            if step.get('kill_at'):
                e['G14D_KILL_AT'] = step['kill_at']
            for package, action in step.get('dll', {}).items():
                e['G14D_DLL_' + package.upper().replace('-', '_')] = action
            if step.get('touch'):
                e['G14D_TOUCH_FILE'] = '{game}\\' + step['touch']
            if step.get('wait_dllmain'):
                e['G14D_WAIT_DLLMAIN'] = '1'
            steps.append({'env': e, 'timeout': step.get('timeout', 120), 'hold_lock': step.get('hold_lock', False),
                          'watch_sf': step.get('watch_sf', False)})
        plugins = case.get('plugins', ['kiln-eml.dll', 'kiln-sfi.dll'])
        (folder / 'case.json').write_text(json.dumps({'name': name, 'meta': meta, 'plugins': plugins, 'steps': steps},
                                                     indent=1))
        print('prepared', name)


def selected(args):
    names = args.case or list(CASES)
    unknown = [n for n in names if n not in CASES]
    if unknown:
        raise SystemExit(f'unknown case {unknown}')
    return names


def wine_path(p):
    return 'Z:' + str(p).replace('/', '\\')


SNAPSHOT_FILES = ('enshrouded.kfc', 'enshrouded.kfc_resources', 'enshrouded.kfc.bak', 'enshrouded.kfc_resources.bak',
                  'enshrouded.kfc.bak.integrity')


def snapshot(game, dest):
    """What the checks read after a start: kiln's state, logs and eml folder, the archive, ShroudForge's folder."""
    dest.mkdir(parents=True, exist_ok=True)
    for name in os.listdir(game):
        if name in SNAPSHOT_FILES or (name.startswith('enshrouded_') and name.endswith('.dat')):
            shutil.copyfile(game / name, dest / name)
    for sub in ['kiln/state', 'kiln/logs', 'kiln/eml', 'shroudforge']:
        if (game / sub).is_dir():
            shutil.copytree(game / sub, dest / sub)


class Inotify:
    """Every event below a folder tree (IN_ALL_EVENTS) while it runs: Wine's file accesses there, through Linux."""

    def __init__(self, root):
        import ctypes
        self.libc = ctypes.CDLL(None, use_errno=True)
        self.fd = self.libc.inotify_init1(os.O_NONBLOCK)
        if self.fd < 0:
            raise OSError(ctypes.get_errno(), 'inotify_init1')
        self.watches = {}
        for folder in [root] + [Path(d) for d, _, _ in os.walk(root) if Path(d) != root]:
            wd = self.libc.inotify_add_watch(self.fd, str(folder).encode(), 0xFFF)
            if wd < 0:
                raise OSError(ctypes.get_errno(), f'inotify_add_watch {folder}')
            self.watches[wd] = folder

    def events(self):
        import struct
        out = []
        try:
            data = os.read(self.fd, 1 << 20)
        except BlockingIOError:
            data = b''
        while data:
            at = 0
            while at < len(data):
                wd, mask, _, size = struct.unpack_from('iIII', data, at)
                name = data[at + 16:at + 16 + size].rstrip(b'\0').decode(errors='replace')
                out.append({'path': str(self.watches.get(wd, '?')) + ('/' + name if name else ''), 'mask': mask})
                at += 16 + size
            try:
                data = os.read(self.fd, 1 << 20)
            except BlockingIOError:
                data = b''
        os.close(self.fd)
        return out


def wine_binaries():
    """The Wine the runs use, (wine, wineserver): G14D_WINE (a wine binary, its wineserver beside it), else the newest
    GE-Proton's own Wine when Steam has one (the Wine the game runs under on the test PC: GE-Proton11's Wine 11.0 refused a
    directory listing that the system's Wine 11.20 allowed, so a run under the system's Wine alone hid it), else the
    wine on PATH."""
    chosen = os.environ.get('G14D_WINE')
    if not chosen:
        tools = Path.home() / '.local/share/Steam/compatibilitytools.d'
        def version(p):
            return [int(n) for n in ''.join(c if c.isdigit() else ' ' for c in p.name).split()]
        found = sorted((p for p in tools.glob('GE-Proton*') if (p / 'files/bin/wine').is_file()), key=version)
        if found:
            chosen = str(found[-1] / 'files/bin/wine')
    if not chosen:
        return 'wine', 'wineserver'
    server = Path(chosen).with_name('wineserver')
    return chosen, str(server) if server.is_file() else 'wineserver'


def wine(args):
    out = args.out.resolve()
    wine_bin, wineserver = wine_binaries()
    prefix = out / 'wineprefix'
    home, runtime = out / 'home', out / 'xdg-runtime'
    home.mkdir(exist_ok=True)
    runtime.mkdir(mode=0o700, exist_ok=True)
    env = dict(os.environ, HOME=str(home), XDG_DATA_HOME=str(home / 'data'), XDG_CONFIG_HOME=str(home / 'config'),
               XDG_CACHE_HOME=str(home / 'cache'), XDG_RUNTIME_DIR=str(runtime), WINEPREFIX=str(prefix), WINEARCH='win64',
               WINEDEBUG='-all', WAYLAND_DISPLAY='kiln-no-display', WINEDLLOVERRIDES='winedbg.exe=d;winemenubuilder.exe=d;mscoree,mshtml=')
    env.pop('DISPLAY', None)
    for k in [k for k in env if k.startswith(('KILN_', 'G14D_'))]:
        env.pop(k)
    if not (prefix / 'system.reg').exists():
        subprocess.run([wine_bin, 'wineboot', '-i'], env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=300)
        subprocess.run([wineserver, '-w'], env=env, timeout=300)
    wine_version = subprocess.run([wine_bin, '--version'], env=env, capture_output=True, text=True).stdout.strip()
    print(f'wine: {wine_bin} ({wine_version})', flush=True)
    runs = (args.runs or out / 'runs').resolve()
    winedebug = os.environ.get('G14D_WINEDEBUG')  # a Wine debug channel set for the host's starts (a trace)
    for name in selected(args):
        folder = out / 'cases' / name
        case = json.loads((folder / 'case.json').read_text())
        for r in range(1, args.repeat + 1):
            run = runs / name / str(r)
            if run.exists():
                shutil.rmtree(run)
            run.mkdir(parents=True)
            game = run / 'game'
            shutil.copytree(folder / 'template' / 'game', game)
            (game / 'kiln' / 'plugins').mkdir(parents=True, exist_ok=True)
            for dll in case['plugins']:
                shutil.copyfile(out / 'shared' / dll, game / 'kiln' / 'plugins' / dll)
            for n, step in enumerate(case['steps'], 1):
                subs = {'{game}': wine_path(game), '{run}': wine_path(run)}
                e = dict(env)
                for k, v in step['env'].items():
                    for a, b in subs.items():
                        v = v.replace(a, b)
                    e[k] = v
                if winedebug:
                    e['WINEDEBUG'] = winedebug
                holder = None
                if step['hold_lock']:
                    ready, stop = run / f'lock-ready-{n}', run / f'lock-stop-{n}'
                    holder = subprocess.Popen([wine_bin, str(out / 'shared' / 'lockholder.exe'), '--hold-lock', wine_path(game / '.kiln-eml.lock'),
                                               wine_path(ready), wine_path(stop)], env=env, stdout=subprocess.DEVNULL,
                                              stderr=subprocess.DEVNULL)
                    for _ in range(600):
                        if ready.exists() or holder.poll() is not None:
                            break
                        time.sleep(0.05)
                    if not ready.exists():
                        raise SystemExit(f'{name}: the lock holder did not take the lock (exit {holder.poll()})')
                watch = None
                if step['watch_sf']:
                    # Wine's own trace of every file access through ntdll, and Linux's view of the folder
                    subprocess.run([wineserver, '-w'], env=env, timeout=300)
                    e['WINEDEBUG'] = '+file'
                    watch = Inotify(game / 'shroudforge')
                started = time.monotonic()
                with open(run / f'wine-{n}.out', 'wb') as sink:
                    p = subprocess.Popen([wine_bin, str(game / 'enshrouded.exe')], cwd=game, env=e, stdout=sink,
                                         stderr=subprocess.STDOUT)
                    try:
                        rc = p.wait(timeout=step['timeout'])
                        timed_out = False
                    except subprocess.TimeoutExpired:
                        p.kill()  # this run's own process, by its exact PID
                        rc = p.wait()
                        timed_out = True
                elapsed = time.monotonic() - started
                events = watch.events() if watch else None
                if holder:
                    (run / f'lock-stop-{n}').write_text('stop')
                    holder.wait(timeout=60)
                subprocess.run([wineserver, '-w'], env=env, timeout=300)
                snapshot(game, run / f'step-{n}')
                result = {'rc': rc, 'elapsed': round(elapsed, 3), 'timed_out': timed_out, 'runner': 'wine', 'wine': wine_version}
                if events is not None:
                    result['inotify'] = events
                (run / f'step-{n}' / 'result.json').write_text(json.dumps(result, indent=1))
                print(f'{name} run {r} step {n}: rc={rc} {elapsed:.1f}s{" TIMEOUT" if timed_out else ""}', flush=True)


def payload(args):
    out = args.out.resolve()
    to = args.to.resolve()
    if to.exists():
        shutil.rmtree(to)
    (to / 'cases').mkdir(parents=True)
    for name in selected(args):
        shutil.copytree(out / 'cases' / name, to / 'cases' / name, ignore=shutil.ignore_patterns('spec.json'))
    shutil.copytree(out / 'shared', to / 'shared')
    shutil.copyfile(HERE / 'run-native.ps1', to / 'run-native.ps1')
    (to / 'cases.txt').write_text('\n'.join(selected(args)) + '\n')
    print('payload', to)


# ---- the checks ----

def parse_host(path):
    lines = []
    if not path.exists():
        return lines
    for raw in path.read_text(errors='replace').splitlines():
        parts = raw.split(' ', 4)
        if len(parts) < 5:
            continue
        lines.append({'qpc': int(parts[0]), 'ms': float(parts[1]), 'tid': parts[2], 'tag': parts[3], 'text': parts[4]})
    return lines


def parse_dll(path):
    out = []
    if not path.exists():
        return out
    for raw in path.read_text(errors='replace').splitlines():
        fields = raw.split(' ', 4)
        out.append({'qpc': int(fields[0][4:]), 'tid': fields[1][4:], 'stem': fields[3], 'text': fields[4] if len(fields) > 4 else ''})
    return out


class Step:
    def __init__(self, run, n, gen, out):
        self.run, self.n = run, n
        self.dir = run / f'step-{n}'
        self.result = json.loads((self.dir / 'result.json').read_text())
        self.host = parse_host(run / f'host-{n}.log')
        self.dll = parse_dll(run / f'dll-{n}.log')
        start = next((l for l in self.host if l['tag'] == 'host' and l['text'].startswith('start ')), None)
        self.freq = int(start['text'].split('qpc-frequency=')[1].split()[0]) if start else 10_000_000
        self.qpc0 = start['qpc'] if start else 0
        self.eml = self.json('kiln/state/eml.json')
        self.sfi = self.json('kiln/state/sfi.json')
        self._contents = None
        self.gen, self.out = gen, out
        self.owners = {}
        for l in self.host:
            if l['tag'] == 'host' and l['text'].startswith('owner '):
                _, o, who = l['text'].split(' ', 2)
                self.owners[who] = o

    def json(self, rel):
        p = self.dir / rel
        return json.loads(p.read_text()) if p.exists() else None

    def ms(self, qpc):
        return (qpc - self.qpc0) * 1000.0 / self.freq

    def mark(self, prefix):
        for l in self.host:
            if l['tag'] == 'host' and l['text'].startswith(prefix):
                return l
        return None

    def at(self, prefix):
        l = self.mark(prefix)
        return self.ms(l['qpc']) if l else None

    def core(self, owner_of=None):
        """The core log's lines (owner, level, text), of one plugin's owner (e.g. 'plugin:kiln-sfi.dll') when given."""
        out = []
        for l in self.host:
            if l['tag'] != 'core' or not l['text'].startswith('log o='):
                continue
            head, _, text = l['text'][6:].partition(' l=')
            level, _, text = text.partition(' ')
            if owner_of is None or self.owners.get(owner_of) == head:
                out.append((head, int(level), text, self.ms(l['qpc'])))
        return out

    def logged(self, needle):
        return any(needle in t for _, _, t, _ in self.core())

    def dll_event(self, stem, event):
        for d in self.dll:
            if d['stem'] == stem and d['text'].startswith(event):
                return d
        return None

    def contents(self):
        if self._contents is None:
            target = self.dir / 'contents.json'
            result = subprocess.run([str(self.gen), '--exact', 'g14d_verify_archive', '--ignored', '--test-threads=1'],
                                    env=dict(gen_env(self.out), G14D_VERIFY_GAME=str(self.dir), G14D_VERIFY_OUT=str(target)),
                                    stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
            if result.returncode:
                raise AssertionError(f'verify failed: {result.stdout}')
            self._contents = json.loads(target.read_text())
        return self._contents

    def blobs(self):
        c = self.contents()
        assert c['valid'], c
        return sorted(x for x in c['contents'] if x.endswith(' blob'))

    def eml_row(self, id):
        for m in (self.eml or {}).get('mods', []):
            if m.get('id') == id or str(m.get('path', '')).replace('\\', '/').endswith('/' + id):
                return m
        return None

    def sfi_row(self, id):
        for p in (self.sfi or {}).get('packages', []):
            if p['id'] == id:
                return p
        return None

    def ran(self, id):
        """kiln-sfi's runtime ran the package's Lua (its log line, under kiln-sfi's plugin owner)."""
        return any(f'{id} ran' in t for _, _, t, _ in self.core('plugin:kiln-sfi.dll'))

    def summary(self):
        rows = {}
        for p in (self.sfi or {}).get('packages', []):
            rows[p['id']] = {'data': p['data'], 'lua': p['lua'], 'present': p['archivePresent'],
                             'skip': (p.get('skip') or {}).get('code'), 'warning': (p.get('warning') or {}).get('presence')}
        eml = {(m.get('id') or m.get('path')): (m.get('state'), m.get('source'), m.get('reason')) for m in (self.eml or {}).get('mods', [])}
        marks = {l['text'].split(' ')[0] + ' ' + (l['text'].split(' ')[1] if ' ' in l['text'] else ''): round(self.ms(l['qpc']), 1)
                 for l in self.host if l['tag'] == 'host'}
        return {'result': self.result, 'sfi_phase': (self.sfi or {}).get('phase'), 'sfi_reason': (self.sfi or {}).get('reason'),
                'sfi_rows': rows, 'eml_phase': (self.eml or {}).get('phase'), 'eml_error': (self.eml or {}).get('error'),
                'eml_rows': eml, 'marks': marks,
                'dll': [(d['stem'], d['text'], round(self.ms(d['qpc']), 1)) for d in self.dll],
                'lua': [t for _, _, t, _ in self.core('plugin:kiln-sfi.dll') if ' ran' in t]}


def check(args):
    out = args.out.resolve()
    runs = (args.runs or out / 'runs').resolve()
    print('kiln-eml.dll: none of', check_dll(out), 'failpoints')
    failures, passes = [], 0
    report = {}
    for name in selected(args):
        case = json.loads((out / 'cases' / name / 'case.json').read_text())
        if not (runs / name).is_dir():
            failures.append(f'{name}: no runs')
            continue
        for run in sorted((runs / name).iterdir(), key=lambda p: int(p.name) if p.name.isdigit() else 0):
            steps = [Step(run, n, args.gen, out) for n in range(1, len(case['steps']) + 1)]
            key = f'{name}/{run.name}'
            report[key] = [s.summary() for s in steps]
            for s in steps:
                s.case_meta = case['meta']
            try:
                CHECKS[name](steps, case)
                check_every_step(steps, case)
                passes += 1
                print('PASS', key)
                for n, s in enumerate(steps, 1):
                    extra = {k: getattr(s, k) for k in ('timings', 'cached_rewrite', 'onboot') if hasattr(s, k)}
                    if extra:
                        report[key][n - 1]['measured'] = extra
                        print('   ', f'step {n}', json.dumps(extra, default=str))
            except AssertionError as e:
                failures.append(f'{key}: {e}')
                print('FAIL', key, str(e)[:6000])
                for n, s in enumerate(steps, 1):
                    if hasattr(s, 'timings'):
                        print('   ', f'step {n}', json.dumps(s.timings))
    (runs / 'report.json').write_text(json.dumps(report, indent=1, default=str))
    print(f'{passes} passed, {len(failures)} failed')
    if failures:
        raise SystemExit('\n'.join(failures))


# ---- what each case must show (the same on Wine and on native Windows) ----

def need(cond, what, s=None):
    if not cond:
        detail = json.dumps(s.summary(), default=str)[:3000] if s is not None else ''
        raise AssertionError(f'{what}\n{detail}')


def ms_between(s, a, b):
    """Milliseconds between two host marks (or None)."""
    x, y = s.at(a), s.at(b)
    return None if x is None or y is None else y - x


def line_at(s, needle):
    for _, _, text, ms in s.core():
        if needle in text:
            return ms
    return None


def early_order(s, dll=True):
    """The core's order and the witness before the first package DLL: kiln-eml's early step, then kiln-sfi's (attach),
    every early on_ready after both; a package DLL's initializer starts only after kiln-eml's early step, when the
    working baseline its witness captures is on disk. `dll`: the step loads dllpack.dll, so its initializer must be seen
    to begin (a step whose DLL never attached would pass the order check with nothing checked)."""
    eml_end, sfi_begin = s.at('early-main-end kiln-eml.dll'), s.at('early-main-begin kiln-sfi.dll')
    need(eml_end is not None and sfi_begin is not None and eml_end <= sfi_begin, 'kiln-eml early step first', s)
    attached = [d for d in s.dll if d['text'].startswith('attach-begin')]
    if dll:
        need(any(d['stem'] == 'dllpack' for d in attached), 'no package DLL attached: the order of the witness and the '
             'first package DLL was not checked', s)
    for d in attached:
        need(s.ms(d['qpc']) > eml_end, f"{d['stem']} loaded before kiln-eml's witness", s)
        need('baseline=1' in d['text'], f"{d['stem']} loaded before the working baseline existed", s)


def rows(s, expected):
    """sfi.json rows: id -> (data, lua, archivePresent, skip code)."""
    for id, (data, lua, present, skip) in expected.items():
        r = s.sfi_row(id)
        need(r is not None, f'no sfi.json row {id}', s)
        got = (r['data'], r['lua'], r['archivePresent'], (r.get('skip') or {}).get('code'))
        need(got == (data, lua, present, skip), f'{id}: {got} != {(data, lua, present, skip)}', s)


def lua_ran(s, ids):
    ran = sorted(t.split('] ')[1].split(' ran')[0] for _, _, t, _ in s.core('plugin:kiln-sfi.dll') if ' ran types=' in t)
    need(ran == sorted(ids), f'Lua ran {ran}, expected {sorted(ids)}', s)
    need(all('types=true' in t for _, _, t, _ in s.core('plugin:kiln-sfi.dll') if ' ran types=' in t),
         'a runtime package found no granted archive (game.types)', s)


def blobs(s, ids):
    need(s.blobs() == sorted(f'{i} blob' for i in ids), f'archive blobs {s.blobs()} != {ids}', s)


def eml_row(s, id, state, contains=None):
    r = s.eml_row(id)
    need(r is not None and r['state'] == state, f'eml.json {id}: {r}', s)
    if contains:
        need(contains in (r.get('reason') or ''), f'eml.json {id} reason: {r.get("reason")}', s)


def committed(s, data='committed'):
    return (data, 'not-needed', True, None)


ACTIVE = ('not-needed', 'active', None, None)


def check_happy(steps, case):
    first, second = steps
    for s in steps:
        need(s.result['rc'] == 0 and not s.result['timed_out'], 'host failed', s)
        early_order(s)
        need((s.sfi or {}).get('phase') == 'ended', 'sfi.json not ended', s)
        lua_ran(s, ['rt-alpha', 'rt-dll'])
        blobs(s, ['alpha', 'beta', 'dllpack', 'edit'])
        for id in ['alpha', 'beta', 'dllpack', 'edit']:
            r = s.eml_row(id)
            need(r and r['state'] == 'loaded' and r['source'] == 'shroudforge', f'eml.json {id}: {r}', s)
        # the Library copy the plan does not list (discovered with its receipt): skipped
        eml_row(s, 'xhl-copy', 'skipped')
        # OnBoot reads the value the startup pass committed (the cached generation's on the second start)
        need(check_onboot(s, value=ONBOOT_VALUE), 'no OnBoot read', s)
    rows(first, {'alpha': committed(first), 'beta': committed(first), 'dllpack': committed(first), 'edit': committed(first),
                 'rt-alpha': ACTIVE, 'rt-dll': ACTIVE})
    rows(second, {'alpha': committed(second, 'cached-verified'), 'beta': committed(second, 'cached-verified'),
                  'dllpack': committed(second, 'cached-verified'), 'edit': committed(second, 'cached-verified'),
                  'rt-alpha': ACTIVE, 'rt-dll': ACTIVE})
    need(second.eml.get('cached') is True, 'the second start is not a cache hit', second)


def check_input_changed(steps, case):
    (s,) = steps
    early_order(s)
    need(s.dll_event('dllpack', 'touch') is not None and 'appended' in s.dll_event('dllpack', 'touch')['text'], 'no touch', s)
    need(s.eml['phase'] == 'failed' and 'changed' in (s.eml.get('error') or ''), 'kiln-eml did not refuse the changed input', s)
    blobs(s, [])  # no publication
    need(not (s.dir / 'kiln/eml/ownership').is_dir() or not any((s.dir / 'kiln/eml/ownership').glob('*.receipt.json')),
         'a receipt was written', s)
    for id in ['alpha', 'dllpack']:
        r = s.sfi_row(id)
        need(r and r['data'] == 'excluded' and (r.get('skip') or {}).get('code') == 'startup-input-changed', f'{id}: {r}', s)
    lua_ran(s, [])
    need(s.sfi['phase'] == 'refused', 'kiln-sfi ran without a grant', s)


def lock_timings(s):
    """The loader-lock numbers of a start whose package DLL initializer is slow (ms from the host's start)."""
    begin, end = s.dll_event('dllpack', 'attach-begin'), s.dll_event('dllpack', 'attach-end')
    t = {'dllmain_begin': begin and round(s.ms(begin['qpc']), 1), 'dllmain_end': end and round(s.ms(end['qpc']), 1),
         'attach': s.at('early-main-end kiln-sfi.dll'), 'on_ready_begin': s.at('early-ready-begin kiln-eml.dll'),
         'discovery_end': line_at(s, "kiln-eml: Kiln's interpreter has this start"),
         'pass_begin': line_at(s, 'Type registry loaded'), 'on_ready_end': s.at('early-ready-end kiln-eml.dll'),
         'plugin_main_sfi_end': s.at('plugin-main-end kiln-sfi.dll'), 'plugin_ready_sfi_end': s.at('plugin-ready-end kiln-sfi.dll'),
         'onboot': s.at('onboot ')}
    t = {k: (round(v, 1) if isinstance(v, float) else v) for k, v in t.items()}
    if t['dllmain_end'] is not None and t['on_ready_end'] is not None:
        t['on_ready_under_lock'] = t['on_ready_end'] < t['dllmain_end']
    # kiln-sfi's plugin step (main and on_ready) asks the loader nothing while its run may still be inside a
    # package DLL's load, so it ends while the initializer holds the loader lock (or that never returns)
    if t['plugin_ready_sfi_end'] is not None:
        t['plugin_step_under_lock'] = t['dllmain_end'] is None or t['plugin_ready_sfi_end'] < t['dllmain_end']
    if t['attach'] is not None and t['pass_begin'] is not None:
        t['attach_to_pass_ms'] = round(t['pass_begin'] - t['attach'], 1)
    if t['on_ready_begin'] is not None and t['discovery_end'] is not None:
        t['discovery_ms'] = round(t['discovery_end'] - t['on_ready_begin'], 1)
    s.timings = t
    return t


def under_dllmain(s):
    need(s.mark('dllmain-begun') is not None, "the host did not see the initializer begin", s)
    t = lock_timings(s)
    need(t['dllmain_begin'] < t['on_ready_begin'] < (t['dllmain_end'] or 1e12),
         "kiln-eml's on_ready did not start while the initializer held the loader lock", s)
    return t


def check_slow_dllmain(steps, case):
    (s,) = steps
    need(s.result['rc'] == 0 and not s.result['timed_out'], 'host failed', s)
    early_order(s)
    under_dllmain(s)
    rows(s, {'alpha': committed(s), 'dllpack': committed(s), 'rt-alpha': ACTIVE, 'rt-dll': ACTIVE})
    blobs(s, ['alpha', 'dllpack'])
    lua_ran(s, ['rt-alpha', 'rt-dll'])
    eml_row(s, 'xhl-copy', 'skipped')


def check_slow_past_deadline(steps, case):
    (s,) = steps
    need(s.result['rc'] == 0 and not s.result['timed_out'], 'host failed', s)
    early_order(s)
    t = under_dllmain(s)
    # the deadline thread's terminal at 15 s from the arming ends kiln-eml's wait (20 s from the attach at most)
    need(t.get('attach_to_pass_ms') is not None and 14500 <= t['attach_to_pass_ms'] <= 20000,
         f"kiln-eml's pass did not start 15 s after the attach: {t}", s)
    rows(s, {'alpha': committed(s), 'dllpack': ('excluded', 'not-needed', False, 'native-refused'), 'rt-alpha': ACTIVE,
             'rt-dll': ('not-needed', 'excluded', None, 'dependency-skipped')})
    eml_row(s, 'dllpack', 'skipped', 'within 15 seconds')
    blobs(s, ['alpha'])
    lua_ran(s, ['rt-alpha'])
    # kiln-eml's barrier, pass, publication and grant finish while the initializer still holds the loader lock
    need(t.get('on_ready_under_lock') is True, f"kiln-eml's on_ready waited for the initializer: {t}", s)
    # so does kiln-sfi's plugin step (its look for ShroudForge's own loader is the early phase's)
    need(t.get('plugin_step_under_lock') is True, f"kiln-sfi's plugin step waited for the initializer: {t}", s)


def check_hung_dllmain(steps, case):
    (s,) = steps
    early_order(s)
    t = under_dllmain(s)
    need(s.dll_event('dllpack', 'attach-end') is None, 'the initializer returned', s)
    # kiln-sfi's plugin step no longer waits for it (its look for ShroudForge's own loader is the early phase's);
    # where the host stops after it is the host's own business (the real core's plugin step loads its service DLLs
    # first, which waits): recorded, not asserted
    need(t.get('plugin_step_under_lock') is True, f"kiln-sfi's plugin step waited for the initializer: {t}", s)
    stopped = [l['text'] for l in s.host if l['tag'] == 'host']
    t['host_last_mark'] = stopped[-1] if stopped else None
    t['host_timed_out'] = s.result['timed_out']
    need(t.get('attach_to_pass_ms') is not None and 14500 <= t['attach_to_pass_ms'] <= 20000,
         f"kiln-eml's pass did not start 15 s after the attach: {t}", s)
    need(t['on_ready_end'] is not None, "kiln-eml's barrier never ended", s)
    need(s.eml is not None and s.eml['phase'] == 'done', 'kiln-eml wrote no outcome', s)
    eml_row(s, 'dllpack', 'skipped', 'within 15 seconds')
    eml_row(s, 'alpha', 'loaded')
    blobs(s, ['alpha'])


def not_armed(s, refused):
    need(s.result['rc'] == 0, 'host failed', s)
    early_order(s, dll=False)
    need(not s.dll, f'a package DLL loaded: {s.dll}', s)
    expected = {'alpha': committed(s), 'regdll': ('excluded', 'excluded', False, 'native-refused'), 'rt-alpha': ACTIVE,
                'rt-regdll': ('not-needed', 'excluded', None, 'dependency-skipped')}
    for id in refused:
        expected[id] = ('excluded', 'not-needed', False, 'native-refused')
    rows(s, expected)
    eml_row(s, 'regdll', 'skipped', 'registers DLLs from its Lua')
    blobs(s, ['alpha'])  # the data package that registers DLLs from its Lua applied nothing
    lua_ran(s, ['rt-alpha'])


def check_not_armed_nodll(steps, case):
    not_armed(steps[0], [])


def check_not_armed_never(steps, case):
    s = steps[0]
    not_armed(s, ['dllpack'])
    rows(s, {'rt-dll': ('not-needed', 'excluded', None, 'dependency-skipped')})
    eml_row(s, 'dllpack', 'skipped', 'native.policy is "never"')


def check_cached_rewrite(steps, case):
    first, second, third = steps
    early_order(first)
    early_order(second)
    template = set(case['meta']['containers'])
    rows(first, {'alpha': committed(first), 'beta': committed(first), 'dllpack': committed(first), 'rt-alpha': ACTIVE})
    rewrote = second.dll_event('dllpack', 'rewrote')
    need(rewrote is not None, f'the DLL rewrote no container: {second.dll}', second)
    name = rewrote['text'].split()[-1]
    need(name not in template and (first.dir / name).is_file(),
         f'{name} is not a container the published generation added ({sorted(template)})', second)
    need((second.dir / name).stat().st_size == (first.dir / name).stat().st_size, 'the rewrite changed the size', second)
    need((second.dir / name).read_bytes() != (first.dir / name).read_bytes(), 'the rewrite changed nothing', second)
    # the cached generation's own added containers are witnessed by their file stamps, so
    # the rewrite during the DLL window is seen by the recheck before kiln-eml's pass: no cache hit, no publication
    second.cached_rewrite = {'container': name, 'eml_phase': second.eml['phase'], 'eml_error': second.eml.get('error'),
                             'sfi_phase': second.sfi['phase'], 'rows': second.summary()['sfi_rows']}
    need(second.eml['phase'] == 'failed' and 'changed' in (second.eml.get('error') or ''),
         f'the rewritten container was not seen: {second.cached_rewrite}', second)
    for id in ['alpha', 'beta', 'dllpack']:
        r = second.sfi_row(id)
        need(r and r['data'] == 'excluded' and (r.get('skip') or {}).get('code') == 'startup-input-changed', f'{id}: {r}', second)
    lua_ran(second, [])
    # the start after it does not trust that generation either (the startup cache vouches for it by its sizes only):
    # the early witness's plan check sees the archive changed since the launcher made the plan (plan-stale), so no DLL
    # loads, nothing is granted and no Lua runs until the launcher makes a new plan
    need(third.result['rc'] == 0 and third.eml.get('cached') is not True, 'the next start was a cache hit over the '
         'rewritten container', third)
    early_order(third, dll=False)
    need(not third.dll, f'a package DLL loaded over the rewritten container: {third.dll}', third)
    need(third.eml['phase'] == 'failed' and 'changed since the launcher made' in (third.eml.get('error') or ''),
         'the next start did not refuse the rewritten generation', third)
    for id in ['alpha', 'beta', 'dllpack']:
        r = third.sfi_row(id)
        need(r and r['data'] == 'excluded' and (r.get('skip') or {}).get('code') == 'plan-stale', f'{id}: {r}', third)
    need(third.sfi['phase'] == 'refused', 'kiln-sfi ran without a grant', third)
    lua_ran(third, [])


def check_sf_untouched(steps, case):
    (s,) = steps
    need(s.result['rc'] == 0, 'host failed', s)
    early_order(s)
    rows(s, {'alpha': committed(s), 'dllpack': committed(s), 'rt-alpha': ACTIVE, 'rt-dll': ACTIVE})
    blobs(s, ['alpha', 'dllpack'])
    template = out_dir(s) / 'cases' / case['name'] / 'template' / 'game' / 'shroudforge'
    for f in template.rglob('*'):
        if f.is_file():
            got = s.dir / 'shroudforge' / f.relative_to(template)
            need(got.is_file() and got.read_bytes() == f.read_bytes(), f'{f.relative_to(template)} changed', s)
    if s.result['runner'] == 'wine':
        events = s.result.get('inotify')
        need(events == [], f'ShroudForge folder accessed (inotify): {events[:20] if events else events}', s)
    else:
        acl = s.result.get('acl') or {}
        need(acl.get('effective') is True, f'the deny-all ACL was not in force: {acl}', s)
        dropped = s.mark('privileges removed:')
        need(dropped is not None and 'SeBackupPrivilege' in dropped['text'],
             f'the host kept the privilege that passes a deny ACE: {dropped}', s)
        need(acl.get('before') == acl.get('after') and acl.get('folderWrite') == acl.get('folderWriteAfter'),
             f'ShroudForge folder changed under the deny ACL: {acl}', s)


def check_lock_busy(steps, case):
    (s,) = steps
    need(s.result['rc'] == 1, 'the start was not stopped before the entry point', s)
    need(not s.dll, 'a package DLL loaded with the install lock busy', s)
    need(s.eml['phase'] == 'failed' and 'install lock' in (s.eml.get('error') or ''), 'no lock refusal', s)
    need(s.at('plugin-step') is None, 'the start went on to the plugin step', s)
    need(not (s.dir / 'enshrouded.kfc.bak').exists(), 'a baseline was captured without the install lock', s)
    blobs(s, [])


def check_no_sfi(steps, case):
    (s,) = steps
    need(s.result['rc'] == 0, 'host failed', s)
    need(s.sfi is None, 'kiln-sfi ran', s)
    ready = ms_between(s, 'early-ready-begin kiln-eml.dll', 'early-ready-end kiln-eml.dll')
    need(ready is not None and ready < 1000, f'on_ready waited for a terminal without an attach: {ready} ms', s)
    s.timings = {'on_ready_ms': round(ready, 1)}
    for id in ['alpha', 'dllpack']:
        eml_row(s, id, 'skipped', 'backend-unavailable')
    blobs(s, [])


def check_kill_dll_window(steps, case):
    killed, after = steps
    need(killed.result['rc'] == 99, 'not killed', killed)
    blobs(killed, [])
    need(after.result['rc'] == 0, 'the next start failed', after)
    early_order(after)
    rows(after, {'alpha': committed(after), 'dllpack': committed(after), 'rt-alpha': ACTIVE, 'rt-dll': ACTIVE})
    blobs(after, ['alpha', 'dllpack'])
    lua_ran(after, ['rt-alpha', 'rt-dll'])


def check_kill_after_record(steps, case):
    killed, after = steps
    need(killed.result['rc'] == 99 and killed.at('early-ready-end kiln-eml.dll') is not None, 'not killed after on_ready', killed)
    for s in steps:
        early_order(s)
    blobs(killed, ['alpha', 'dllpack'])
    rows(after, {'alpha': committed(after, 'cached-verified'), 'dllpack': committed(after, 'cached-verified'),
                 'rt-alpha': ACTIVE, 'rt-dll': ACTIVE})
    blobs(after, ['alpha', 'dllpack'])
    lua_ran(after, ['rt-alpha', 'rt-dll'])


def out_dir(s):
    return s.out


# ---- what every kiln-backend step must show ----

def fnv1a32(text):
    h = 0x811c9dc5
    for b in text.encode():
        h = ((h ^ b) * 0x01000193) & 0xffffffff
    return h


def onboot_lines(s):
    return [l['text'] for l in s.host if l['tag'] == 'host' and l['text'].startswith('onboot ')]


def check_onboot(s, value=None):
    """kiln.boot's bound() (host.cpp onboot(), over the core's boot_archive_eml.h): in a start whose startup pass granted
    a generation (eml.json done or idle) the view binds the generation kiln-eml published, with kiln.eml.startup's
    session-free identity: PUBLISHED, this launch, this plan, epoch 1, a generation; kiln.eml.archive's view acquired with
    exactly those pins; and the OnBoot read answers from it (`value`: the committed child.extra). Without a grant: game
    data unavailable with kiln-eml's reason and no read at all (never a snapshot). A step that never got to kiln.boot's
    bound (killed, stopped before the plugin step, hung under the loader lock) checks nothing; the caller says when that
    is wrong."""
    lines = onboot_lines(s)
    if not lines:
        return False
    grant = (s.eml or {}).get('phase') in ('done', 'idle')
    bound = [l for l in lines if l.startswith('onboot bound ')]
    if not grant:
        need(not bound and any(l.startswith('onboot unavailable ') and 'granted no archive generation' in l for l in lines),
             f'OnBoot without a grant: {lines}', s)
        need(not any(l.startswith('onboot read ') for l in lines), f'an OnBoot read without a grant: {lines}', s)
        return True
    need(len(bound) == 1, f'OnBoot did not bind the published generation: {lines}', s)
    fields = dict(kv.split('=', 1) for kv in bound[0].split()[2:])
    meta = s.case_meta
    need(fields['identity-rc'] == '0' and fields['phase'] == '2' and fields['epoch'] == '1' and fields['generation'] != '0',
         f'the published identity: {fields}', s)
    need(fields['launch'] == '0' * 16 + LAUNCH_ID and fields['plan'] == meta['plan_sha256'], f'the identity names another launch: {fields}', s)
    need(len(fields['digest']) == 64, f'no generation digest: {fields}', s)
    read = [l for l in lines if l.startswith('onboot read ')]
    need(len(read) == 1 and read[0].startswith('onboot read rc=0 '), f'the OnBoot read: {read}', s)
    got = json.loads(read[0].split(' ', 3)[3])
    need(got['resourceId'] == '01234567-89ab-cdef-0123-456789abcdef' and got['typeHash'] == '%08x' % fnv1a32('m6.Root')
         and got['part'] == 0, f'the OnBoot read named another resource: {got}', s)
    if value is not None:
        need(got['value']['child']['extra'] == value, f'OnBoot read {got["value"]["child"]}, not the committed {value}', s)
    need('onboot released' in lines, f'the view was not released: {lines}', s)
    s.onboot = {'digest': fields['digest'][:16], 'generation': fields['generation'], 'child': got['value'].get('child')}
    return True


def check_progress(s):
    """kiln-eml opens no progress window in a kiln-mode start: the core's first begin asks
    user32 for the screen size, which can load Wine's display driver under a package DLL's loader lock."""
    begun = [l['text'] for l in s.host if l['tag'] == 'host' and l['text'].startswith('progress ')]
    need(not begun, f'kiln.progress was used in a kiln-mode start: {begun}', s)


def check_every_step(steps, case):
    for n, s in enumerate(steps, 1):
        check_progress(s)
        reached = check_onboot(s)
        killed = s.result['rc'] == 99
        stopped = s.mark('plugin-step') is None  # the start was stopped before Kiln's plugin step (lock-busy)
        hung = s.result['timed_out']
        need(reached or killed or stopped or hung, f'step {n} never got to kiln.boot\'s bound', s)


CHECKS = {
    'happy': check_happy, 'input-changed': check_input_changed, 'slow-dllmain': check_slow_dllmain,
    'slow-past-deadline': check_slow_past_deadline, 'past-deadline-plain': check_slow_past_deadline, 'hung-dllmain': check_hung_dllmain,
    'not-armed-nodll': check_not_armed_nodll, 'not-armed-never': check_not_armed_never,
    'cached-rewrite': check_cached_rewrite, 'sf-untouched': check_sf_untouched, 'lock-busy': check_lock_busy,
    'no-sfi': check_no_sfi, 'kill-dll-window': check_kill_dll_window, 'kill-after-record': check_kill_after_record,
}
assert set(CHECKS) == set(CASES)


def check_dll(out):
    """kiln-eml.dll as build.sh built it carries none of mod-loader-lua's test failpoints."""
    source = HERE.parents[3] / 'gpl' / 'kiln-eml' / 'eml' / 'mod-loader-lua' / 'src'
    import re
    names = set()
    for f in source.rglob('*.rs'):
        names |= set(re.findall(r'failpoint!\(\s*"([^"]+)"', f.read_text(errors='replace')))
    data = (out / 'shared' / 'kiln-eml.dll').read_bytes()
    found = sorted(n for n in names if n.encode() in data)
    if not names or found:
        raise SystemExit(f'failpoints in kiln-eml.dll: {found} (of {len(names)})')
    return len(names)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('command', choices=['build', 'prepare', 'wine', 'check', 'payload'])
    parser.add_argument('--out', required=True, type=Path)
    parser.add_argument('--xwin', type=Path, default=Path.home() / '.cache/cargo-xwin/xwin')
    parser.add_argument('--eml', type=Path)
    parser.add_argument('--sfi', type=Path)
    parser.add_argument('--gen', type=Path)
    parser.add_argument('--case', action='append', default=[])
    parser.add_argument('--repeat', type=int, default=1)
    parser.add_argument('--runs', type=Path)
    parser.add_argument('--to', type=Path)
    args = parser.parse_args()
    {'build': build, 'prepare': prepare, 'wine': wine, 'check': check, 'payload': payload}[args.command](args)


if __name__ == '__main__':
    main()
