#!/usr/bin/env bash
# Host tests of projection, the radial wheel's geometry and rules, the main-menu page, the render service, callback
# lifetime and signature guards: no game, GPU or Wine.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
: "${KILN_OUT:=$here/../../../build}"
export KILN_OUT
out=$KILN_OUT/ui-host-test
mkdir -p "$out"
bash "$here/run-window-constraints.sh"
# the overlay's image budget: the descriptor pool holds every image Kiln lets exist, uploads spread over frames (image_budget.h)
g++ -std=c++20 -O1 -g -Wall -Wextra -fsanitize=address,undefined "$here/image_budget_test.cpp" -o "$out/image_budget_test"
"$out/image_budget_test" "$here/../vk.cpp"
g++ -std=c++20 -O1 -g -Wall -Wextra -fsanitize=address,undefined "$here/render_math_test.cpp" -o "$out/render_math_test"
g++ -std=c++20 -O1 -g -Wall -Wextra -fsanitize=address,undefined "$here/wheel_math_test.cpp" -o "$out/wheel_math_test"
"$out/wheel_math_test"
"$out/render_math_test"
g++ -std=c++20 -O1 -g -Wall -Wextra -fsanitize=address,undefined "$here/overlay_guard_test.cpp" -o "$out/overlay_guard_test"
"$out/overlay_guard_test"
g++ -std=c++20 -O1 -g -Wall -Wextra -fsanitize=address,undefined "$here/overlay_mode_test.cpp" -o "$out/overlay_mode_test"
"$out/overlay_mode_test"
# which renderer call is the main menu's Settings entry (another module wrapping the game's call): plain logic
g++ -std=c++20 -O1 -g -Wall -Wextra -fsanitize=address,undefined "$here/menu_site_test.cpp" -o "$out/menu_site_test"
"$out/menu_site_test"
# which of Kiln's rows a game menu draws (mods' rows past a few would push Quit off the screen): plain logic
g++ -std=c++20 -O1 -g -Wall -Wextra -fsanitize=address,undefined -I"$here/../../../core/abi" "$here/menu_rows_test.cpp" -o "$out/menu_rows_test"
"$out/menu_rows_test"
# the scrolling pause menu's rules: begin/end pairing, when it scrolls, the crash guard (menu_scroll.h)
g++ -std=c++20 -O1 -g -Wall -Wextra -fsanitize=address,undefined -I"$here/../../../core/abi" "$here/menu_scroll_test.cpp" -o "$out/menu_scroll_test"
"$out/menu_scroll_test"
bash "$here/run-menu-scroll-host.sh"
# the middle and side mouse buttons and the wheel as key presses: twins of raw records and messages, notches (mouse_press.h)
g++ -std=c++20 -O1 -g -Wall -Wextra -fsanitize=address,undefined "$here/mouse_press_test.cpp" -o "$out/mouse_press_test"
"$out/mouse_press_test"
# one key press counts once, also across a focus loss (key_hold.h, GitHub issue 39): plain logic
g++ -std=c++20 -O1 -g -Wall -Wextra -fsanitize=address,undefined "$here/key_hold_test.cpp" -o "$out/key_hold_test"
"$out/key_hold_test"
# Repeat-window configuration on the render thread alongside key events on the window thread.
g++ -std=c++20 -O1 -g -Wall -Wextra -pthread -fsanitize=thread "$here/key_hold_thread_test.cpp" -o "$out/key_hold_thread_test"
"$out/key_hold_thread_test"
# key names and which key a binding means on any layout (key_names.h, GitHub issue 33): plain logic
g++ -std=c++20 -O1 -g -Wall -Wextra -fsanitize=address,undefined "$here/key_names_test.cpp" -o "$out/key_names_test"
"$out/key_names_test"
# the window procedure chain (another mod subclassing the game window after Kiln): fake window functions
g++ -std=c++20 -O1 -g -Wall -Wextra -fsanitize=address,undefined -I"$here/chain_host" "$here/wndproc_chain_test.cpp" -o "$out/wndproc_chain_test"
"$out/wndproc_chain_test"
g++ -std=c++20 -O1 -g -Wall -Wextra -pthread -fsanitize=address,undefined \
    -I"$here/stubs" -I"$here/../../../core/abi" "$here/menu_page_host_test.cpp" -o "$out/menu_page_host_test"
"$out/menu_page_host_test"
# Kiln's line in the main menu's footer: its text, its mark's place, when Kiln's copy runs (menu_footer.h)
g++ -std=c++20 -O1 -g -Wall -Wextra -fsanitize=address,undefined "$here/menu_footer_test.cpp" -o "$out/menu_footer_test"
"$out/menu_footer_test"
# the footer function's copy keeps its meaning (code_clone.h, Zydis from safetyhook); also over the game's own footer
# function when the client exe is installed (read only)
safetyhook=$here/../../../third_party/safetyhook
[[ $out/zydis.o -nt $safetyhook/Zydis.c ]] || gcc -O1 -g -w -I"$safetyhook" -c "$safetyhook/Zydis.c" -o "$out/zydis.o"
g++ -std=c++20 -O1 -g -Wall -Wextra -fsanitize=address,undefined -I"$safetyhook" "$here/code_clone_test.cpp" "$out/zydis.o" \
    -o "$out/code_clone_test"
"$out/code_clone_test"

imgui=$here/../../../third_party/imgui
# Dear ImGui once, as objects, for both ImGui tests
for f in imgui imgui_draw imgui_tables imgui_widgets; do
    [[ $out/$f.o -nt $imgui/$f.cpp ]] || g++ -std=c++20 -O1 -g -w -fsanitize=address,undefined -I"$imgui" -c "$imgui/$f.cpp" -o "$out/$f.o" &
done
wait
objs=("$out/imgui.o" "$out/imgui_draw.o" "$out/imgui_tables.o" "$out/imgui_widgets.o")
g++ -std=c++20 -O1 -g -Wall -Wextra -pthread -fsanitize=address,undefined \
    -I"$here/stubs" -I"$here/../../../core/abi" -I"$imgui" \
    "$here/render_host_test.cpp" "${objs[@]}" -o "$out/render_host_test"
"$out/render_host_test"
g++ -std=c++20 -O1 -g -Wall -Wextra -pthread -fsanitize=address,undefined \
    -I"$here/stubs" -I"$here/../../../core/abi" -I"$imgui" \
    "$here/wheel_host_test.cpp" "${objs[@]}" -o "$out/wheel_host_test"
"$out/wheel_host_test"
# the looks: the game's colours through reflection, the player's look, themes, mod fonts (style.cpp, look_palette.h, font_budget.h)
g++ -std=c++20 -O1 -g -Wall -Wextra -Wno-unused-function -pthread -fsanitize=address,undefined \
    -I"$here/stubs" -I"$here/.." -I"$here/../../../core/abi" -I"$imgui" -I"$here/../../../third_party" \
    "$here/style_host_test.cpp" "${objs[@]}" -o "$out/style_host_test"
"$out/style_host_test"
PYTHONDONTWRITEBYTECODE=1 python3 "$here/render_sigdb_test.py"
PYTHONDONTWRITEBYTECODE=1 python3 "$here/per_caller_test.py"
