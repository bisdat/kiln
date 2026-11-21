#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# The release hold of Kiln's interpreter for ShroudForge mods on the host (no Windows, no game): src/sfi_release_policy.cpp
# and src/gates.cpp as they are, against the compiled list as the core's build generates it (native/core/gates, gen.py).
#   source checks        the release policy reads nothing at run time (no environment, file, config or kiln.json), the
#                        record of a final ShroudForge 1.8.x is the one constant false and release_allows_180() returns
#                        it, and the compiled list holds kiln-sfi.sf18 on both sides, kiln-sfi.interpreter on both sides
#                        (a preview) or not at all (a normal choice), and nothing else
#   sfi_release_policy   built as every release and preview build is, with ASan+UBSan and with TSan
#   native/core/tests/test-sfi-release-policy.sh
# Environment: KILN_OUT (output folder, default native/build), KILN_TEST_REPEAT (runs of each binary, default 1),
# KILN_SFI_RELEASE_POLICY_SRC / KILN_GATES_SRC: another sfi_release_policy.cpp / gates.cpp (to see a broken rule fail),
# KILN_GATES_DIR: another gate list folder.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
src=$here/../src
out=${KILN_OUT:-$here/../../build}/sfi-release-policy-test
mkdir -p "$out"
policy_src=${KILN_SFI_RELEASE_POLICY_SRC:-$src/sfi_release_policy.cpp}
gates_src=${KILN_GATES_SRC:-$src/gates.cpp}
gates_dir=${KILN_GATES_DIR:-$here/../gates}
repeat=${KILN_TEST_REPEAT:-1}
cxx=${CXX:-clang++}

python3 -I "$here/../gates/gen.py" "$gates_dir" "$out/kiln_gates.g.h"
python3 -I - "$policy_src" "$gates_dir" "$here/../gates/gen.py" <<'PY'
import pathlib, re, subprocess, sys
src = pathlib.Path(sys.argv[1]).read_text()
code = re.sub(r'//[^\n]*', '', src)
release = code  # what a release or preview build compiles
fails, checked = [], []
def need(ok, what):
    checked.append(what)
    if not ok: fails.append(what)
need(re.findall(r'kUpstream180Final\s*=[^;]*;', release) == ['kUpstream180Final = false;'],
     'the record of a final ShroudForge 1.8.x must be the one constant kUpstream180Final = false')
need(re.findall(r'bool release_allows_180\(\)[^}]*\}', release) == ['bool release_allows_180() { return kUpstream180Final; }'],
     'release_allows_180() must return kUpstream180Final and nothing else, defined once')
for word in ('getenv', 'GetEnvironmentVariable', '_wgetenv', 'environ', 'config()', 'read_file', 'fopen', 'ifstream',
             'kiln.json', 'CreateFile', 'RegOpenKey', 'RegQueryValue', 'RegGetValue'):
    need(word not in code, f'the release policy must read nothing at run time ({word})')
rows = subprocess.run([sys.executable, '-I', sys.argv[3], '--list', sys.argv[2]], check=True, capture_output=True,
                      text=True).stdout.splitlines()
names = [r.split('\t')[0] for r in rows]
sides = [r.split('\t')[1] for r in rows]
need((names, sides) in ((['kiln-sfi.interpreter', 'kiln-sfi.sf18'], ['both', 'both']), (['kiln-sfi.sf18'], ['both'])),
     'the compiled list must hold kiln-sfi.sf18 on both sides, kiln-sfi.interpreter on both sides (a preview) or not at '
     f'all (a normal choice), and nothing else (got {rows})')
for f in fails: print('FAIL sfi_release_policy source: ' + f, file=sys.stderr)
print(f'sfi_release_policy source: {len(checked)} checks, {len(fails)} failures')
sys.exit(1 if fails else 0)
PY

common=(-std=c++20 -O1 -g -Wall -Wextra -Werror -pthread -I"$here/stubs" -I"$src" -I"$here/../abi" -I"$here/../../third_party" -I"$out")
asan=(-fsanitize=address,undefined -fno-sanitize-recover=undefined)
sources=("$here/sfi_release_policy_test.cpp" "$gates_src" "$policy_src")
"$cxx" "${common[@]}" "${asan[@]}" "${sources[@]}" -o "$out/release"
"$cxx" "${common[@]}" -fsanitize=thread "${sources[@]}" -o "$out/release_tsan"

for ((i = 1; i <= repeat; i++)); do
  "$out/release"
  TSAN_OPTIONS=halt_on_error=1 "$out/release_tsan"
done
