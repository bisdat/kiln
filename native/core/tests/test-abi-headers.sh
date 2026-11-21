#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Compile-only: kiln_world.h and kiln_abilities.h together in both orders (C11 and C++20), and each alone (C++):
# each header's program-info struct has its own name (KilnWorldProgramInfo, KilnImpactProgramInfo), KilnProgramInfo stays
# an alias of the first one's. Also the layouts of kiln_tasks.h, tasks_host.h, kiln_launch.h, launch_internal.h,
# kiln_hooks.h, kiln_client.h, kiln_eml_startup.h, kiln_net.h and kiln_build.h, and the Kiln-internal hooks_protect_internal.h. Output: $KILN_OUT/core-tests (KILN_OUT
# default native/build).
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
out=${KILN_OUT:-$here/../../build}/core-tests
mkdir -p "$out"
cc=${CC:-$(command -v clang || command -v gcc)}
cxx=${CXX:-$(command -v clang++ || command -v g++)}
inc=(-I"$here/../abi")
n=0
for f in world_first.c abilities_first.c; do
    "$cc" -std=c11 -Wall -Wextra -Werror -fsyntax-only "${inc[@]}" "$here/abi_headers/$f"; n=$((n + 1))
done
for d in "" -DKILN_TEST_ABILITIES_FIRST -DKILN_TEST_ABILITIES_ONLY -DKILN_TEST_WORLD_ONLY; do
    "$cxx" -std=c++20 -Wall -Wextra -Werror -fsyntax-only $d "${inc[@]}" "$here/abi_headers/both.cpp"; n=$((n + 1))
done
# kiln.tasks v1: every size/offset/tag as C11 and C++20, and the Kiln-internal host table (src/tasks_host.h, C++ only)
"$cc" -std=c11 -Wall -Wextra -Werror -fsyntax-only "${inc[@]}" "$here/tasks_abi_test.c"; n=$((n + 1))
"$cxx" -std=c++20 -x c++ -Wall -Wextra -Werror -fsyntax-only "${inc[@]}" "$here/tasks_abi_test.c"; n=$((n + 1))
"$cxx" -std=c++20 -x c++ -Wall -Wextra -Werror -fsyntax-only "${inc[@]}" -I"$here/../src" -include tasks_host.h /dev/null; n=$((n + 1))
# kiln.launch v1 (Kiln-internal, not in the SDK's headers): its layout asserts as C11 and C++20
"$cc" -std=c11 -Wall -Wextra -Werror -fsyntax-only "${inc[@]}" -include services/kiln_launch.h -x c /dev/null; n=$((n + 1))
"$cxx" -std=c++20 -Wall -Wextra -Werror -fsyntax-only "${inc[@]}" -include services/kiln_launch.h -x c++ /dev/null; n=$((n + 1))
# ...and its Kiln-internal table (src/launch_internal.h, C++ only)
"$cxx" -std=c++20 -x c++ -Wall -Wextra -Werror -fsyntax-only "${inc[@]}" -I"$here/../src" -include launch_internal.h /dev/null; n=$((n + 1))
# ...and kiln.internal.owners (src/owners_internal.h, C++ only: Kiln-internal, like launch_internal.h)
"$cxx" -std=c++20 -x c++ -Wall -Wextra -Werror -fsyntax-only "${inc[@]}" -I"$here/../src" -include owners_internal.h /dev/null; n=$((n + 1))
# kiln.hooks: the appended members' offsets and the table's size, as C11 and C++20
"$cc" -std=c11 -Wall -Wextra -Werror -fsyntax-only "${inc[@]}" -include services/kiln_hooks.h -x c /dev/null; n=$((n + 1))
"$cxx" -std=c++20 -Wall -Wextra -Werror -fsyntax-only "${inc[@]}" -include services/kiln_hooks.h -x c++ /dev/null; n=$((n + 1))
# kiln.build: grid_for_session appended at 152 (160 bytes) and KilnBuildGridSpec's layout, as C11 and C++20
"$cc" -std=c11 -Wall -Wextra -Werror -fsyntax-only "${inc[@]}" -include services/kiln_build.h -x c /dev/null; n=$((n + 1))
"$cxx" -std=c++20 -Wall -Wextra -Werror -fsyntax-only "${inc[@]}" -include services/kiln_build.h -x c++ /dev/null; n=$((n + 1))
# ...and kiln.internal.hooks (src/hooks_protect_internal.h, C++ only: Kiln-internal, like launch_internal.h)
"$cxx" -std=c++20 -x c++ -Wall -Wextra -Werror -fsyntax-only "${inc[@]}" -I"$here/../src" -include hooks_protect_internal.h /dev/null; n=$((n + 1))
# kiln.eml.startup v1 (Kiln-internal, not in the SDK's headers): the probe's size, offset, width, enum and signature
# asserts and its table extent checks, built and run as C11 and as C++20 (C++ with the archive header included first),
# and its layout part compile-only for Windows x64 in both languages (when cc is clang)
"$cc" -std=c11 -Wall -Wextra -Werror "${inc[@]}" "$here/abi_headers/eml_startup.c" -o "$out/eml-startup-c"
"$out/eml-startup-c" >/dev/null; n=$((n + 1))
"$cxx" -x c++ -std=c++20 -Wall -Wextra -Werror -DPROBE_ARCHIVE_FIRST "${inc[@]}" "$here/abi_headers/eml_startup.c" \
    -o "$out/eml-startup-cpp"
