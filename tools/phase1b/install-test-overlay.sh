#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Reversible Phase 1b test overlay. This is not a Kiln runtime bundle.
set -euo pipefail

usage() {
  echo "Usage: bash install-test-overlay.sh <Enshrouded game folder> [install|restore]" >&2
  echo "Stop the game first. Use the folder containing enshrouded.exe." >&2
  exit 2
}

(($# >= 1 && $# <= 2)) || usage
game=$(realpath -- "${1}")
mode=${2:-install}
[[ "$mode" == install || "$mode" == restore ]] || usage
[[ -f "$game/enshrouded.exe" || -f "$game/Enshrouded.exe" ]] || {
  echo "Refusing: not an Enshrouded client game folder: $game" >&2
  exit 2
}
[[ -d "$game/kiln" ]] || { echo "Refusing: Kiln is not installed at $game" >&2; exit 2; }
if command -v pgrep >/dev/null && pgrep -fi 'enshrouded[.]exe' >/dev/null; then
  echo "Refusing: Enshrouded appears to be running. Exit it before replacing DLLs." >&2
  exit 2
fi

base=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
backup="$game/kiln/data/phase1b-test-backup"
files=(
  "kiln/services/kiln-ui.dll"
  "kiln/host/Kiln.Chat.dll"
  "kiln/host/Kiln.Ui.dll"
)

for path in "${files[@]}"; do
  [[ -f "$game/$path" ]] || { echo "Refusing: installed file missing: $game/$path" >&2; exit 2; }
  if [[ "$mode" == install ]]; then
    [[ -f "$base/$path" ]] || { echo "Refusing: overlay file missing: $base/$path" >&2; exit 2; }
  fi
done

if [[ "$mode" == install ]]; then
  [[ ! -e "$backup" ]] || {
    echo "Existing Phase 1b backup: $backup" >&2
    echo "Restore before installing a different candidate (backups are preserved)." >&2
    exit 2
  }
  mkdir -p "$(dirname "$backup")"
  temp=$(mktemp -d "$game/kiln/data/.phase1b-backup.XXXXXXXX")
  trap 'if [[ -n "${temp:-}" && -d "$temp" ]]; then rm -rf -- "$temp"; fi' EXIT
  for path in "${files[@]}"; do
    mkdir -p "$temp/$(dirname "$path")"
    cp -p -- "$game/$path" "$temp/$path"
  done
  mv -- "$temp" "$backup"
  temp=""
  for path in "${files[@]}"; do
    staged="$game/$path.phase1b-new"
    cp -p -- "$base/$path" "$staged"
    mv -f -- "$staged" "$game/$path"
    echo "Installed: $path"
  done
  echo "Phase 1b native + managed overlay installed. Originals saved under: $backup"
  echo "Completion is still OFF until you enable ui.chatCompletion in kiln/kiln.json."
else
  [[ -d "$backup" ]] || { echo "No Phase 1b backup to restore: $backup" >&2; exit 2; }
  for path in "${files[@]}"; do
    [[ -f "$backup/$path" ]] || { echo "Incomplete backup: $backup/$path" >&2; exit 2; }
  done
  for path in "${files[@]}"; do
    staged="$game/$path.phase1b-restore"
    cp -p -- "$backup/$path" "$staged"
    mv -f -- "$staged" "$game/$path"
    echo "Restored: $path"
  done
  stamp=$(date -u +%Y%m%dT%H%M%SZ)
  archive="$backup.restored.$stamp"
  [[ ! -e "$archive" ]] || { echo "Restore succeeded, but backup remains at $backup" >&2; exit 0; }
  mv -- "$backup" "$archive"
  echo "Original binaries restored. Recovery copy kept: $archive"
  echo "Remove ui.chatCompletion from kiln/kiln.json (or set it to false) after testing."
fi
