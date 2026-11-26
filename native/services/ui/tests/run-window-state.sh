#!/usr/bin/env bash
# kiln.ui's window_state slot and the Dear ImGui behaviour Kiln.Ui's window docking relies on (GitHub issue 149):
# window_state_test.cpp on a headless ImGui context, ASan/UBSan.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
: "${KILN_OUT:=$here/../../../build}"
out=$KILN_OUT/ui-host-test
mkdir -p "$out"
imgui=$here/../../../third_party/imgui
objs=()
for f in imgui imgui_draw imgui_tables imgui_widgets; do
    [[ $out/$f.o -nt $imgui/$f.cpp ]] || g++ -std=c++20 -O1 -g -w -fsanitize=address,undefined -I"$imgui" -c "$imgui/$f.cpp" -o "$out/$f.o"
    objs+=("$out/$f.o")
done
g++ -std=c++20 -O1 -g -Wall -Wextra -Wno-unused-function -fsanitize=address,undefined -I"$here/../../../core/abi" -I"$imgui" \
    "$here/window_state_test.cpp" "${objs[@]}" -o "$out/window_state_test"
"$out/window_state_test"