"$out/eml-startup-cpp" >/dev/null; n=$((n + 1))
rm -f "$out/eml-startup-c" "$out/eml-startup-cpp"
if [[ $("$cc" --version) == *clang* ]]; then  # no pipe: grep -q under pipefail can SIGPIPE cc and skip this
  for lang in c c++; do
    standard=c11; [[ $lang == c++ ]] && standard=c++20
    "$cc" --target=x86_64-pc-windows-msvc -x "$lang" -std="$standard" -ffreestanding -Wall -Wextra -Werror \
        -DPROBE_LAYOUT_ONLY "${inc[@]}" -fsyntax-only "$here/abi_headers/eml_startup.c"; n=$((n + 1))
  done
fi
# kiln.client and kiln.net in both forms. The core's view: the building input members appended at 56 (80 bytes in all)
# and the Steam peer-to-peer messages members appended at 128 (184 bytes), with their records, as C11 and C++20. The SDK's
# view (managed/Kiln.Sdk/Kiln.Sdk.csproj leaves out every "kiln-internal begin" ... "kiln-internal end" block of the ABI
# headers): since 0.2.0-dev.30.3 neither header has such a block (building input and the Steam members are public), so
# the SDK's kiln.client table is the core's 80 bytes with building_input_cancel its last member, and its kiln.net table
# the core's 184 bytes with steam_accept its last member, each file the core's, as C11 and C++20.
pub=$out/sdk-abi
rm -rf "$pub" && cp -r "$here/../abi" "$pub"
strip_internal() {  # the SDK's rule: a block from its begin line to its end line, both included; never nested
  awk '/^[[:space:]]*(\/\*|\/\/)[[:space:]]*kiln-internal begin/ { if (skip) exit 1; skip = 1; next }
       /^[[:space:]]*(\/\*|\/\/)[[:space:]]*kiln-internal end/ { if (!skip) exit 1; skip = 0; next }
       !skip { print } END { if (skip) exit 1 }' "$1"
}
check_view() {  # header, the names only the core's view may hold, size test, C and C++
  local h=$1 names=$2 sizes=$3
  "$cc" -std=c11 -Wall -Wextra -Werror -fsyntax-only "${inc[@]}" -include "services/$h" -x c /dev/null; n=$((n + 1))
  "$cxx" -std=c++20 -Wall -Wextra -Werror -fsyntax-only "${inc[@]}" -include "services/$h" -x c++ /dev/null; n=$((n + 1))
  strip_internal "$here/../abi/services/$h" > "$pub/services/$h" ||
    { echo "abi headers: FAIL $h's kiln-internal blocks do not pair up" >&2; exit 1; }
  local pattern=kiln-internal
  [[ -n $names ]] && pattern="$pattern|$names"
  if grep -n -E "$pattern" "$pub/services/$h" || ! cat -s "$pub/services/$h" | cmp -s - "$pub/services/$h"; then
    echo "abi headers: FAIL $h without its kiln-internal blocks still has internal text or a doubled blank line" >&2
    exit 1
  fi
  local probe
  probe=$(printf '#include <stddef.h>\n#include "services/%s"\n#ifdef __cplusplus\n#define KILN_TEST_ASSERT static_assert\n#else\n#define KILN_TEST_ASSERT _Static_assert\n#endif\n%s\n' "$h" "$sizes")
  printf '%s\n' "$probe" | "$cc" -std=c11 -Wall -Wextra -Werror -fsyntax-only -I"$pub" -x c -; n=$((n + 1))
  printf '%s\n' "$probe" | "$cxx" -std=c++20 -Wall -Wextra -Werror -fsyntax-only -I"$pub" -x c++ -; n=$((n + 1))
}
check_view kiln_client.h '' \
  'KILN_TEST_ASSERT(sizeof(KilnClientApi) == 80, "the SDK kiln.client table is 80 bytes"); KILN_TEST_ASSERT(offsetof(KilnClientApi, building_input_submit) == 56, "building input ships"); KILN_TEST_ASSERT(offsetof(KilnClientApi, building_input_cancel) == 72, "building_input_cancel is its last member"); KILN_TEST_ASSERT(sizeof(KilnBuildingInput) == 112, "with its record");'
