// SPDX-License-Identifier: MIT
// kiln.ui: the player's switch for the overlay's drawing in kiln.json (overlay_guard.cpp reads it at start; host test
// tests/overlay_mode_test.cpp). No windows.h.
//
//   "ui": { "overlay": "off" }   the overlay's drawing is off, as with KILN_UI_OVERLAY=0: key bindings, the Mods entry,
//                                mods' native menu rows and kiln.ui.tick keep working (docs/kiln/ui.md)
//   "ui": { "overlay": "auto" }  Kiln decides (the default): on, unless Streamline, OptiScaler or the crash guard says off
//
// Steam's launch options on Windows cannot set an environment variable for the game, so KILN_UI_OVERLAY=0 is out of a
// Windows player's reach; the launcher's Settings ("Kiln") writes this one. KILN_UI_OVERLAY, where it is set, wins.
#pragma once
#include <string>
#include <string_view>

#include "nlohmann/json.hpp"

namespace kui {

// kiln.json's text -> choose_overlay_mode's `setting`: 0 for "off", -1 (Kiln decides) for "auto", for anything else,
// and for a file, section or key that is missing or not readable. Comments allowed, as the core reads kiln.json.
inline int overlay_setting(std::string_view kiln_json) {
    try {
        const auto doc = nlohmann::json::parse(kiln_json.begin(), kiln_json.end(), nullptr, false, true);
        if (!doc.is_object()) return -1;
        const auto ui = doc.find("ui");
        if (ui == doc.end() || !ui->is_object()) return -1;
        const auto v = ui->find("overlay");
        return v != ui->end() && v->is_string() && v->get<std::string>() == "off" ? 0 : -1;
    } catch (...) {
        return -1;
    }
}

}  // namespace kui
