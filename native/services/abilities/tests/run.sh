#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Host tests of kiln.abilities' native thunk decisions and the program verifier, with ASan/UBSan:
#   KILN_OUT=<build dir> native/services/abilities/tests/run.sh
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
out=${KILN_OUT:?set KILN_OUT}/abilities-host-test
mkdir -p "$out"
flags=(-std=c++20 -O1 -g -pthread -Wall -Wextra -fsanitize=address,undefined -fno-sanitize-recover=undefined
       -D__fastcall= -I"$here/.." -I"$here/../../../core/abi" -I"$here/../../../third_party")
g++ "${flags[@]}" "$here/natives_test.cpp" "$here/../verify.cpp" -o "$out/natives_test"
"$out/natives_test"

# Compile the actual Windows thunk body against a tiny fake dispatcher on the host.
python3 - "$here/../abilities.cpp" "$out/thunk_under_test.h" <<'PYCODE'
from pathlib import Path
import sys
source = Path(sys.argv[1]).read_text()
start = source.index("template <int I>\nNativeResult* __fastcall thunk")
end = source.index("\ntemplate <int... I>", start)
Path(sys.argv[2]).write_text(source[start:end])
PYCODE
g++ "${flags[@]}" -I"$out" "$here/thunk_test.cpp" -o "$out/thunk_test"
"$out/thunk_test"

python3 - "$here/../abilities.cpp" "$out/registration_under_test.h" "$out/slot_under_test.h" <<'PYCODE'
from pathlib import Path
import sys
source = Path(sys.argv[1]).read_text()
start = source.index("int add_kiln_native(")
end = source.index("\nKilnResult KILN_CALL t_native_register(", start)
registration = source[start:end]
start = source.index("void dispatch_managed(")
end = source.index("\nstd::atomic<uint32_t> g_slow_logged", start)
registration += "\n" + source[start:end]
start = source.index("void check_programs_ready()")
end = source.index("\nvoid expire()", start)
Path(sys.argv[2]).write_text(registration + "\n" + source[start:end])
start = source.index("enum SlotKind")
end = source.index("\nstd::unordered_map<uint32_t, int> g_slot_of", start)
Path(sys.argv[3]).write_text(source[start:end])
PYCODE
g++ "${flags[@]}" -I"$out" "$here/registration_test.cpp" -o "$out/registration_test"
"$out/registration_test"
