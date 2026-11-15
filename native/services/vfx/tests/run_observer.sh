#!/usr/bin/env bash
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../../../.." && pwd)
out=$KILN_OUT/vfx-observer
ledger=${VFX_LEDGER_SOURCE:-$here/../ledger.cpp}
mkdir -p "$out"
python3 - "$root" "$out" <<'PY'
import sys
from pathlib import Path
root,out=map(Path,sys.argv[1:])
s=(root/'native/services/vfx/boot.cpp').read_text()
start=s.index('namespace {\nstd::atomic<uint32_t> g_observe_n')
end=s.index('void build_on_update(',start)
(out/'observer.inc').write_text(s[start:end])
PY
g++ -std=c++20 -O0 -g0 -Wall -Wextra -fsanitize=address,undefined -pthread -DKILN_VFX_LEDGER_TEST -I"$root/native/core/abi" -I"$root/native/third_party" -I"$out" -I"$here/.." "$here/test_observer.cpp" "$ledger" "$here/../pending_cleanup.cpp" -o "$out/test_observer"
"$out/test_observer" "$out/stalled-files"
g++ -std=c++20 -O0 -g0 -Wall -Wextra -fsanitize=address,undefined -pthread -DKILN_VFX_LEDGER_TEST -I"$root/native/core/abi" -I"$root/native/third_party" -I"$out" -I"$here/.." \
 "$here/test_registry_walk.cpp" "$here/../core.cpp" "$here/../sim.cpp" "$ledger" "$here/../pending_cleanup.cpp" -o "$out/test_registry_walk"
"$out/test_registry_walk"
