// SPDX-License-Identifier: MIT
#include "text.h"

namespace kmk {

bool valid_identifier(const char* p, size_t n) {
    if (!p || n == 0 || n > kIdentifierMax) return false;
    for (size_t i = 0; i < n; ++i) {
        const unsigned char c = static_cast<unsigned char>(p[i]);
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '.' ||
                        c == ':' || c == '-';
        if (!ok) return false;
    }
    return true;
}

const char* text_problem(const char* p, size_t n, size_t max) {
    if (n > max) return "text is longer than 128 bytes of UTF-8";
    if (n && !p) return "text has a length but no bytes";
    size_t i = 0;
    while (i < n) {
        const unsigned char c = static_cast<unsigned char>(p[i]);
        uint32_t cp = 0;
        size_t len = 0;
        if (c < 0x80) {
            cp = c;
            len = 1;
        } else if (c >= 0xc2 && c <= 0xdf) {
            cp = c & 0x1f;
            len = 2;
        } else if (c >= 0xe0 && c <= 0xef) {
            cp = c & 0x0f;
            len = 3;
        } else if (c >= 0xf0 && c <= 0xf4) {
            cp = c & 0x07;
            len = 4;
        } else {
            return "text is not UTF-8";
        }
        if (i + len > n) return "text is not UTF-8 (cut in a character)";
        for (size_t k = 1; k < len; ++k) {
            const unsigned char cc = static_cast<unsigned char>(p[i + k]);
            if ((cc & 0xc0) != 0x80) return "text is not UTF-8";
            cp = (cp << 6) | (cc & 0x3f);
        }
        if ((len == 3 && cp < 0x800) || (len == 4 && cp < 0x10000)) return "text is not UTF-8 (overlong form)";
        if (cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff)) return "text is not UTF-8 (no such code point)";
        if (cp <= 0x1f || (cp >= 0x7f && cp <= 0x9f)) return "text has a control character (one line, no U+0000-001F or U+007F-009F)";
        i += len;
    }
    return nullptr;
}

}  // namespace kmk
