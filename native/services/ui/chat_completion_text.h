// SPDX-License-Identifier: MIT
// Safe native UTF-8 value snapshots for completion. Do not reuse History's
// non-empty-entry restriction when reading an editor being cleared.
#pragma once

#include "chat_history.h"
#include <string_view>

namespace kui::chat_completion_text {

inline bool valid(std::string_view bytes) {
    if (bytes.size() > 256) return false;
    // Empty/space-only fields are legitimate intermediate edit snapshots.
    // Capturing them is essential to clear old Tab suggestion state.
    if (bytes.empty() || bytes.find_first_not_of(' ') == std::string_view::npos)
        return true;
    // Strict UTF-8 (no overlong, surrogate, or out-of-range codepoints) and
    // no control bytes, including tabs or embedded newlines.
    return chat_history::History::valid(bytes);
}

} // namespace kui::chat_completion_text
