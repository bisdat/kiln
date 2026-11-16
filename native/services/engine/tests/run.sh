#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# kiln.engine host suite: generator check, C layout, identity and the real DLL entry over a fake core; ASan/UBSan and
# ThreadSanitizer builds (any-thread members). Run with KILN_OUT set.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../../../.." && pwd)
out=${KILN_OUT:?set KILN_OUT}/engine-host-test
mkdir -p "$out"
python3 -I "$root/tools/engine/generate.py" --check
python3 -I "$root/tools/engine/test_generate.py"
cc=${CC:-gcc}
cxx=${CXX:-g++}
"$cc" -std=c11 -Wall -Wextra -Werror -fsyntax-only -I"$root/native/core/abi" "$here/abi_test.c"
inc=(-I"$here/.." -I"$here/../include" -I"$root/native/core/abi")
src=("$here/../identity.cpp")
for kind in asan tsan; do
  if [[ $kind == asan ]]; then san=(-fsanitize=address,undefined -fno-sanitize-recover=undefined); else san=(-fsanitize=thread); fi
  "$cxx" -std=c++20 -O1 -g -Wall -Wextra -Werror -pthread "${san[@]}" "${inc[@]}" "$here/identity_test.cpp" "${src[@]}" \
    -o "$out/identity_test_$kind"
  TSAN_OPTIONS=halt_on_error=1:exitcode=66 "$out/identity_test_$kind"
  "$cxx" -std=c++20 -O1 -g -Wall -Wextra -Werror -pthread "${san[@]}" "${inc[@]}" "$here/plugin_host_test.cpp" \
    "$here/../engine.cpp" "${src[@]}" -o "$out/plugin_host_test_$kind"
  TSAN_OPTIONS=halt_on_error=1:exitcode=66 "$out/plugin_host_test_$kind"
done
