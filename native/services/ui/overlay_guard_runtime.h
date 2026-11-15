#pragma once
#include "overlay_guard.h"
namespace kui {
bool overlay_guard_start() noexcept; // choose mode and prepare guard; true: drawing off, keep non-Vulkan services
void overlay_guard_stage(overlay_guard::Stage stage) noexcept;
// Right before the overlay's first draw in this process (render thread, once): arms the record again when the start
// was already counted as fine without a frame (overlay_guard::rearm).
void overlay_guard_first_draw() noexcept;
// The process exits normally with code 0 (kiln.ui's on_unload): a start still trying did not die at the overlay.
void overlay_guard_exit() noexcept;
}
