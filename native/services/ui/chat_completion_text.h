// SPDX-License-Identifier: MIT
// Safe native UTF-8 value snapshots for completion. Do not reuse History's
// non-empty-entry restriction when reading an editor being cleared.
#pragma once

#include "chat_history.h"
#include <cstdint>
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

// The game's caller-owned UTF-8 StringHead can be empty/stale before
// TextInputProcess has copied the native editor into it. A completion was
// requested from the *post-call, validated editor snapshot*, not that
// pre-call StringHead. Refuse stale revisions, focus loss and edits, while
// allowing the readback StringHead to be empty.
inline bool can_apply_editor_snapshot(
    uint32_t live_field, uint32_t live_revision, std::string_view live_text,
    uint32_t live_cursor, uint32_t expected_field, uint32_t expected_revision,
    std::string_view expected_text, bool focused, bool editing,
    bool user_edited, bool composing) {
    if (!focused || !editing || user_edited || composing ||
        live_field != expected_field || live_revision != expected_revision ||
        live_text != expected_text || !valid(expected_text))
        return false;
    uint32_t codepoints = 0;
    for (const unsigned char c : expected_text)
        if ((c & 0xc0u) != 0x80u) ++codepoints;
    return live_cursor == codepoints;
}

} // namespace kui::chat_completion_text
