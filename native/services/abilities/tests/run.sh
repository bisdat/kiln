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

# The impact pool watch's pure parts (impact_watch.h): layout check, thresholds, top programs, age table, control words.
g++ "${flags[@]}" "$here/impact_watch_test.cpp" -o "$out/impact_watch_test"
"$out/impact_watch_test"

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
start = source.index("void free_retired()")
end = source.index("\n// The program `id`", start)
registration += "\n" + source[start:end]
start = source.index("void dispatch_managed(")
end = source.index("\nstd::atomic<uint32_t> g_slow_logged", start)
registration += "\n" + source[start:end]
start = source.index("void check_programs_ready()")
end = source.index("\nvoid expire()", start)
registration += "\n" + source[start:end]
start = source.index("KilnResult KILN_CALL t_native_state(")
end = source.index("\nKilnResult KILN_CALL t_native_attach_state(", start)
Path(sys.argv[2]).write_text(registration + "\n" + source[start:end])
start = source.index("enum SlotKind")
end = source.index("\nstd::unordered_map<uint32_t, int> g_slot_of", start)
Path(sys.argv[3]).write_text(source[start:end])
PYCODE
g++ "${flags[@]}" -I"$out" "$here/registration_test.cpp" -o "$out/registration_test"
"$out/registration_test"

python3 - "$here/../abilities.cpp" "$out/watchdog_under_test.h" <<'PYCODE'
from pathlib import Path
import sys
s = Path(sys.argv[1]).read_text()
Path(sys.argv[2]).write_text(s[s.index("void watchdog_loop()"):s.index("void start_watchdog()")])
PYCODE
g++ "${flags[@]}" -I"$out" "$here/watchdog_test.cpp" -o "$out/watchdog_test"
"$out/watchdog_test"

python3 - "$here/../abilities.cpp" "$out/ecs_under_test.h" <<'PYCODE'
from pathlib import Path
import sys
s = Path(sys.argv[1]).read_text()
start = s.index("bool world_foreign()")
prefix = s[start:s.index("\n}\n", start) + 3]
Path(sys.argv[2]).write_text(prefix + s[s.index("const EcsView* ecs_for("):s.index("// ---- the watchdog")])
PYCODE
g++ "${flags[@]}" -I"$out" "$here/ecs_test.cpp" -o "$out/ecs_test"
"$out/ecs_test"

# Exercise the production queue, drained stop and next-Pre delivery/cancellation bodies.
python3 - "$here/../abilities.cpp" "$out/events_under_test.h" <<'PYCODE'
from pathlib import Path
import sys
s = Path(sys.argv[1]).read_text()
queue = s[s.index("bool push_event("):s.index("KilnAbilityEvent new_event(")]
delivery = s[s.index("    // collect events\n"):s.index("\nvoid KILN_CALL on_world(")]
stop = s[s.index("void KILN_CALL on_world("):s.index("// ------------------------------------------------------------------------------------------- table")]
Path(sys.argv[2]).write_text(queue + "\nvoid deliver(uintptr_t world) {\n" + delivery + stop)
PYCODE
g++ "${flags[@]}" -I"$out" "$here/events_test.cpp" -o "$out/events_test"
"$out/events_test"

# #108: the program lifecycle across world changes (add_program, refresh_programs, check_programs_ready, on_world)
# against a fake ImpactRegistry: the same registry address again, another one, a stop that is never seen. Twice: with
# ASan/UBSan, and with TSan for the stop on the destroying thread against the tick thread's registrations.
python3 - "$here/../abilities.cpp" "$out/programs_under_test.h" <<'PYCODE'
from pathlib import Path
import sys
s = Path(sys.argv[1]).read_text()
programs = s[s.index("// ---- programs ----"):s.index("// ------------------------------------------------------------------------------------------- Pre")]
ready = s[s.index("void check_programs_ready()"):s.index("\nvoid expire()")]
stop = s[s.index("void KILN_CALL on_world("):s.index("// ------------------------------------------------------------------------------------------- table")]
Path(sys.argv[2]).write_text(programs + "\n" + ready + "\n" + stop)
PYCODE
g++ "${flags[@]}" -I"$out" "$here/programs_test.cpp" "$here/../verify.cpp" -o "$out/programs_test"
"$out/programs_test"
tsan=("${flags[@]/-fsanitize=address,undefined/-fsanitize=thread}")
g++ "${tsan[@]}" -I"$out" "$here/programs_test.cpp" "$here/../verify.cpp" -o "$out/programs_test_tsan"
TSAN_OPTIONS=halt_on_error=1 "$out/programs_test_tsan"
