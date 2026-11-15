#include <initializer_list>
#include "../overlay_mode.h"
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
    std::printf("overlay_mode: %u combinations passed\n", count);
}
