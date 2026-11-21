#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""The gate path's sources must not reach a wait, a lock, an allocation, a log or the KilnApi (the GameServer thread spins unbounded behind a held Scene word, so any of these inside the hold can freeze the server).
Checks the code (comments and string literals stripped) of every file a gate member runs: gate, resolve and the
Windows memory bodies, the content worker's lifetime guard, which runs inside a gate's hold when it borrows
the gate's share and blocks thread registration while it owns one, and the engine call inside the hold. The
guard's and the engine call's files must also have no loop without a fixed bound (`while`, `for (;;)`): their loops
run over the registry rows (bounded by the profile's capacity) and over a recipe's ranges and arguments.
Run by tests/run.sh."""
import re
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
FILES = ['gate.cpp', 'gate.h', 'resolve.cpp', 'resolve.h', 'memory.h', 'memory_win.cpp', 'resource_guard.cpp',
         'resource_guard.h', 'invoke.cpp', 'invoke.h', 'invoke_win.cpp']
# Only for the guard and the engine call (the gate's exit/stop closing loops are time-bounded by design).
BOUNDED = {'resource_guard.cpp', 'resource_guard.h', 'invoke.cpp', 'invoke.h', 'invoke_win.cpp'}
UNBOUNDED = r'\bwhile\s*\((?!\s*0\s*\))|\bfor\s*\(\s*;\s*;'

FORBIDDEN = {
    r'\bSleep(Ex)?\b|\bSwitchToThread\b|\bYieldProcessor\b|\bWaitFor\w*\b|\bWaitOnAddress\b|\bthis_thread\b': 'a wait',
    r'\bEnterCriticalSection\b|\bAcquireSRWLock\w*\b|\bstd::(recursive_|timed_|shared_)?mutex\b|\block_guard\b|'
    r'\bscoped_lock\b|\bunique_lock\b|\bshared_lock\b|\bcondition_variable\b|\bpthread_\w+': 'a lock',
    r'\bnew\b|\bmalloc\b|\bcalloc\b|\brealloc\b|\bstd::(string|vector|map|unordered_map|function|deque|list)\b|'
    r'\bmake_unique\b|\bmake_shared\b|\bHeapAlloc\b|\bVirtualAlloc\b': 'an allocation',
    r'\bKLOG_\w+\b|\bprintf\b|\bfprintf\b|\bsnprintf\b|\bOutputDebugString\w*\b|->\s*log\s*\(': 'a log',
    r'\bKilnApi\b|\bapi\s*->': 'a KilnApi call',
    r'\bthrow\b': 'an exception',
}


def code_of(text):
    text = re.sub(r'/\*.*?\*/', ' ', text, flags=re.S)
    text = re.sub(r'//[^\n]*', ' ', text)
    text = re.sub(r'"(\\.|[^"\\])*"', '""', text)
    return re.sub(r"'(\\.|[^'\\])*'", "''", text)


def main():
    problems = []
    for name in FILES:
        code = code_of((HERE.parent / name).read_text(encoding='utf-8'))
        for number, line in enumerate(code.splitlines(), 1):
            for pattern, what in FORBIDDEN.items():
                if re.search(pattern, line):
                    problems.append('%s:%d: %s: %s' % (name, number, what, line.strip()))
            if name in BOUNDED and re.search(UNBOUNDED, line):
                problems.append('%s:%d: an unbounded loop: %s' % (name, number, line.strip()))
    for problem in problems:
        print(problem, file=sys.stderr)
    print('gate path lint: %d files, %d problems' % (len(FILES), len(problems)))
    return 1 if problems else 0


if __name__ == '__main__':
    sys.exit(main())
