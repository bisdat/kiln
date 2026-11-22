#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# kiln.repl host tests under plain Wine, no game: the ABI layout of kiln_repl.h and the fault rule every observer
# detour follows (guard.h), with real access violations; the lab harness's gate (policy "any" and kiln.net's lab port)
# on the service's own start. The game's behaviour is the lab test mods/tests/repl-test.
#   KILN_OUT=<build dir> native/services/repl/tests/run.sh
#   KILN_OUT=<build dir> native/services/repl/tests/run.sh msg  # message suite only
#   KILN_OUT=<build dir> native/services/repl/tests/run.sh msg-linux  # message suite, skips the SEH fault case
# Exit code 0 = every check passed.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
export KILN_OUT=${KILN_OUT:-$here/../../../build}
if [[ ${1:-} == msg-linux ]]; then
    host=$KILN_OUT/repl-msg-linux
    mkdir -p "$host"
    cat >"$host/windows.h" <<'HEADER'
#pragma once
#define __try try
#define __except(...) catch (...)
#define EXCEPTION_EXECUTE_HANDLER 1
struct EXCEPTION_POINTERS { struct { void* ExceptionAddress; } *ExceptionRecord; };
HEADER
    "${CXX:-c++}" -std=c++20 -O2 -I"$host" -I"$here/../../../core/abi" \
        "$here/repl_msg_test.cpp" "$here/../messages.cpp" -o "$host/repl-msg-test"
    exec "$host/repl-msg-test"
fi
suites=(host gate net track watch client msg rxtrack)
if [[ ${1:-} == msg ]]; then suites=(msg); fi
targets=()
for suite in "${suites[@]}"; do targets+=("repl-$suite-test"); done
"$here/../../../build.sh" "${targets[@]}" >/dev/null
bin=$KILN_OUT/cmake/bin/repl-tests
export WINEPREFIX=$KILN_OUT/repl-tests-pfx WINEDEBUG=-all WINEDLLOVERRIDES="mscoree=;mshtml=;winedbg.exe=d"
# no window may reach the desktop: an unset WAYLAND_DISPLAY means the desktop's wayland-0
unset DISPLAY; export WAYLAND_DISPLAY=kiln-no-display
mkdir -p "$WINEPREFIX"
[[ -f $WINEPREFIX/system.reg ]] || wine wineboot -i >/dev/null 2>&1
fail=0
for target in "${targets[@]}"; do wine "$bin/$target.exe" || fail=1; done
exit $fail
