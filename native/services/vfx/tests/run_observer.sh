#!/usr/bin/env bash
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../../../.." && pwd)
out=$KILN_OUT/vfx-observer
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
g++ -std=c++20 -O0 -g0 -Wall -Wextra -fsanitize=address,undefined -pthread -I"$root/native/core/abi" -I"$root/native/third_party" -I"$out" "$here/test_observer.cpp" "$here/../ledger.cpp" -o "$out/test_observer"
"$out/test_observer"
