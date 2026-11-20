// SPDX-License-Identifier: MIT
// The new internal slot must be appended; released Kiln UI calls must not move.
#include <cstddef>
#include <cstdint>
#include "services/kiln_ui.h"

static_assert(offsetof(KilnUiApi, set_texts) == 1008);
static_assert(offsetof(KilnUiApi, graphics_set) == 1016);
static_assert(offsetof(KilnUiApi, graphics_status) == 1024);
static_assert(offsetof(KilnUiApi, chat_completion) == 1032);
static_assert(sizeof(KilnUiApi) == 1040);
static_assert(offsetof(KilnUiChatCompletion, text) == 60);
static_assert(offsetof(KilnUiChatCompletion, replacement) == 317);
static_assert(sizeof(KilnUiChatCompletion) == 576);
int main() { return 0; }
