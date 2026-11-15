#pragma once
#include "overlay_guard.h"
namespace kui {
bool overlay_guard_start() noexcept; // true: same early unavailable path as KILN_UI_DISABLE
void overlay_guard_stage(overlay_guard::Stage stage) noexcept;
}
