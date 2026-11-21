#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# kiln.client's local input observation on the Windows code path under headless Wine: client_input.cpp,
# client_building_input.cpp and guard.cpp as they are, built with clang-cl against the xwin SDK and /EHa as the core
# is (client_input_windows_host.cpp). No game, no CMake. Build and run:
#   native/core/tests/test-client-input-wine.sh
# Output: $KILN_OUT/client-input-wine when KILN_OUT is set, else a private temporary directory removed on exit. The Wine
# prefix: $KILN_WINEPREFIX when set, else <output>/pfx (an inherited WINEPREFIX is ignored).
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
src=$here/../src
abi=$here/../abi
if [[ -n ${KILN_OUT:-} ]]; then
  out=$KILN_OUT/client-input-wine
  mkdir -p "$out"
else
  out=$(mktemp -d "${TMPDIR:-/tmp}/kiln-client-input-wine.XXXXXX")
  trap 'rm -rf "$out"' EXIT
fi
xwin=${XWIN_ROOT:-$HOME/.cache/cargo-xwin/xwin}
[[ -d $xwin/crt/include ]] || { printf 'No xwin SDK under %s; run `cargo xwin env` once\n' "$xwin" >&2; exit 2; }
inc=(-imsvc "$xwin/crt/include" -imsvc "$xwin/sdk/include/ucrt" -imsvc "$xwin/sdk/include/um" -imsvc "$xwin/sdk/include/shared")
libdirs=(-libpath:"$xwin/crt/lib/x86_64" -libpath:"$xwin/sdk/lib/um/x86_64" -libpath:"$xwin/sdk/lib/ucrt/x86_64")
# /EHa as native/CMakeLists.txt: with /EHsc clang may drop a __try around an inlined copy
# common.h wants the generated kiln_version.h: the host tests' stub, copied alone (tests/stubs also has a windows.h stub)
cp "$here/stubs/kiln_version.h" "$out/kiln_version.h"
cl=(clang-cl --target=x86_64-pc-windows-msvc /std:c++20 /EHa /W4 /WX /O1 /MT -Wno-msvc-not-found -fuse-ld=lld "${inc[@]}"
    -DWIN32_LEAN_AND_MEAN -DNOMINMAX -I"$out" -I"$src" -I"$abi")
"${cl[@]}" "$here/client_input_windows_host.cpp" "$src/services/client_input.cpp" "$src/services/client_building_input.cpp" \
  "$src/guard.cpp" /Fe:"$out/client_input_windows_host.exe" /link "${libdirs[@]}" >/dev/null

out=$(cd "$out" && pwd)
export WINEPREFIX=${KILN_WINEPREFIX:-$out/pfx}
mkdir -p "$WINEPREFIX"
unset DISPLAY
export WAYLAND_DISPLAY=kiln-no-display WINEDLLOVERRIDES=winedbg.exe=d WINEDEBUG=-all
[[ -f $WINEPREFIX/system.reg ]] || timeout 120s wine wineboot -i >/dev/null 2>&1
set +e
( cd "$out" && timeout 90s wine client_input_windows_host.exe )
rc=$?
timeout 60s wineserver -w || { printf 'wineserver of %s did not exit within 60 s\n' "$WINEPREFIX" >&2; [[ $rc != 0 ]] || rc=1; }
exit $rc
