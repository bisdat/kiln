#include <initializer_list>
#include "../overlay_mode.h"
#include "../overlay_setting.h"
#include <cassert>
#include <cstdio>
using namespace kui;
int main() {
    unsigned count = 0;
    for (bool disabled : {false, true}) for (bool guard : {false, true})
    for (bool sl : {false, true}) for (int env : {-1, 0, 1}) for (int setting : {-1, 0, 1}) {
        const int override = env == -1 ? setting : env;
        const auto expected = disabled ? OverlayMode::disabled : override == 1 ? OverlayMode::on :
            override == 0 || guard || sl ? OverlayMode::drawing_off : OverlayMode::on;
        ++count;
        assert(choose_overlay_mode(disabled, guard, sl, env, setting) == expected);
    }
    // kiln.json "ui": {"overlay": "off"} (overlay_setting.h): the setting choose_overlay_mode gets
    unsigned settings = 0;
    auto setting = [&](const char* text, int want) { ++settings; assert(overlay_setting(text) == want); };
    setting(R"({"ui": {"overlay": "off"}})", 0);
    setting("// the player's kiln.json\n{\"policy\": \"verified-only\", /* Kiln's look */ \"ui\": {\"look\": \"game\", \"overlay\": \"off\"}}", 0);
    setting(R"({"ui": {"overlay": "auto"}})", -1);
    setting(R"({"ui": {"overlay": "on"}})", -1);  // no force-on from the file: KILN_UI_OVERLAY=1 only
    setting(R"({"ui": {"overlay": "OFF"}})", -1);
    setting(R"({"ui": {"overlay": false}})", -1);
    setting(R"({"ui": {"chatHistory": true}})", -1);
    setting(R"({"overlay": "off"})", -1);  // top level: not the ui section
    setting(R"({"ui": "off"})", -1);
    setting(R"({"ui": {"overlay": "off"})", -1);  // broken file: Kiln decides
    setting("", -1);
    setting("[]", -1);
    // the setting off, KILN_UI_OVERLAY unset: drawing off; KILN_UI_OVERLAY wins either way
    assert(choose_overlay_mode(false, false, false, -1, overlay_setting(R"({"ui": {"overlay": "off"}})")) == OverlayMode::drawing_off);
    assert(choose_overlay_mode(false, false, false, 1, 0) == OverlayMode::on);
    assert(choose_overlay_mode(false, false, false, -1, overlay_setting(R"({"ui": {"overlay": "auto"}})")) == OverlayMode::on);
    std::printf("overlay_mode: %u combinations passed, %u kiln.json settings\n", count, settings);
}
