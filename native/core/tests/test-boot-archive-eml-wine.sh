#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# kiln.boot's kiln-backend sources (services/boot_archive_eml.cpp) on the Windows code path under headless Wine: the host
# boot_archive_eml_windows_host.cpp and a stand-in kiln-eml.dll (and other-name.dll, and old\kiln-eml.dll: a kiln-eml
# from before kiln.eml.startup's published_identity), built with clang-cl against the xwin SDK. PRIVATE
# (tools/sfi/public_cut.py). Build and run:
#   native/core/tests/test-boot-archive-eml-wine.sh
# Output: $KILN_OUT/boot-archive-eml-wine when KILN_OUT is set, else a private temporary directory removed on exit.
# The Wine prefix: $KILN_WINEPREFIX when set, else <output>/pfx. KILN_BUILD_ONLY=1 stops after the build.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
if [[ -n ${KILN_OUT:-} ]]; then
  out=$KILN_OUT/boot-archive-eml-wine
  mkdir -p "$out"
else
  out=$(mktemp -d "${TMPDIR:-/tmp}/kiln-boot-archive-eml-wine.XXXXXX")
  trap 'rm -rf "$out"' EXIT
fi
xwin=${XWIN_ROOT:-$HOME/.cache/cargo-xwin/xwin}
[[ -d $xwin/crt/include ]] || { printf "No xwin SDK under %s\n" "$xwin" >&2; exit 2; }
inc=(-imsvc "$xwin/crt/include" -imsvc "$xwin/sdk/include/ucrt" -imsvc "$xwin/sdk/include/um" -imsvc "$xwin/sdk/include/shared")
libdirs=(-libpath:"$xwin/crt/lib/x86_64" -libpath:"$xwin/sdk/lib/um/x86_64" -libpath:"$xwin/sdk/lib/ucrt/x86_64")
cl=(clang-cl --target=x86_64-pc-windows-msvc /W4 /WX /O1 /MT -Wno-msvc-not-found -fuse-ld=lld "${inc[@]}"
    -DWIN32_LEAN_AND_MEAN -DNOMINMAX -DUNICODE -D_UNICODE
    -I"$here/../src" -I"$here/../src/services" -I"$here/../abi" -I"$here/../../third_party")
for name in kiln-eml other-name; do
  "${cl[@]}" /LD "$here/boot_archive_eml_standin.c" /Fe:"$out/$name.dll" /Fo:"$out/$name.obj" /link "${libdirs[@]}" \
    /EXPORT:kiln_eml_archive_service /EXPORT:kiln_eml_startup_service >/dev/null
done
mkdir -p "$out/old"
"${cl[@]}" /LD -DSTANDIN_OLD_STARTUP "$here/boot_archive_eml_standin.c" /Fe:"$out/old/kiln-eml.dll" /Fo:"$out/old/kiln-eml.obj" \
  /link "${libdirs[@]}" /EXPORT:kiln_eml_archive_service /EXPORT:kiln_eml_startup_service >/dev/null
"${cl[@]}" /std:c++20 /EHsc "$here/boot_archive_eml_windows_host.cpp" "$here/../src/services/boot_archive_eml.cpp" \
  /Fe:"$out/boot_archive_eml_windows_host.exe" /link "${libdirs[@]}" >/dev/null
[[ ${KILN_BUILD_ONLY:-} == 1 ]] && exit 0
out=$(cd "$out" && pwd)
# the game runs under GE-Proton's Wine on the test PC; prefer it (tools/sfi/proton-wine.sh)
here_pw=$(cd "$(dirname "$0")/../../.." && pwd)
# shellcheck source=/dev/null
[[ -f $here_pw/tools/sfi/proton-wine.sh ]] && source "$here_pw/tools/sfi/proton-wine.sh"
export WINEPREFIX=${KILN_WINEPREFIX:-$out/pfx}
mkdir -p "$WINEPREFIX"
unset DISPLAY
export WAYLAND_DISPLAY=kiln-no-display WINEDLLOVERRIDES=winedbg.exe=d WINEDEBUG=-all
[[ -f $WINEPREFIX/system.reg ]] || timeout 120s wine wineboot -i >/dev/null 2>&1
set +e
( cd "$out" && timeout 120s wine boot_archive_eml_windows_host.exe )
rc=$?
timeout 60s wineserver -w
set -e
exit $rc
