# source this in a Wine test script so it runs under the Wine the game itself runs under.
# GE-Proton11's Wine 11.0 refused what PATH's Wine 11.20 allowed (listing a held directory, the first kiln-mode start,
# 2026-11-20), so a test that passes only under PATH's Wine proves little. Order: KILN_WINE (a wine binary; its
# wineserver beside it), else the newest GE-Proton's own Wine in Steam's compatibilitytools.d, else PATH's Wine.
# KILN_WINE_SYSTEM=1 keeps PATH's. Exports KILN_WINE_USED (the chosen wine) and puts its bin folder first on PATH.
_kiln_wine_pick() {
  [[ ${KILN_WINE_SYSTEM:-0} == 1 ]] && return 0
  local wine=${KILN_WINE:-}
  if [[ -z $wine ]]; then
    local d best="" bestv=""
    for d in "$HOME"/.local/share/Steam/compatibilitytools.d/GE-Proton*/ "$HOME"/.steam/root/compatibilitytools.d/GE-Proton*/; do
      [[ -x $d/files/bin/wine ]] || continue
      local v; v=$(basename "$d"); v=${v#GE-Proton}
      if [[ -z $bestv ]] || [[ $(printf '%s\n%s\n' "$bestv" "$v" | sort -V | tail -1) == "$v" ]]; then best=$d; bestv=$v; fi
    done
    [[ -n $best ]] && wine=$best/files/bin/wine
  fi
  [[ -n $wine && -x $wine ]] || return 0
  PATH="$(dirname "$wine"):$PATH"; export PATH
}
_kiln_wine_pick
KILN_WINE_USED=$(command -v wine || true); export KILN_WINE_USED
echo "wine: ${KILN_WINE_USED:-none} ($( { wine --version 2>/dev/null || true; } | head -1))" >&2
