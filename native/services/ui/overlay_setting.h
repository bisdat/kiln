// SPDX-License-Identifier: MIT
// kiln.ui: the player's switch for the overlay's drawing in kiln.json (overlay_guard.cpp reads it at start; host test
// tests/overlay_mode_test.cpp). No windows.h.
//
//   "ui": { "overlay": "off" }   the overlay's drawing is off, as with KILN_UI_OVERLAY=0: key bindings, the Mods entry,
//                                mods' native menu rows and kiln.ui.tick keep working (docs/kiln/ui.md)
//   "ui": { "overlay": "auto" }  Kiln decides (the default): on, unless Streamline, OptiScaler or the crash guard says off
//   "ui": { "overlay": "on" }    (0.2.0-dev.31.25) the drawing on whatever Kiln would decide, as with KILN_UI_OVERLAY=1:
//                                next to NVIDIA Streamline and OptiScaler, and after a start the crash guard counted
//                                (overlay_guard.cpp: the attempt is recorded, a death in it counted, and the next start
//                                with the setting still on tries again). The player's choice, at their own risk.
//
// Steam's launch options on Windows cannot set an environment variable for the game, so KILN_UI_OVERLAY is out of a
// Windows player's reach; the launcher writes this one (Settings, Advanced, Kiln's file: "Kiln's overlay"; Settings,
// Graphics and the Play page: "Draw Kiln's overlay anyway" next to Streamline or OptiScaler). KILN_UI_OVERLAY, where it
// is set (0 or 1), wins.
#pragma once
#include <string>
#include <string_view>

#include "nlohmann/json.hpp"

namespace kui {

// kiln.json's text -> choose_overlay_mode's `setting`: 0 for "off", 1 for "on", -1 (Kiln decides) for "auto", for
// anything else (the values are exact: "ON", true and 1 are not "on"), and for a file, section or key that is missing or
// not readable. Comments allowed, as the core reads kiln.json.
inline int overlay_setting(std::string_view kiln_json) {
    try {
        const auto doc = nlohmann::json::parse(kiln_json.begin(), kiln_json.end(), nullptr, false, true);
        if (!doc.is_object()) return -1;
        const auto ui = doc.find("ui");
        if (ui == doc.end() || !ui->is_object()) return -1;
        const auto v = ui->find("overlay");
        if (v == ui->end() || !v->is_string()) return -1;
        const auto& value = v->get_ref<const std::string&>();
        return value == "off" ? 0 : value == "on" ? 1 : -1;
    } catch (...) {
        return -1;
    }
}

}  // namespace kui
