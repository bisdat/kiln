#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# A staged Kiln update at the real proxy's entry gate (staged_gate/gate_test.py) under headless Wine: dinput8.dll as
# native/build.sh builds it, a stand-in enshrouded_server.exe that imports it and two stand-in kiln-core.dll versions.
# On Windows, run the same test natively: python staged_gate/gate_test.py <scratch> <bin> with the files this script
# builds into <output>/bin.
#   native/proxy/tests/test-staged-gate.sh
# Output: $KILN_OUT/staged-gate (KILN_OUT default native/build); the Wine prefix: $KILN_WINEPREFIX, else <output>/pfx.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
native=$here/../..
export KILN_OUT=${KILN_OUT:-$native/build}
out=$KILN_OUT/staged-gate
mkdir -p "$out/bin"
"$native/build.sh" kiln-proxy >/dev/null
cp "$KILN_OUT/cmake/bin/dinput8.dll" "$out/bin/"
xwin=${XWIN_ROOT:-$HOME/.cache/cargo-xwin/xwin}
[[ -d $xwin/crt/include ]] || { printf 'No xwin SDK under %s; run `cargo xwin env` once\n' "$xwin" >&2; exit 2; }
cl=(clang-cl --target=x86_64-pc-windows-msvc /std:c++20 /EHsc /O1 /MT -Wno-msvc-not-found -fuse-ld=lld
    -imsvc "$xwin/crt/include" -imsvc "$xwin/sdk/include/ucrt" -imsvc "$xwin/sdk/include/um" -imsvc "$xwin/sdk/include/shared"
    /DUNICODE /D_UNICODE)
libs=(-libpath:"$xwin/crt/lib/x86_64" -libpath:"$xwin/sdk/lib/um/x86_64" -libpath:"$xwin/sdk/lib/ucrt/x86_64")
tmp=$(mktemp -d); trap 'rm -rf "$tmp"' EXIT
(cd "$tmp" && "${cl[@]}" "$here/staged_gate/fake_server.cpp" /Fe:"$out/bin/enshrouded_server.exe" /link "${libs[@]}" dinput8.lib >/dev/null
 for v in A B; do "${cl[@]}" /LD /DCORE_TAG=core-$v "$here/staged_gate/fake_core.cpp" /Fe:"$out/bin/core-$v.dll" /link "${libs[@]}" >/dev/null; done)
export WINEPREFIX=${KILN_WINEPREFIX:-$out/pfx}
mkdir -p "$WINEPREFIX"
unset DISPLAY
export WAYLAND_DISPLAY=kiln-no-display WINEDEBUG=-all
[[ -f $WINEPREFIX/system.reg ]] || timeout 120s wine wineboot -i >/dev/null 2>&1
set +e
python3 -I "$here/staged_gate/gate_test.py" "$out/run" "$out/bin" --wine
rc=$?
timeout 60s wineserver -w
exit $rc
