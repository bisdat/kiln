#!/usr/bin/env bash
# Actual serving, rollback and tick-registration code against fake assets; no Windows or game.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../../../.." && pwd)
: "${KILN_OUT:?set KILN_OUT}"
out=$KILN_OUT/vfx-boot-build
mkdir -p "$out"
python3 - "$root" "$out" <<'PY'
import sys
from pathlib import Path
root, out = map(Path, sys.argv[1:])
s = (root / 'native/services/vfx/boot.cpp').read_text()
def between(a, b): return a + s.split(a, 1)[1].split(b, 1)[0]
(out / 'boot_build_impl.inc').write_text(
    between('namespace fs =', 'fs::path to_path(') +
    between('KilnOwner reskin_owner_locked(', '\n}  // namespace\n') +
    between('void build_forget() {', '// The game resolves a VFX GUID') +
    'void subscribe_reskins(const KilnApi* api, KilnOwner self) {\n' +
    between('        {\n            std::lock_guard lk(g_mu);\n            if (!g_reskins.empty()', '        {\n            std::lock_guard lk(g_mu);\n            for (const kvb::BuiltBlock&') + '}\n' +
    'void build_serve(const kvb::CookResult& r, [[maybe_unused]] const kiln::boot::Input* boot = nullptr) {\n' +
    between('        if (!serve(r', '        {\n            std::lock_guard lk(g_mu);\n            if (!g_reskins.empty()') + '}\n')
PY
c++ -std=c++23 -O0 -g0 -Wall -Wextra -fsanitize=address,undefined -pthread \
 -I"$root/native/third_party" -I"$root/native/core/abi" -I"$out" \
 "$here/test_boot_build.cpp" "$here/../cook/contenthash.cpp" "$here/../cook/ids.cpp" "$here/../ledger.cpp" -o "$out/test_boot_build"
"$out/test_boot_build"
