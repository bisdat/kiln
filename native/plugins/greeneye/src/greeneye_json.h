// SPDX-License-Identifier: MIT
// kiln-greeneye: mod.json's flat fields as Greeneye Loader reads them. Header only and free of Windows calls, so the host
// test of own_off_greeneye.h reads mod.json with the very reader the plugin uses.
#pragma once
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>

namespace kgreeneye {

// --- mod.json: the flat fields Greeneye Loader reads, read the way it reads them (a lenient key search, so a file
// it accepts is accepted here too). Ported from Greeneye Loader src/main.cpp (MIT).
inline const char* FindKey(const std::string& json, const char* key) {
    std::string quoted = std::string("\"") + key + "\"";
    size_t at = json.find(quoted);
    if (at == std::string::npos) return nullptr;
    at = json.find(':', at + quoted.size());
    if (at == std::string::npos) return nullptr;
    const char* p = json.c_str() + at + 1;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') ++p;
    return p;
}

inline std::string JsonString(const std::string& json, const char* key, const std::string& fallback) {
    const char* p = FindKey(json, key);
    if (!p || *p != '"') return fallback;
    std::string out;
    for (++p; *p && *p != '"'; ++p) {
        if (*p == '\\' && p[1]) ++p;
        out += *p;
    }
    return out;
}

inline bool JsonBool(const std::string& json, const char* key, bool fallback) {
    const char* p = FindKey(json, key);
    if (!p) return fallback;
    if (std::strncmp(p, "true", 4) == 0) return true;
    if (std::strncmp(p, "false", 5) == 0) return false;
    return fallback;
}

inline int JsonInt(const std::string& json, const char* key, int fallback) {
    const char* p = FindKey(json, key);
    if (!p || (*p != '-' && (*p < '0' || *p > '9'))) return fallback;
    errno = 0;
    const long long value = std::strtoll(p, nullptr, 10);
    if (errno == ERANGE) return *p == '-' ? (std::numeric_limits<int>::min)() : (std::numeric_limits<int>::max)();
    if (value > (std::numeric_limits<int>::max)()) return (std::numeric_limits<int>::max)();
    if (value < (std::numeric_limits<int>::min)()) return (std::numeric_limits<int>::min)();
    return static_cast<int>(value);
}

}  // namespace kgreeneye