cmp -s "$here/../abi/services/kiln_client.h" "$pub/services/kiln_client.h" ||
  { echo "abi headers: FAIL the SDK's kiln_client.h is not the core's (it has no kiln-internal block)" >&2; exit 1; }
n=$((n + 1))
check_view kiln_net.h '' \
  'KILN_TEST_ASSERT(sizeof(KilnNetApi) == 184, "the SDK kiln.net table is 184 bytes"); KILN_TEST_ASSERT(offsetof(KilnNetApi, steam_status) == 128, "the Steam members ship"); KILN_TEST_ASSERT(offsetof(KilnNetApi, steam_accept) == 176, "steam_accept is its last member"); KILN_TEST_ASSERT(sizeof(KilnSteamMessagesStatus) == 48 && sizeof(KilnSteamMessageInfo) == 24, "with their records");'
cmp -s "$here/../abi/services/kiln_net.h" "$pub/services/kiln_net.h" ||
  { echo "abi headers: FAIL the SDK's kiln_net.h is not the core's (it has no kiln-internal block)" >&2; exit 1; }
n=$((n + 1))
# no ABI header has a kiln-internal block (the SDK would strip it from every header the same way; a new one needs its SDK
# view check here)
if grep -l -E 'kiln-internal (begin|end)' "$here/../abi"/*.h "$here/../abi"/services/*.h; then
  echo "abi headers: FAIL a kiln-internal block in an ABI header (add its SDK view check here)" >&2; exit 1
fi
n=$((n + 1))
rm -rf "$pub"
echo "abi headers: PASS $n checks (kiln_world.h and kiln_abilities.h together in both orders, C and C++; each alone; kiln_tasks.h, tasks_host.h; kiln_launch.h, launch_internal.h; kiln_hooks.h, hooks_protect_internal.h; kiln_eml_startup.h run as C and C++, Windows x64 layout; kiln_build.h as C11 and C++20; kiln_client.h and kiln_net.h whole in the core's view and the SDK's)"
