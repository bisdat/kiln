// SPDX-License-Identifier: MIT
#pragma once
#include <string>
namespace kui {
enum class OverlayMode { on, drawing_off, disabled }; // future: device_only at the vk_install boundary
// -1 = unspecified/invalid, 0 = off, 1 = force on. Environment takes precedence.
inline OverlayMode choose_overlay_mode(bool disabled, bool guarded, bool streamline, int environment, int setting = -1) {
    if (disabled) return OverlayMode::disabled;
    const int option = environment == 0 || environment == 1 ? environment : setting;
    if (option == 1) return OverlayMode::on;
    return option == 0 || guarded || streamline ? OverlayMode::drawing_off : OverlayMode::on;
}
inline OverlayMode g_overlay_mode = OverlayMode::on; // fixed before publishing any service/hook
inline std::string g_overlay_reason; // stable after startup; also carried by status.error/capabilities
inline bool overlay_drawing_on() { return g_overlay_mode == OverlayMode::on; }
}
