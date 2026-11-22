// SPDX-License-Identifier: MIT
// kiln.markers' text rules (chapter 04 §4.1, §4.5): identifiers (namespaces and keys) and marker text. Pure.
#ifndef KILN_MARKERS_TEXT_H
#define KILN_MARKERS_TEXT_H
#include <cstddef>
#include <cstdint>

namespace kmk {

constexpr size_t kIdentifierMax = 64;  // namespace and key: 1 to 64 bytes
constexpr size_t kTextMax = 128;       // marker text: at most 128 bytes of UTF-8

// 1 to 64 bytes of [A-Za-z0-9_.:-].
bool valid_identifier(const char* p, size_t n);

// Strict UTF-8 of at most `max` bytes (no overlong forms, no surrogates, nothing above U+10FFFF) with no code point in
// U+0000-001F or U+007F-009F. nullptr when it is good, else the reason in words (a constant).
const char* text_problem(const char* p, size_t n, size_t max);

}  // namespace kmk
#endif
