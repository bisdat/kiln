#!/usr/bin/env bash
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
out=${KILN_OUT:?}/core-tests
mkdir -p "$out"
python3 -I - "$here/../src/services/hooks.cpp" "$out/hooks_status.g.h" <<'EXTRACT'
import pathlib, sys
s = pathlib.Path(sys.argv[1]).read_text()
pathlib.Path(sys.argv[2]).write_text(s[s.index('KilnResult KILN_CALL h_sig_status('):s.index('KilnResult KILN_CALL h_validate_typed(')])
EXTRACT
"${CXX:-clang++}" -std=c++20 -O1 -g -Wall -Wextra -I"$out" "$here/hooks_status_test.cpp" -o "$out/hooks_status_test"
"$out/hooks_status_test"
