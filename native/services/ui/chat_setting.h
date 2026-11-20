// SPDX-License-Identifier: MIT
// kiln.ui: the player's chat switches in kiln.json (chat_input_probe.cpp reads them once at start; host test
// tests/overlay_mode_test.cpp). No windows.h.
//
//   "ui": { "chatHistory": true }      Up/Down history in the game's chat field (off by default)
//   "ui": { "chatCompletion": false }  Tab completion of Kiln commands (on by default since 0.2.0-dev.29.22)
//
// KILN_CHAT_HISTORY / KILN_CHAT_COMPLETION, where they are set, win (chat_input_probe_install).
#pragma once
#include <string_view>

#include "nlohmann/json.hpp"

namespace kui {

// kiln.json's text -> the switch `key` of its "ui" section: its value when it is a boolean; `fallback` (the setting's
// default) for a file, section or key that is missing, not readable or not a boolean. Comments allowed, as the core
// reads kiln.json.
inline bool chat_setting(std::string_view kiln_json, const char* key, bool fallback) {
    try {
        const auto doc = nlohmann::json::parse(kiln_json.begin(), kiln_json.end(), nullptr, false, true);
        if (!doc.is_object()) return fallback;
        const auto ui = doc.find("ui");
        if (ui == doc.end() || !ui->is_object()) return fallback;
        const auto v = ui->find(key);
        return v != ui->end() && v->is_boolean() ? v->get<bool>() : fallback;
    } catch (...) {
        return fallback;
    }
}

}  // namespace kui
