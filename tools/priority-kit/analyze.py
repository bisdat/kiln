#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""The priority kit's analysis (docs/kiln/priority-kit.md): turns the zips collect.ps1 and collect.sh made into the numbers
GitHub #58 asks for, per platform (native Windows, Proton), priority raise (on, off) and CPU load (idle, loaded):

  1. the hold time of every call that took the engine's gate (the test mod's writes.csv): a histogram and percentiles;
     and, from kit 3 on a Kiln that times it apart, the priority restore after each release (restore_ns);
  2. the server step time: the test mod's own Pre-to-Pre intervals and Pre-to-Post times (steps.csv), and the minute
     windows of Kiln's tick-stats.jsonl that lie inside one load phase;
  3. the outcomes (ok, busy, conflict, ...).

    python3 analyze.py kiln-priority-kit-*.zip [--json] [--margin 2]

A row's load phase comes from the CPU-load helper's phase file in the same zip (cpu-load-*.csv, UTC times); without one,
everything counts as idle. --margin drops the seconds around each phase change (default 2). Markdown goes to stdout,
ready to paste into the issue; --json prints the same numbers as one JSON document.

Runs of an older kit (run.json "kit", else the CSV header) are kept apart: kit 1 retried a busy answer every millisecond
and locked onto the start of each update (GitHub #58), so its outcomes and step times are not comparable with kit 2's
and 3's (kit 3 is kit 2's loop with the restore columns).

