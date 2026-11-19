#pragma once
#include "overlay_guard.h"
namespace kui {
bool overlay_guard_start() noexcept; // choose mode and prepare guard; true: drawing off, keep non-Vulkan services
void overlay_guard_stage(overlay_guard::Stage stage) noexcept;
// Right before each overlay draw attempt (render thread; after the first, one atomic read): the first attempt of the
// process stops the idle timer from completing the start, and arms the record again when the timer already counted it
// as fine (overlay_guard::rearm).
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
// retry (overlay_guard::at_exit).
void overlay_guard_exit() noexcept;
// What overlay_guard_start found next to the game: OptiScaler (by its DLL's contents) and NVIDIA Streamline
// (sl.interposer.dll in the game folder, Streamline/ or loaded). Kiln's graphics tools step aside for both (graphics.cpp).
void overlay_guard_tools(bool* optiscaler, bool* streamline) noexcept;
}
