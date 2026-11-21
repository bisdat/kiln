#!/usr/bin/env bash
# debug-inspect.sh <signed kiln.exe> <sdk dir> <version>: stages the debug-inspect helper (kiln debug's Windows side under
# Wine/Proton; docs/kiln/modding/debugging.md) for an SDK folder:
#   <sdk>/tools/debug-inspect/win-x64/kiln.exe     the Windows kiln.exe (signed first: the hash is of the shipped bytes)
#   <sdk>/tools/debug-inspect/manifest.json        {"format":1,"helper":"win-x64/kiln.exe","sha256":"<hex>","kiln":"<version>"}
#   <sdk>/kiln-debug-inspect-<version>.zip         tools/debug-inspect/... for a kiln downloaded on its own: unpacked into
#                                                  the Kiln data folder (or next to kiln) it is found the same way
# kiln looks for tools/debug-inspect next to itself, then in the data folder (crates/kiln-cli/src/debug_inspect.rs
# find_helper), and starts the helper only when its SHA-256 is the manifest's.
set -euo pipefail
exe=${1:?usage: debug-inspect.sh <kiln.exe> <sdk dir> <version>}; sdk=${2:?}; version=${3:?}
[[ -f $exe ]] || { echo "debug-inspect.sh: no $exe" >&2; exit 1; }
dir=$sdk/tools/debug-inspect
rm -rf "$dir"
install -D -m 644 "$exe" "$dir/win-x64/kiln.exe"
sha=$(sha256sum "$dir/win-x64/kiln.exe" | cut -d' ' -f1)
printf '{"format":1,"helper":"win-x64/kiln.exe","sha256":"%s","kiln":"%s"}\n' "$sha" "$version" > "$dir/manifest.json"
zip=$sdk/kiln-debug-inspect-$version.zip
rm -f "$zip"
(cd "$sdk" && TZ=UTC find tools/debug-inspect -exec touch -h -d @"${SOURCE_DATE_EPOCH:-0}" {} + && zip -q -X -r "$zip" tools/debug-inspect)
echo "debug-inspect: $dir (sha256 $sha), $zip"
