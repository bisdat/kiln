#!/usr/bin/env bash
# Regenerates managed/Kiln.Analyzers/src/Kiln.Analyzers/KilnApis.g.cs (the analyzers' tick-thread, advanced,
# completed-task, capability-guard and service tables) from Kiln's managed sources with tools/ApiScan (Roslyn). Run after changing Kiln.Api, Kiln.Testing
# or a service, and commit the result:
#   KILN_OUT=<build dir> managed/Kiln.Analyzers/tools/gen-apis.sh
# --check regenerates into a temporary file and compares it with the committed one (tools/check.sh's apis stage; 31.13
# shipped a stale table): exit 0 current, 1 stale (the first lines of the diff and the command that regenerates it),
# 2 the scan could not run. Nothing in the repository is written.
#   KILN_OUT=<build dir> managed/Kiln.Analyzers/tools/gen-apis.sh --check
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../../.." && pwd)
check=0
for a in "$@"; do
  case $a in
    --check) check=1 ;;
    *) echo "gen-apis: unknown argument $a (usage: gen-apis.sh [--check])" >&2; exit 2 ;;
  esac
done
committed=$repo/managed/Kiln.Analyzers/src/Kiln.Analyzers/KilnApis.g.cs
target=$committed
out=${KILN_OUT:-$repo/.out/analyzers}
stage=$out/apiscan-stage
rm -rf "$stage" && mkdir -p "$stage"
build() { dotnet build "$1" -c Release -o "$stage" --artifacts-path "$out/dotnet" -v q -nologo >/dev/null; }
for proj in "$repo/managed/Kiln.Api/Kiln.Api.csproj" "$repo/managed/Kiln.Testing/Kiln.Testing.csproj"; do
  build "$proj" || { echo "gen-apis: $(basename "$proj" .csproj) failed to build" >&2; exit 2; }
done
for proj in "$repo"/managed/services/*/*.csproj; do
  build "$proj" || echo "gen-apis: WARNING: $(basename "$proj" .csproj) failed to build; scanning it against the others" >&2
done
dotnet_root=$(dirname "$(readlink -f "$(command -v dotnet)")")
refs=$( (ls -d "$dotnet_root"/packs/Microsoft.NETCore.App.Ref/10.*/ref/net10.0 \
              "$HOME"/.dotnet/packs/Microsoft.NETCore.App.Ref/10.*/ref/net10.0 2>/dev/null || true) | sort -V | tail -1)
[[ -d $refs ]] || { echo "gen-apis: no Microsoft.NETCore.App.Ref 10.x reference pack found" >&2; exit 2; }
if ((check)); then
  mkdir -p "$out"
  target=$(mktemp "$out/KilnApis.check.XXXXXX")
  trap 'rm -f "$target"' EXIT
fi
dotnet run --project "$here/ApiScan/ApiScan.csproj" -c Release --artifacts-path "$out/dotnet" -- \
  --repo "$repo" --stage "$stage" --refs "$refs" --out "$target" || { echo "gen-apis: ApiScan failed" >&2; exit 2; }
if ((check)); then
  [[ -s $target ]] || { echo "gen-apis: the scan wrote nothing" >&2; exit 2; }
  if diff -q "$committed" "$target" >/dev/null; then
    echo "gen-apis: KilnApis.g.cs is current"
    exit 0
  fi
  echo "gen-apis: KilnApis.g.cs is STALE: the managed sources changed the analyzers' API tables. Regenerate and commit it:"
  echo "  KILN_OUT=<build dir> managed/Kiln.Analyzers/tools/gen-apis.sh"
  diff -u --label committed --label regenerated "$committed" "$target" | head -60 || true
  exit 1
fi
