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
// [sf] line as info. Such a line's tag alone gives its level: a message may quote "[E]", ERROR or WARN (a mod's own
// log line, a setting's name) without making an info line an error. A line without that tag (an older or a
// hand-made log, the bootstrap's own lines) keeps the older rule: "[E]" or "[W]" anywhere, or ERROR or WARN as a word.
inline int32_t sf_line_level(std::string_view line) {
    if (line.size() >= 4 && line[0] == '[' && line[2] == ' ' && line[3] >= '0' && line[3] <= '9') {
        switch (line[1]) {
            case 'E': return KILN_LOG_ERROR;
            case 'W': return KILN_LOG_WARN;
            case 'I': case 'D': case 'T': return KILN_LOG_INFO;
            default: break;
        }
    }
    if (line.find("[E]") != std::string_view::npos || line.find(" ERROR ") != std::string_view::npos) return KILN_LOG_ERROR;
    if (line.find("[W]") != std::string_view::npos || line.find(" WARN ") != std::string_view::npos) return KILN_LOG_WARN;
    return KILN_LOG_INFO;
}

}  // namespace ksf
