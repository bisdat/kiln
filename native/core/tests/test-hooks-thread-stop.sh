#!/usr/bin/env bash
# Host execution of the actual production stop loop with deterministic OS responses.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
out=${KILN_OUT:-$here/../../build}/core-tests
mkdir -p "$out"
python3 - "$here/../src/services/hooks_chain.cpp" "$out/hooks_stop_loop.g.h" <<'PY'
import pathlib, sys
s = pathlib.Path(sys.argv[1]).read_text()
stopped = s[s.index('struct Stopped {'):s.index('constexpr DWORD kThreadAccess')]
report = s[s.index('struct FreezeReport {'):s.index('// Nothing in here may allocate, log, lock or throw: other threads are stopped.\nvoid write_with_threads_stopped')]
loop = s[s.index('    for (Stopped& s : th) {', s.index('void write_with_threads_stopped')):s.index('    read_stopped_positions(th, rep, query);', s.index('void write_with_threads_stopped'))]
pathlib.Path(sys.argv[2]).write_text(stopped + report + '\nvoid stop_loop(std::vector<Stopped>& th, FreezeReport& rep, uint32_t* unchecked) {\n' + loop + '}\n')
PY
cxx=${CXX:-$(command -v clang++ || command -v g++)}
"$cxx" -std=c++20 -O1 -g -Wall -Wextra -fsanitize=address,undefined -fno-sanitize-recover=undefined \
    -I"$out" "$here/hooks_thread_stop_test.cpp" -o "$out/hooks_thread_stop_test"
"$out/hooks_thread_stop_test"
