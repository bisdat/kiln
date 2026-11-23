#!/usr/bin/env bash
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
out=${KILN_OUT:?set KILN_OUT}/flame-host
mkdir -p "$out"
${CXX:-g++} -std=c++20 -O1 -g -pthread -Wall -Wextra -fsanitize=address,undefined \
  -I"$here/host" -I"$here/../../../core/abi" "$here/flame_test.cpp" -o "$out/flame_test"
ASAN_OPTIONS=detect_leaks=0 "$out/flame_test"
# the roster (package 17c): roster_core.h and the builder's admission, with the sanitizers and with threads
${CXX:-g++} -std=c++20 -O1 -g -pthread -Wall -Wextra -fsanitize=address,undefined \
  -I"$here/.." -I"$here/../../../core/abi" "$here/roster_test.cpp" "$here/../questbuild.cpp" -o "$out/roster_test"
ASAN_OPTIONS=detect_leaks=0 "$out/roster_test"
${CXX:-g++} -std=c++20 -O1 -g -pthread -Wall -Wextra -fsanitize=thread \
  -I"$here/.." -I"$here/../../../core/abi" "$here/roster_test.cpp" "$here/../questbuild.cpp" -o "$out/roster_test_tsan"
"$out/roster_test_tsan"
# the boot build's stops (0.2.0-dev.29, contentstop): native_pack.cpp with fake Kiln services and a fake build; the texts
${CXX:-g++} -std=c++20 -O1 -g -pthread -Wall -Wextra -Wno-missing-field-initializers -fsanitize=address,undefined \
  -I"$here/host" -I"$here/.." -I"$here/../../../core/abi" -I"$here/../../../third_party" \
  "$here/native_pack_test.cpp" "$here/../bootpack.cpp" "$here/../questbuild.cpp" -o "$out/native_pack_test"
ASAN_OPTIONS=detect_leaks=0 "$out/native_pack_test" "$out"
# Fisher's portable body serializer: argument and finite-value boundaries (full roundtrips in tools/kfc/tests).
${CXX:-g++} -std=c++20 -O1 -g -Wall -Wextra -Wno-missing-field-initializers -fsanitize=address,undefined \
  -I"$here/.." "$here/bodypack_test.cpp" "$here/../bodypack.cpp" "$here/../questbuild.cpp" -o "$out/bodypack_test"
ASAN_OPTIONS=detect_leaks=0 "$out/bodypack_test"
# per-mod native boot rollback (16f): the real publisher with failure injection and isolated owners
${CXX:-g++} -std=c++20 -O1 -g -pthread -Wall -Wextra -Wno-missing-field-initializers -fsanitize=address,undefined \
  -I"$here/host" -I"$here/.." -I"$here/../../../core/abi" -I"$here/../../../third_party" \
  "$here/bootpack_native_test.cpp" "$here/../bootpack.cpp" "$here/../questbuild.cpp" -o "$out/bootpack_native_test"
ASAN_OPTIONS=detect_leaks=0 "$out/bootpack_native_test" "$out"
# The real compiler over synthetic knowledge maps: bounded per-pack resources and compact rollback records.
${CXX:-g++} -std=c++20 -O1 -g -pthread -Wall -Wextra -Wno-missing-field-initializers -fsanitize=address,undefined \
  -I"$here/.." -I"$here/../../../third_party" \
  "$here/bootpack_memory_test.cpp" "$here/../bootpack.cpp" "$here/../questpack.cpp" "$here/../questbuild.cpp" -o "$out/bootpack_memory_test"
ASAN_OPTIONS=detect_leaks=0 "$out/bootpack_memory_test"
${CXX:-g++} -std=c++20 -O1 -g -pthread -Wall -Wextra -Wno-missing-field-initializers -fsanitize=address,undefined \
  -I"$here/.." -I"$here/../../../third_party" \
  "$here/bootpack_compile_test.cpp" "$here/../bootpack.cpp" "$here/../questpack.cpp" "$here/../questbuild.cpp" -o "$out/bootpack_compile_test"
ASAN_OPTIONS=detect_leaks=0 "$out/bootpack_compile_test"
${CXX:-g++} -std=c++20 -O1 -g -pthread -Wall -Wextra -fsanitize=address,undefined \
  "$here/../../content/tests/boot_text_undo_test.cpp" -o "$out/boot_text_undo_test"
ASAN_OPTIONS=detect_leaks=0 "$out/boot_text_undo_test"
# kiln.survivors' game-free core (dev.31 package 18): request shapes, outcome and authorization rules, result cache, events
${CXX:-g++} -std=c++20 -O1 -g -pthread -Wall -Wextra -fsanitize=address,undefined \
  -I"$here/.." -I"$here/../../../core/abi" "$here/survivors_test.cpp" -o "$out/survivors_test"
ASAN_OPTIONS=detect_leaks=0 "$out/survivors_test"
${CXX:-g++} -std=c++20 -O1 -g -pthread -Wall -Wextra -fsanitize=thread \
  -I"$here/.." -I"$here/../../../core/abi" "$here/survivors_test.cpp" -o "$out/survivors_test_tsan"
"$out/survivors_test_tsan"
# kiln.survivors' request handling (survivors.cpp) with fake services: review findings S1-S3
${CXX:-g++} -std=c++20 -O1 -g -pthread -Wall -Wextra -Wno-unused-function -Wno-missing-field-initializers -fsanitize=address,undefined \
  -I"$here/host" -I"$here/.." -I"$here/../../../core/abi" "$here/survivors_flow_test.cpp" -o "$out/survivors_flow_test"
ASAN_OPTIONS=detect_leaks=0 "$out/survivors_flow_test"
