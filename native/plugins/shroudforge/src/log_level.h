// SPDX-License-Identifier: MIT
// kiln-shroudforge: the level of one line of ShroudForge's log in its mirror into kiln.log ("[sf] ..." lines). No
// Windows code, so the host test (tests/test-log-level.sh) builds it on Linux.
#pragma once
#include <cstdint>
#include <string_view>

#include "kiln_api.h"

namespace ksf {

// ShroudForge writes each line as "[<L> <yyyy-mm-dd hh:mm:ss,mmm> UTC] [<source>] <message>" (modules/logging
// format_log_line.rs: L is E, W, I, D or T; a message's own line breaks are written as \n, so a line is one entry).
// Kiln's log mirror once looked only for "[E]" and "[W]", which ShroudForge's lines never hold, and logged every
// [sf] line as info. Error and warning lines keep their level; so do "[E] ..." and "[W] ..." anywhere in a line (an older
// or a hand-made log) and a line that names ERROR or WARN as a word.
inline int32_t sf_line_level(std::string_view line) {
    const auto tag = [&](char c) { return line.size() >= 3 && line[0] == '[' && line[1] == c && line[2] == ' '; };
    if (tag('E') || line.find("[E]") != std::string_view::npos || line.find(" ERROR ") != std::string_view::npos)
        return KILN_LOG_ERROR;
    if (tag('W') || line.find("[W]") != std::string_view::npos || line.find(" WARN ") != std::string_view::npos)
        return KILN_LOG_WARN;
    return KILN_LOG_INFO;
}

}  // namespace ksf