Where a run's Kiln ended a hold is kept apart too (run.json "hold_ends", else the header's "hold="): kit 3 on a Kiln that
ends it at the Scene word's release says "scene-release" and has the restore apart; every other run (kit 1 and 2, and
kit 3 on an older Kiln such as 0.2.0-dev.30.1, "release-end") timed each hold to the end of the gate's release, the
priority restore included, and is labelled "hold includes the release and restore" (GitHub #58)."""
import argparse
import csv
import io
import json
import math
import re
import sys
import zipfile
from datetime import datetime, timedelta, timezone
from pathlib import Path

BUCKETS_US = [10, 20, 50, 100, 200, 500, 1000, 2000, 5000, 10000, 20000]
KIT = '3'  # the test mod's kit version this analysis is for (kit.h kKitVersion)
SCENE = 'scene-release'  # a hold that ends at the Scene word's release, the restore timed apart (kit 3 on a Kiln with it)
RELEASE = 'release-end'  # a hold timed to the end of the gate's release, the priority restore included (older runs)
OLD_HOLD = 'hold includes the release and restore'


def bucket_names():
    names, lo = [], 0
    for hi in BUCKETS_US:
        names.append(label(lo) + '-' + label(hi))
        lo = hi
    names.append('>=' + label(lo))
    return names


def label(us):
    return '%g ms' % (us / 1000) if us >= 1000 else '%g us' % us


def percentile(values, p):
    """Nearest-rank percentile of sorted values (0 when empty)."""
    if not values:
        return 0
    rank = max(1, math.ceil(p * len(values) / 100 - 1e-9))  # the epsilon: 99.9 * 1000 / 100 is not quite 999
    return values[min(rank, len(values)) - 1]


class Source:
    """One zip or folder from a collection script."""

    def __init__(self, path):
        self.path = Path(path)
        self._zip = zipfile.ZipFile(self.path) if self.path.is_file() else None
        # Windows PowerShell 5.1's Compress-Archive writes entry names with backslashes: use '/' throughout
        if self._zip:
            self._entries = {n.replace('\\', '/'): n for n in self._zip.namelist() if not n.endswith(('/', '\\'))}
        else:
            self._entries = {str(p.relative_to(self.path)).replace('\\', '/'): p for p in self.path.rglob('*') if p.is_file()}

    def names(self):
        return list(self._entries)

    def text(self, name):
        entry = self._entries.get(name)
        if entry is None:
            return None
        try:
            if self._zip:
                return self._zip.read(entry).decode('utf-8', errors='replace')
            return Path(entry).read_text(encoding='utf-8', errors='replace')
        except (KeyError, OSError):
            return None


def rows(text):
    """CSV rows as dicts; '#' lines (the header comment) are skipped and returned apart."""
    if not text:
        return [], ''
    comment = ''
    body = []
    for line in text.splitlines():
        if line.startswith('#'):
            comment = line
        elif line.strip():
            body.append(line)
    return list(csv.DictReader(io.StringIO('\n'.join(body)))), comment


class Phases:
    def __init__(self, phases, margin_ms):
        self.phases = sorted(phases, key=lambda p: p[1])
        self.margin = margin_ms

    def of(self, t_ms):
        """'loaded', 'idle', or None inside a margin around a change."""
        for name, start, end in self.phases:
            if start <= t_ms <= end:
                if t_ms - start < self.margin or end - t_ms < self.margin:
                    return None
                return 'loaded' if name == 'load' else 'idle'
        for name, start, end in self.phases:  # within a margin outside a phase
            if start - self.margin <= t_ms <= end + self.margin:
                return None
        return 'idle'

    def window(self, start_ms, end_ms):
        """The phase a whole window lies in (90% of it), else None."""
        for name, start, end in self.phases:
            overlap = min(end, end_ms) - max(start, start_ms)
            if overlap >= 0.9 * (end_ms - start_ms):
                return 'loaded' if name == 'load' else 'idle'
        if not any(min(end, end_ms) > max(start, start_ms) for _, start, end in self.phases):
            return 'idle'
        return None


def kit_of(run, header):
    """The kit version of a run: run.json's "kit", else the CSV header's ('# gate-priority-test 1; ...'), else '1'."""
    if run.get('kit'):
        return str(run['kit'])
    m = re.match(r'# gate-priority-test (\S+);', header or '')
    return m.group(1) if m else '1'


def hold_of(run, header):
    """Where the run's Kiln ended a hold: run.json's "hold_ends", else the CSV header's '; hold=...', else (kit 1 and 2,
    which never said) the end of the gate's release. Anything but SCENE counts as RELEASE."""
    hold = run.get('hold_ends')
    if not hold:
        m = re.search(r'; hold=([^;\s]+)', header or '')
        hold = m.group(1) if m else RELEASE
    return SCENE if hold == SCENE else RELEASE


def platform_of(system):
    if not system:
        return 'unknown'
    if system.get('platform') == 'linux':
        return 'proton' if system.get('proton') is not None else 'linux'
    return 'wine' if system.get('wine') else 'windows'


def analyze(paths, margin_s=2.0):
    groups = {}   # (platform, kit, hold, raise, load) -> {"holds": [], "restores": [], "status": {}, "intervals": [], ...}
    runs = []
    notes = []

    def group(platform, kit, hold, raise_, load):
        return groups.setdefault((platform, kit, hold, raise_, load), {'holds': [], 'restores': [], 'status': {},
                                                                        'intervals': [], 'pre_post': [], 'windows': []})

    for path in paths:
        src = Source(path)
        names = src.names()
        system = json.loads(src.text('system.json') or '{}')
        platform = platform_of(system)
        phases = []
        for n in names:
            if n.startswith('load/') and n.endswith('.csv'):
                for r in rows(src.text(n))[0]:
                    try:
                        phases.append((r['phase'], int(r['start_utc_ms']), int(r['end_utc_ms'])))
                    except (KeyError, ValueError):
                        pass
        if not phases:
            notes.append('%s: no CPU-load phase file; everything counts as idle' % src.path.name)
        ph = Phases(phases, margin_s * 1000)
        run_dirs = sorted({n.split('/')[1] for n in names if n.startswith('runs/') and n.count('/') >= 2})
        offset = timedelta(minutes=system.get('utc_offset_minutes', 0) or 0)
        for rd in run_dirs:
            run = json.loads(src.text('runs/%s/run.json' % rd) or '{}')
            raise_ = run.get('raise', 'unknown')
            writes, header = rows(src.text('runs/%s/writes.csv' % rd))
            if header and ('raise=%s' % raise_) not in header:
                notes.append('%s/%s: writes.csv says %s but run.json says raise=%s; skipped' % (src.path.name, rd, header, raise_))
                continue
            kit = kit_of(run, header)
            hold = hold_of(run, header)
            runs.append({'source': src.path.name, 'run': rd, 'platform': platform, 'kit': kit, 'hold': hold,
                         'kiln': run.get('kiln'), 'os': system.get('os'),
                         'cpu': system.get('cpu'), 'logical_processors': system.get('logical_processors'),
                         'raise': raise_, 'mode': run.get('mode'), 'component': run.get('component'),
                         'pairs': run.get('pairs'), 'restored': run.get('restored'), 'verified': run.get('verified'),
                         'end': run.get('end'), 'label': run.get('label'), 'profile': run.get('profile')})
            for w in writes:
                try:
                    t = int(w['utc_ms'])
                except (KeyError, ValueError):
                    continue
                load = ph.of(t)
                if load is None:
                    continue
                g = group(platform, kit, hold, raise_, load)
                g['status'][w['status']] = g['status'].get(w['status'], 0) + 1
                if w['status'] == 'ok':
                    g['holds'].append(int(w['hold_ns']))
                    restore = w.get('restore_ns') or ''
                    if hold == SCENE and restore.isdigit():  # kit 3 leaves it empty where the Kiln does not time it
                        g['restores'].append(int(restore))
            steps = rows(src.text('runs/%s/steps.csv' % rd))[0]
            prev = None
            for s in steps:
                try:
                    step, t, pre, post = int(s['step']), int(s['utc_ms']), int(s['pre_ns']), int(s['post_ns'])
                except (KeyError, ValueError):
                    continue
                load = ph.of(t)
                if load is not None:
                    g = group(platform, kit, hold, raise_, load)
                    if prev and step == prev[0] + 1 and pre > prev[1]:
                        g['intervals'].append((pre - prev[1]) / 1e6)
                    if post > pre:
                        g['pre_post'].append((post - pre) / 1e6)
                prev = (step, pre)
            start = run.get('writes_from_utc_ms') or run.get('started_utc_ms') or 0
            end = run.get('ended_utc_ms') or 2 ** 62
            for line in (src.text('logs/tick-stats.jsonl') or '').splitlines():
                try:
                    d = json.loads(line)
                    if d.get('world', 'server') != 'server':
                        continue
                    local = datetime.strptime(d['time'], '%Y-%m-%dT%H:%M:%S')
                    w_end = int(((local - offset).replace(tzinfo=timezone.utc)).timestamp() * 1000)
                except (ValueError, KeyError, TypeError):
                    continue
                w_start = w_end - 60_000
                if w_start < start or w_end > end:
                    continue
                load = ph.window(w_start, w_end)
                if load is None:
                    continue
                group(platform, kit, hold, raise_, load)['windows'].append({k: d.get(k, {}) for k in ('step_ms', 'interval_ms', 'pre_ms', 'post_ms')} |
                                                                {'steps': d.get('steps', 0)})
    old = sorted({'%s (kit %s)' % (r['source'], r['kit']) for r in runs if r['kit'] != KIT})
    if old:
        text = 'runs of another kit than %s, shown apart (Platform "<platform> kit N"): %s.' % (KIT, ', '.join(old))
        if any(r['kit'] == '1' for r in runs):
            text += (' Kit 1 retried a busy answer every millisecond and locked onto the start of each update (GitHub #58): '
                     'its outcomes, busy share and step times are not comparable with kit 2\'s and 3\'s.')
        if any(r['kit'] == '2' for r in runs):
            text += ' Kit 2 has kit 3\'s loop without the restore columns: its outcomes and step times compare with kit 3\'s.'
        notes.append(text)
    timed_to_release = sorted({'%s (kit %s)' % (r['source'], r['kit']) for r in runs if r['hold'] != SCENE})
    if timed_to_release:
        notes.append('"%s" (Platform): %s. These runs timed each hold to the end of the gate\'s release: after the Scene '
                     'word, the registry word, the holder flag and the priority restore (SetThreadPriority; under Proton a '
                     'wineserver round trip), and any preemption of the thread right after it dropped back to its own '
                     'priority. The Scene word was free for all of that. Kit 3 on a Kiln that ends a hold at the Scene '
                     'word\'s release shows the restore apart (GitHub #58): compare its hold plus restore with these holds, '
                     'not its hold alone.' % (OLD_HOLD, ', '.join(timed_to_release)))
    return summarize(groups, runs, notes)


def spread_us(values_ns):
    """Percentiles of nanosecond values, in µs."""
    us = sorted(v / 1000 for v in values_ns)
    return {'n': len(us), 'p50': percentile(us, 50), 'p90': percentile(us, 90), 'p99': percentile(us, 99),
            'p99.9': percentile(us, 99.9), 'max': us[-1] if us else 0, 'over_1ms': sum(1 for v in us if v >= 1000)}


def summarize(groups, runs, notes):
    out = {'runs': runs, 'notes': notes, 'groups': []}
    for (platform, kit, hold, raise_, load), g in sorted(groups.items()):
        holds = sorted(g['holds'])
        us = [h / 1000 for h in holds]
        hist = [0] * (len(BUCKETS_US) + 1)
        for v in us:
            i = 0
            while i < len(BUCKETS_US) and v >= BUCKETS_US[i]:
                i += 1
            hist[i] += 1
        calls = sum(g['status'].values())
        iv = sorted(g['intervals'])
        pp = sorted(g['pre_post'])
        win = g['windows']
        out['groups'].append({
            'platform': platform, 'kit': kit, 'hold': hold, 'raise': raise_, 'load': load,
            'hold_us': spread_us(holds),
            # the priority restore after each of those releases, apart from the hold (None: not timed apart)
            'restore_us': spread_us(g['restores']) if g['restores'] else None,
            'histogram': dict(zip(bucket_names(), hist)),
            'outcomes': dict(sorted(g['status'].items())), 'calls': calls,
            'busy_share': (g['status'].get('busy', 0) / calls) if calls else 0,
            'interval_ms': {'n': len(iv), 'p50': percentile(iv, 50), 'p99': percentile(iv, 99), 'max': iv[-1] if iv else 0},
            'pre_to_post_ms': {'n': len(pp), 'p50': percentile(pp, 50), 'p99': percentile(pp, 99), 'max': pp[-1] if pp else 0},
            'tick_stats': {
                'windows': len(win),
                'step_p50_ms': percentile(sorted(w['step_ms'].get('p50', 0) for w in win), 50),
                'step_p99_ms': max((w['step_ms'].get('p99', 0) for w in win), default=0),
                'step_max_ms': max((w['step_ms'].get('max', 0) for w in win), default=0),
                'interval_p99_ms': max((w['interval_ms'].get('p99', 0) for w in win), default=0),
                'interval_max_ms': max((w['interval_ms'].get('max', 0) for w in win), default=0),
            },
        })
    # late steps: Pre-to-Pre over 1.5 times the idle median of the same platform, kit, hold and raise
    idle = {(g['platform'], g['kit'], g['hold'], g['raise']): g['interval_ms']['p50'] for g in out['groups'] if g['load'] == 'idle'}
    for g in out['groups']:
        base = idle.get((g['platform'], g['kit'], g['hold'], g['raise']))
        src = groups[(g['platform'], g['kit'], g['hold'], g['raise'], g['load'])]['intervals']
        g['late_steps'] = sum(1 for v in src if base and v > 1.5 * base)
    return out


def where(g):
    """The Platform cell: the platform, the kit when it is not this analysis' kit, and a hold timed to the end of the
    gate's release (the priority restore included) said in words."""
    cell = g['platform'] if g.get('kit', KIT) == KIT else '%s kit %s' % (g['platform'], g['kit'])
    return cell if g.get('hold', SCENE) == SCENE else '%s (%s)' % (cell, OLD_HOLD)


def fmt(v, digits=1):
    return ('%.' + str(digits) + 'f') % v if isinstance(v, float) else str(v)


def markdown(r):
    lines = []
    lines.append('### Hold time of the engine gate (µs, every call that took it)')
    lines.append('')
    lines.append('| Platform | Raise | Load | Calls held | p50 | p90 | p99 | p99.9 | Max | ≥ 1 ms | Busy share |')
    lines.append('|---|---|---|---:|---:|---:|---:|---:|---:|---:|---:|')
    for g in r['groups']:
        h = g['hold_us']
        lines.append('| %s | %s | %s | %d | %s | %s | %s | %s | %s | %d | %.1f%% |' % (
            where(g), g['raise'], g['load'], h['n'], fmt(h['p50']), fmt(h['p90']), fmt(h['p99']), fmt(h['p99.9']),
            fmt(h['max']), h['over_1ms'], 100 * g['busy_share']))
    restored = [g for g in r['groups'] if g.get('restore_us')]
    if restored:
        lines.append('')
        lines.append('### Priority restore after the gate\'s release (µs, the same calls, apart from the hold)')
        lines.append('')
        lines.append('| Platform | Raise | Load | Calls | p50 | p90 | p99 | p99.9 | Max | ≥ 1 ms |')
        lines.append('|---|---|---|---:|---:|---:|---:|---:|---:|---:|')
        for g in restored:
            h = g['restore_us']
            lines.append('| %s | %s | %s | %d | %s | %s | %s | %s | %s | %d |' % (
                where(g), g['raise'], g['load'], h['n'], fmt(h['p50']), fmt(h['p90']), fmt(h['p99']), fmt(h['p99.9']),
                fmt(h['max']), h['over_1ms']))
    lines.append('')
    lines.append('### Hold-time histogram (calls per bucket)')
    lines.append('')
    heads = ['%s/%s/%s' % (where(g), g['raise'], g['load']) for g in r['groups']]
    lines.append('| Bucket | ' + ' | '.join(heads) + ' |')
    lines.append('|---|' + '---:|' * len(heads))
    for b in bucket_names():
        lines.append('| %s | ' % b + ' | '.join(str(g['histogram'][b]) for g in r['groups']) + ' |')
    lines.append('')
    lines.append('### Server step (ms): the test mod\'s own rows')
    lines.append('')
    lines.append('| Platform | Raise | Load | Steps | Pre to Pre p50 | p99 | Max | Late (> 1.5 × idle p50) | Pre to Post p50 | p99 | Max |')
    lines.append('|---|---|---|---:|---:|---:|---:|---:|---:|---:|---:|')
    for g in r['groups']:
        i, p = g['interval_ms'], g['pre_to_post_ms']
        lines.append('| %s | %s | %s | %d | %s | %s | %s | %d | %s | %s | %s |' % (
            where(g), g['raise'], g['load'], i['n'], fmt(i['p50'], 2), fmt(i['p99'], 2), fmt(i['max'], 2),
            g['late_steps'], fmt(p['p50'], 2), fmt(p['p99'], 2), fmt(p['max'], 2)))
    lines.append('')
    lines.append('### Server step (ms): tick-stats.jsonl minute windows inside one phase')
    lines.append('')
    lines.append('| Platform | Raise | Load | Windows | Step p50 | Step p99 | Step max | Interval p99 | Interval max |')
    lines.append('|---|---|---|---:|---:|---:|---:|---:|---:|')
    for g in r['groups']:
        t = g['tick_stats']
        lines.append('| %s | %s | %s | %d | %s | %s | %s | %s | %s |' % (
            where(g), g['raise'], g['load'], t['windows'], fmt(float(t['step_p50_ms']), 2), fmt(float(t['step_p99_ms']), 2),
            fmt(float(t['step_max_ms']), 2), fmt(float(t['interval_p99_ms']), 2), fmt(float(t['interval_max_ms']), 2)))
    lines.append('')
    lines.append('### Outcomes')
    lines.append('')
    lines.append('| Platform | Raise | Load | Calls | Outcomes |')
    lines.append('|---|---|---|---:|---|')
    for g in r['groups']:
        lines.append('| %s | %s | %s | %d | %s |' % (where(g), g['raise'], g['load'], g['calls'],
                                                   ', '.join('%s %d' % kv for kv in g['outcomes'].items())))
    lines.append('')
    lines.append('### Runs')
    lines.append('')
    lines.append('| Zip | Run | Platform | Kit | Kiln | Hold ends | OS | CPU (logical) | Raise | Mode | Component | Pairs | Restored | End |')
    lines.append('|---|---|---|---|---|---|---|---|---|---|---|---:|---|---|')
    for x in r['runs']:
        lines.append('| %s | %s | %s | %s | %s | %s | %s | %s (%s) | %s | %s | %s | %s | %s | %s |' % (
            x['source'], x['run'], x['platform'], x['kit'], x.get('kiln') or '?',
            'at the Scene word\'s release' if x.get('hold') == SCENE else 'after the release and restore', x['os'], x['cpu'],
            x['logical_processors'], x['raise'], x['mode'], x['component'], x['pairs'],
            'yes' if x['restored'] and x['verified'] else 'no', x['end']))
    for n in r['notes']:
        lines.append('')
        lines.append('Note: ' + n)
    return '\n'.join(lines) + '\n'


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('zips', nargs='+', help='zips (or unpacked folders) from collect.ps1 / collect.sh')
    ap.add_argument('--json', action='store_true', help='print JSON instead of Markdown')
    ap.add_argument('--margin', type=float, default=2.0, help='seconds dropped around each load phase change')
    args = ap.parse_args(argv)
    result = analyze(args.zips, args.margin)
    sys.stdout.write(json.dumps(result, indent=2) + '\n' if args.json else markdown(result))
    return 0


if __name__ == '__main__':
    sys.exit(main())
