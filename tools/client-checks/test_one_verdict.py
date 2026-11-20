# SPDX-License-Identifier: MIT
"""The client pass's checks ask for the player's verdict once.

In Kiln.Testing the player's first answer is the check's verdict (CheckContext.Verdict: "a later call returns it at
once"), and an answer given before the body asks ends the check. A check that asks for a verdict to mean "continue" and
then goes on would fall through every later step. So each method of mods/tests/client-pass calls
`await c.Verdict();` at most once on its way, apart from early exits (`await c.Verdict(); return;` or
`return false;`), and no
instruction asks the player to press continue.

    python3 -m unittest discover -s tools/client-checks
"""
from __future__ import annotations

import re
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
PACK = HERE.parent.parent / "mods" / "tests" / "client-pass"

METHOD = re.compile(r"\b(?:public|private|internal)\s+(?:static\s+)?async\s+Task(?:<[^>]*>)?\s+(\w+)\s*\([^)]*\)\s*\{")
VERDICT = re.compile(r"await\s+c\.Verdict\(\)\s*;")
EXIT_AFTER = re.compile(r"\s*return\b[^;]*;")


def _strip(source: str) -> str:
    """Comments and string literals blanked (same length), so braces and calls inside them do not count."""
    out = []
    i, n = 0, len(source)
    while i < n:
        ch = source[i]
        if source.startswith("//", i):
            j = source.find("\n", i)
            j = n if j < 0 else j
            out.append(" " * (j - i))
            i = j
        elif source.startswith("/*", i):
            j = source.find("*/", i + 2)
            j = n if j < 0 else j + 2
            out.append(re.sub(r"[^\n]", " ", source[i:j]))
            i = j
        elif ch == '"' or (ch in "$@" and i + 1 < n and source[i + 1] in '"$@'):
            j = i
            while j < n and source[j] in "$@":
                j += 1
            verbatim = "@" in source[i:j]
            j += 1  # the opening quote
            while j < n:
                if source[j] == "\\" and not verbatim:
                    j += 2
                    continue
                if source[j] == '"':
                    if verbatim and j + 1 < n and source[j + 1] == '"':
                        j += 2
                        continue
                    j += 1
                    break
                j += 1
            out.append(re.sub(r"[^\n]", " ", source[i:j]))
            i = j
        else:
            out.append(ch)
            i += 1
    return "".join(out)


def method_bodies(source: str):
    code = _strip(source)
    for m in METHOD.finditer(code):
        depth, j = 1, m.end()
        while j < len(code) and depth:
            depth += {"{": 1, "}": -1}.get(code[j], 0)
            j += 1
        yield m.group(1), code[m.end():j - 1]


def asking_verdicts(body: str) -> int:
    """Verdict calls that do not end the method right there."""
    return sum(1 for v in VERDICT.finditer(body) if not EXIT_AFTER.match(body, v.end()))


class OneVerdict(unittest.TestCase):
    def test_the_counter_sees_steps_and_spares_early_exits(self):
        steps = 'public async Task A(CheckContext c) { c.Instruct("x { y"); await c.Verdict(); Step(); await c.Verdict(); }'
        exits = "public async Task B(CheckContext c) { if (x) { await c.Verdict(); return; } await c.Verdict(); Take(); }"
        got = dict((name, asking_verdicts(body)) for name, body in method_bodies(steps + exits))
        self.assertEqual(got, {"A": 2, "B": 1})

    def test_every_check_asks_for_one_verdict(self):
        many = []
        for path in sorted(PACK.glob("*.cs")):
            for name, body in method_bodies(path.read_text(encoding="utf-8")):
                if asking_verdicts(body) > 1:
                    many.append(f"{path.name}: {name}")
        self.assertEqual(many, [], "methods that ask for a verdict more than once (the first answer is the verdict)")

    def test_no_instruction_asks_to_press_continue(self):
        hits = [f"{p.name}:{i}" for p in sorted(PACK.glob("*.cs"))
                for i, line in enumerate(p.read_text(encoding="utf-8").splitlines(), 1) if re.search(r"(?i)\bpress continue\b", line.split("//")[0])]
        self.assertEqual(hits, [], "there is no continue answer: a step goes on by what the check observes")


if __name__ == "__main__":
    unittest.main()
