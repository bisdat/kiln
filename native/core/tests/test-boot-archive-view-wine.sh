#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# The base view tests (boot_archive_view_test.cpp) on the Windows code path under headless Wine: built with clang-cl
# against the xwin SDK (std::filesystem's wide paths, the Windows threads). No game, no CMake. Build and run:
#   native/core/tests/test-boot-archive-view-wine.sh
# Output: $KILN_OUT/boot-archive-view-wine when KILN_OUT is set, else a private temporary directory removed on exit.
# The Wine prefix: $KILN_WINEPREFIX when set, else <output>/pfx (an inherited WINEPREFIX is ignored). With
# KILN_BUILD_ONLY=1 it stops after the build (the .exe for a native Windows run).
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
if [[ -n ${KILN_OUT:-} ]]; then
  out=$KILN_OUT/boot-archive-view-wine
  mkdir -p "$out"
else
  out=$(mktemp -d "${TMPDIR:-/tmp}/kiln-boot-archive-view-wine.XXXXXX")
  trap 'rm -rf "$out"' EXIT
fi
xwin=${XWIN_ROOT:-$HOME/.cache/cargo-xwin/xwin}
[[ -d $xwin/crt/include ]] || { printf 'No xwin SDK under %s; run `cargo xwin env` once\n' "$xwin" >&2; exit 2; }
inc=(-imsvc "$xwin/crt/include" -imsvc "$xwin/sdk/include/ucrt" -imsvc "$xwin/sdk/include/um" -imsvc "$xwin/sdk/include/shared")
libdirs=(-libpath:"$xwin/crt/lib/x86_64" -libpath:"$xwin/sdk/lib/um/x86_64" -libpath:"$xwin/sdk/lib/ucrt/x86_64")
cl=(clang-cl --target=x86_64-pc-windows-msvc /std:c++20 /EHsc /W4 /WX /O1 /MT -Wno-msvc-not-found -fuse-ld=lld "${inc[@]}"
    -I"$here/../src" -I"$here/../abi" -I"$here/../../third_party")
"${cl[@]}" "$here/boot_archive_view_test.cpp" /Fe:"$out/boot_archive_view_test.exe" /link "${libdirs[@]}" >/dev/null
[[ ${KILN_BUILD_ONLY:-} == 1 ]] && exit 0
out=$(cd "$out" && pwd)
export WINEPREFIX=${KILN_WINEPREFIX:-$out/pfx}
mkdir -p "$WINEPREFIX"
unset DISPLAY
export WAYLAND_DISPLAY=kiln-no-display WINEDLLOVERRIDES=winedbg.exe=d WINEDEBUG=-all
[[ -f $WINEPREFIX/system.reg ]] || timeout 120s wine wineboot -i >/dev/null 2>&1
rm -rf "$out/scratch"
mkdir -p "$out/scratch"
set +e
( cd "$out" && timeout 300s wine boot_archive_view_test.exe scratch )
rc=$?
timeout 60s wineserver -w
set -e
exit $rc
