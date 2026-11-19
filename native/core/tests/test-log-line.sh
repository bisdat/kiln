#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Host test of one kiln.log line (log.cpp write_entry). Cuts the pure block between "log-line:begin" and
# "log-line:end" out of native/core/src/log.cpp as it is, builds log_line_test.cpp around it with the host's C++ compiler
# and runs it. Also checks in the source that the counted text reaches the line and the subscribers whole. Exit code
# 0 = every check passed. No Wine and no game are needed.
#   native/core/tests/test-log-line.sh
# Env: KILN_OUT            build dir (default native/build); the test goes to $KILN_OUT/log-line-test
#      KILN_LOG_LINE_SRC   another log.cpp to cut the block from (to see a broken rule fail)
#      CXX                 the host compiler (default c++)
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
src=${KILN_LOG_LINE_SRC:-$here/../src/log.cpp}
out=${KILN_OUT:-$here/../../build}/log-line-test
mkdir -p "$out"
sed -n '/^\/\/ log-line:begin$/,/^\/\/ log-line:end$/p' "$src" >"$out/log_line_block.inc"
[[ -s $out/log_line_block.inc ]] || { echo "no log-line block in $src" >&2; exit 2; }
fail=0
# The way in and out keeps the length: KilnApi.log hands the counted KilnStr on (api.cpp), write() copies it whole into
# the entry, write_entry writes log_line() with fwrite, and a subscriber gets the game's line as a counted KilnStr.
need() { grep -qF -- "$2" "$1" || { echo "FAIL  $(basename "$1") lacks: $2"; fail=1; }; }
need "$here/../src/api.cpp" 'log::write(level, reg::owner_name(owner), sv(text));'
need "$src" 'e.text.assign(text);'
need "$src" 'std::fwrite(line.data(), 1, line.size(), g_file);'
need "$src" 'KilnStr{e.text.data(), e.text.size()}'
if grep -nE 'fprintf\(g_file' "$src"; then echo "FAIL  log.cpp still prints an entry with fprintf"; fail=1; fi
"${CXX:-c++}" -std=c++20 -Wall -Wextra -Werror -I "$out" "$here/log_line_test.cpp" -o "$out/log_line_test"
"$out/log_line_test" || fail=1
exit $fail
