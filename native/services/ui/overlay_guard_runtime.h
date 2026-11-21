#pragma once
#include "overlay_guard.h"
namespace kui {
bool overlay_guard_start() noexcept; // choose mode and prepare guard; true: drawing off, keep non-Vulkan services
void overlay_guard_stage(overlay_guard::Stage stage) noexcept;
// Right before the overlay's own work of each draw (vk.cpp draw(), render thread, g_mu held; after the first, one atomic
// read): the first of the process stops the idle timer from completing the start and moves a start still watched to the
// stage "the overlay's first draw", or arms the record again when the timer already counted it as fine
// (overlay_guard::rearm). A present Kiln does not draw on (no swapchain or device of its own seen, a queue it does not
// draw on) is no draw of the overlay: a start that dies with only those stays at "swapchain created", which never locks.
void overlay_guard_first_draw() noexcept;
// After each present with the overlay drawn (render thread; one atomic add): the start counts as fine once the overlay's
// frames have been presented for a while (kDrawnFrames of them, or the game went on presenting kDrawnProofMs after the
// first). A game that dies in its first drawn frames is an interrupted overlay start, as one that dies before them.
void overlay_guard_drawn() noexcept;
// The overlay's drawing turned off during this session because its own work failed (ui.cpp overlay_turned_off; once;
// not for a present it does not draw on): a game that ends before it has presented a while longer counts as an
// interrupted overlay start, so the next start leaves the drawing off; one that goes on presenting, or exits normally
// (overlay_guard_exit), does not.
void overlay_guard_failed() noexcept;
// The process exits normally with code 0 (kiln.ui's on_unload, one of Kiln's own exit steps: under Wine, and on native
// Windows since 0.2.0-dev.29.18): a start still trying did not die at the overlay, a guarded start's record turns to
// retry (overlay_guard::at_exit). Nothing writes the record after it.
void overlay_guard_exit() noexcept;
// Kiln's own Mods entry (game.cpp on_builtin, the main menu's page) or the menu key asks for Kiln's menu (UI or window
// thread). True: open it in the overlay; the ask is watched, and when no frame of the overlay is drawn and presented in
// about a second (longer while the overlay is still making a frame, up to 5 s), Kiln's menu falls back to the game's own
// Mods rows for the session (g_menu_fell_back, game_menu_fell_back: the entry's caption, as with the drawing off), logged
// once. False: the overlay's drawing is off (a switch, the crash guard, Streamline, OptiScaler, or off since), or Kiln's
// menu fell back and no frame of the overlay was presented since: the entry toggles its caption in the game's own menu.
// Never a menu that cannot be seen.
bool overlay_menu_ask() noexcept;
// What overlay_guard_start found next to the game: OptiScaler (by its DLL's contents) and NVIDIA Streamline
// (sl.interposer.dll in the game folder, Streamline/ or loaded). Kiln's graphics tools step aside for both (graphics.cpp).
void overlay_guard_tools(bool* optiscaler, bool* streamline) noexcept;
}
