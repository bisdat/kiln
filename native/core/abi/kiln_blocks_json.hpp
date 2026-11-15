// SPDX-License-Identifier: MIT
// The JSON of Kiln's blocks files as every reader of them takes it (Kiln.Blocks' BlockJson, kiln.worldtables, kiln.vfx and, since the
// 0.2.0-dev.28 fix round, kiln.content's resource patches): JSON with comments, trailing commas, and at most 64 nested containers.
// Header-only, C++17, nlohmann::json (or ordered_json). The same rules as native/services/worldtables/merge.cpp parse_relaxed.
#pragma once
#include <string>

#include "nlohmann/json.hpp"

namespace kiln::blocks_json {

// A discarded value when the text is not such JSON (an unclosed comment, deeper than 64, a comma with no value before it).
template <class Json = nlohmann::json>
inline Json parse_relaxed(const std::string& text) {
    // Match BlockJson: JSON comments, trailing commas and at most 64 nested containers. Strip comments first so a comma can see a closing token
    // across them, while braces and quotes in comments never affect strings or the depth guard. Whitespace keeps tokens from joining together.
    std::string clean = text;
    bool quoted = false, escaped = false;
    unsigned depth = 0;
    for (size_t i = 0; i < clean.size(); ++i) {
        const char c = clean[i];
        if (quoted) {
            if (escaped) escaped = false;
            else if (c == '\\') escaped = true;
            else if (c == '"') quoted = false;
            continue;
        }
        if (c == '"') { quoted = true; continue; }
        if (c == '/' && i + 1 < clean.size() && (clean[i + 1] == '/' || clean[i + 1] == '*')) {
            const bool line = clean[i + 1] == '/';
            clean[i++] = ' ';
            clean[i] = ' ';
            bool closed = line;
            while (++i < clean.size()) {
                if (line && (clean[i] == '\n' || clean[i] == '\r')) break;
                if (!line && clean[i] == '*' && i + 1 < clean.size() && clean[i + 1] == '/') {
                    clean[i++] = ' ';
                    clean[i] = ' ';
                    closed = true;
                    break;
                }
                if (clean[i] != '\n' && clean[i] != '\r') clean[i] = ' ';
            }
            if (!closed) return Json(Json::value_t::discarded);
            continue;
        }
        if (c == '[' || c == '{') {
            if (++depth > 64) return Json(Json::value_t::discarded);
        } else if ((c == ']' || c == '}') && depth) --depth;
    }
    quoted = escaped = false;
    char previous = 0;
    for (size_t i = 0; i < clean.size(); ++i) {
        const char c = clean[i];
        if (quoted) {
            if (escaped) escaped = false;
            else if (c == '\\') escaped = true;
            else if (c == '"') quoted = false;
        } else if (c == '"') quoted = true;
        else if (c == ',') {
            const size_t next = clean.find_first_not_of(" \t\r\n", i + 1);
            // A real value must precede it: never turn [,], {,} or a double comma into valid JSON.
            const bool value = previous == '"' || previous == ']' || previous == '}' || (previous >= '0' && previous <= '9') || previous == 'e' || previous == 'l';
            if (value && next != std::string::npos && (clean[next] == ']' || clean[next] == '}')) clean[i] = ' ';
        }
        if (c != ' ' && c != '\t' && c != '\r' && c != '\n') previous = c;
    }
    return Json::parse(clean, nullptr, false);
}

}  // namespace kiln::blocks_json
