#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# kiln.staged on the Windows code path under headless Wine: src/staged.cpp as it is (MoveFileExW without replace and
# write-through, FlushFileBuffers, reparse points) and native/core/tests/staged_test.cpp, built with clang-cl against the
# xwin SDK; every crash point runs in a child started with CreateProcess that ends itself with TerminateProcess.
#   native/core/tests/test-staged-wine.sh
# Output: $KILN_OUT/staged-wine when KILN_OUT is set, else a private temporary directory removed on exit. The Wine
# prefix: $KILN_WINEPREFIX when set, else <output>/pfx (an inherited WINEPREFIX is ignored).
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
src=$here/../src
abi=$here/../abi
if [[ -n ${KILN_OUT:-} ]]; then
  out=$KILN_OUT/staged-wine
  mkdir -p "$out"
else
  out=$(mktemp -d "${TMPDIR:-/tmp}/kiln-staged-wine.XXXXXX")
  trap 'rm -rf "$out"' EXIT
fi
xwin=${XWIN_ROOT:-$HOME/.cache/cargo-xwin/xwin}
[[ -d $xwin/crt/include ]] || { printf 'No xwin SDK under %s; run `cargo xwin env` once\n' "$xwin" >&2; exit 2; }
inc=(-imsvc "$xwin/crt/include" -imsvc "$xwin/sdk/include/ucrt" -imsvc "$xwin/sdk/include/um" -imsvc "$xwin/sdk/include/shared")
libdirs=(-libpath:"$xwin/crt/lib/x86_64" -libpath:"$xwin/sdk/lib/um/x86_64" -libpath:"$xwin/sdk/lib/ucrt/x86_64")
cl=(clang-cl --target=x86_64-pc-windows-msvc /std:c++20 /EHa /W4 /WX /O1 /MT -Wno-msvc-not-found -fuse-ld=lld "${inc[@]}"
    /DUNICODE /D_UNICODE /DNOMINMAX /D_CRT_SECURE_NO_WARNINGS -I"$src" -I"$abi" -I"$here/../../third_party")
"${cl[@]}" "$here/staged_test.cpp" "$src/staged.cpp" /Fe:"$out/staged_test.exe" /link "${libdirs[@]}" >/dev/null

out=$(cd "$out" && pwd)
export WINEPREFIX=${KILN_WINEPREFIX:-$out/pfx}
mkdir -p "$WINEPREFIX"
unset DISPLAY
export WAYLAND_DISPLAY=kiln-no-display WINEDLLOVERRIDES=winedbg.exe=d WINEDEBUG=-all
[[ -f $WINEPREFIX/system.reg ]] || timeout 120s wine wineboot -i >/dev/null 2>&1
set +e
( cd "$out" && timeout 600s wine staged_test.exe ) 2>"$out/stderr.txt"
rc=$?
timeout 60s wineserver -w
set -e
grep -v '^[0-9a-f]*:\(fixme\|err\|warn\)' "$out/stderr.txt" >&2 || true
exit $rc
