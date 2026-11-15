#!/usr/bin/env bash
# Builds hooks-typed-test's native part for a lab server: -> $KILN_OUT/hooks-typed-test-native/bin/kiln-hooks-typed-test.dll (the
# path is printed). Copy it to <lab>/kiln/plugins/ before the start (it hooks at boot).
# KILN_OUT picks the build folder; the default is native/build, like native/build.sh.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../../../.." && pwd)
out=${KILN_OUT:-$root/native/build}
[[ $out = /* ]] || out=$root/$out
out=$out/hooks-typed-test-native
cmake -S "$here" -B "$out" -G Ninja -DCMAKE_TOOLCHAIN_FILE="$root/native/cmake/clang-cl-xwin.cmake" \
  -DCMAKE_BUILD_TYPE=Release -Wno-dev >/dev/null
cmake --build "$out" >&2
echo "$out/bin/kiln-hooks-typed-test.dll"
