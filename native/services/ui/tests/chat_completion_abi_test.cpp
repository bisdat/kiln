// SPDX-License-Identifier: MIT
// The new internal slot must be appended; released Kiln UI calls must not move.
#include <cstddef>
#include <cstdint>
#include "services/kiln_ui.h"

static_assert(offsetof(KilnUiApi, set_texts) == 1008);
static_assert(offsetof(KilnUiApi, graphics_set) == 1016);
static_assert(offsetof(KilnUiApi, graphics_status) == 1024);
static_assert(offsetof(KilnUiApi, chat_completion) == 1032);
// subscribe_pointer_press (0.2.0-dev.30) is appended after it: the table is 1048 bytes
static_assert(offsetof(KilnUiApi, subscribe_pointer_press) == 1040);
static_assert(offsetof(KilnUiApi, chat_search) == 1048);
// pointer, drag_source and drop_target (0.2.0-dev.30.3) are appended after it: the table is 1080 bytes
static_assert(offsetof(KilnUiApi, pointer) == 1056);
static_assert(offsetof(KilnUiApi, drag_source) == 1064);
static_assert(offsetof(KilnUiApi, drop_target) == 1072);
static_assert(offsetof(KilnUiApi, popups) == 1080);
// window_state (issue 149) after it: the table is 1096 bytes
static_assert(offsetof(KilnUiApi, window_state) == 1088);
static_assert(sizeof(KilnUiApi) == 1096);
static_assert(sizeof(KilnUiPointer) == 24);
static_assert(offsetof(KilnUiChatSearch, query) == 64);
static_assert(offsetof(KilnUiChatSearch, lines) == 321);
static_assert(sizeof(KilnUiChatSearch) == 10572);
static_assert(offsetof(KilnUiChatCompletion, text) == 60);
static_assert(offsetof(KilnUiChatCompletion, replacement) == 317);
static_assert(sizeof(KilnUiChatCompletion) == 576);
int main() { return 0; }
