// SPDX-License-Identifier: MIT
// Greeneye Loader's own loader for compat/own_off.h (a profile mod that is off, while Greeneye Loader runs): it reads
// <game>/greeneye/<folder>/mod.json with its lenient reader (greeneye_json.h) and runs the mod unless mod.json says
// "enabled": false, "side" leaves out this side (default "client"), "api" is above its API or the DLL is missing
// (docs/kiln/engines/greeneye.md "Which mods load"). The mod's id is mod.json's "id", else the folder's name.
// Header only and free of Windows calls (compat/tests/own_off_test.cpp).
#pragma once
#include <string>

#include "../../compat/own_off.h"
#include "greeneye_json.h"

namespace kgreeneye {

inline kcompat::own_off::Loader greeneye_own_loader(const std::filesystem::path& game, bool server, int api_max) {
    namespace oo = kcompat::own_off;
    oo::Loader l;
    l.name = "Greeneye Loader";
    l.kind = "greeneye";
    l.folder = game / "greeneye";
    l.folder_text = "greeneye";
    l.own_text = "Greeneye Loader";
    l.read = [server, api_max](const std::filesystem::path& e, bool dir) -> std::optional<oo::Seen> {
        std::string json;
        if (!dir || !oo::read_text(e / "mod.json", &json)) return std::nullopt;
        const std::string folder = oo::utf8(e.filename());
        const std::string id = JsonString(json, "id", folder);
        const std::string side = JsonString(json, "side", "client");
        const int api = JsonInt(json, "api", 1);
        const std::string dll = JsonString(json, "dll", id + ".dll");
        oo::Seen s;
        s.keys = {oo::low(id)};
        s.turn_off = "set \"enabled\": false in <game>\\greeneye\\" + folder + "\\mod.json (Greeneye Loader's own switch)";
        std::error_code ec;
        if (!JsonBool(json, "enabled", true)) {
            s.runs = false;
            s.why_not = "its mod.json says \"enabled\": false";
        } else if (side != "both" && side != (server ? "server" : "client")) {
            s.runs = false;
            s.why_not = "its mod.json says \"side\": \"" + side + "\"";
        } else if (api > api_max) {
            s.runs = false;
            s.why_not = "it needs Greeneye API " + std::to_string(api) + ", above " + std::to_string(api_max);
        } else if (!std::filesystem::is_regular_file(e / oo::u8path(dll), ec)) {
            s.runs = false;
            s.why_not = "its DLL " + dll + " is missing";
        } else {
            s.on_text = "Greeneye Loader's own switch is on: its mod.json has no \"enabled\": false";
        }
        return s;
    };
    return l;
}

}  // namespace kgreeneye
