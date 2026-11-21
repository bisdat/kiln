#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# kiln/state/launch.json on the Windows code path under headless Wine: common.cpp's write_file_atomic, cut out of the
# file as it is, and src/launch.cpp as it is, built with clang-cl against the xwin SDK (launch_windows_host.cpp). No
# game, no CMake. Build and run:
#   native/core/tests/test-launch-wine.sh
# Output: $KILN_OUT/launch-wine when KILN_OUT is set, else a private temporary directory removed on exit. The Wine
# prefix: $KILN_WINEPREFIX when set, else <output>/pfx (an inherited WINEPREFIX is ignored).
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
src=$here/../src
abi=$here/../abi
if [[ -n ${KILN_OUT:-} ]]; then
  out=$KILN_OUT/launch-wine
  mkdir -p "$out"
else
  out=$(mktemp -d "${TMPDIR:-/tmp}/kiln-launch-wine.XXXXXX")
  trap 'rm -rf "$out"' EXIT
fi
xwin=${XWIN_ROOT:-$HOME/.cache/cargo-xwin/xwin}
[[ -d $xwin/crt/include ]] || { printf 'No xwin SDK under %s; run `cargo xwin env` once\n' "$xwin" >&2; exit 2; }
inc=(-imsvc "$xwin/crt/include" -imsvc "$xwin/sdk/include/ucrt" -imsvc "$xwin/sdk/include/um" -imsvc "$xwin/sdk/include/shared")
libdirs=(-libpath:"$xwin/crt/lib/x86_64" -libpath:"$xwin/sdk/lib/um/x86_64" -libpath:"$xwin/sdk/lib/ucrt/x86_64")
cl=(clang-cl --target=x86_64-pc-windows-msvc /std:c++20 /EHsc /W4 /WX /O1 /MT -Wno-msvc-not-found -fuse-ld=lld "${inc[@]}"
    -I"$out" -I"$src" -I"$abi" -I"$here/../../third_party")

python3 -I - "$src/common.cpp" "$out" <<'PY'
import pathlib, sys
common = pathlib.Path(sys.argv[1]).read_text()
start = common.index('bool write_file_atomic(')
end = common.index('bool read_file(', start)
(pathlib.Path(sys.argv[2]) / 'write_file_atomic.inc').write_text(common[start:end])
PY
"${cl[@]}" "$here/launch_windows_host.cpp" "$src/launch.cpp" /Fe:"$out/launch_windows_host.exe" /link "${libdirs[@]}" >/dev/null

out=$(cd "$out" && pwd)
export WINEPREFIX=${KILN_WINEPREFIX:-$out/pfx}
mkdir -p "$WINEPREFIX"
unset DISPLAY
export WAYLAND_DISPLAY=kiln-no-display WINEDLLOVERRIDES=winedbg.exe=d WINEDEBUG=-all
[[ -f $WINEPREFIX/system.reg ]] || timeout 120s wine wineboot -i >/dev/null 2>&1
rm -rf "$out/state"
mkdir -p "$out/state"
set +e
( cd "$out" && timeout 90s wine launch_windows_host.exe 'state' ) 2>"$out/stderr.txt"
rc=$?
timeout 60s wineserver -w
set -e
exit $rc
