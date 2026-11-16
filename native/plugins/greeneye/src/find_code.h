// SPDX-License-Identifier: MIT
// Included by hot.cpp and the host test, with TextSection, ReadOriginalCode, Read and jumppatch available.
uint8_t* g_text = nullptr;
size_t g_textSize = 0;
std::once_flag g_canonicalOnce;
std::vector<uint8_t> g_canonical; // .text as it was before our hooks (made on first find)
bool g_codeFromFile = false;
std::string g_codeNote;

std::vector<int> ParsePattern(const char* pattern)
{
    std::vector<int> bytes;
    if (!pattern) return bytes;
    auto hex = [](unsigned char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    for (const char* p = pattern; *p;) {
        if (std::isspace(static_cast<unsigned char>(*p))) { ++p; continue; }
        if (*p == '?') {
            bytes.push_back(-1);
            p += p[1] == '?' ? 2 : 1;
        } else {
            const int a = hex(static_cast<unsigned char>(p[0]));
            if (!p[1]) return {};
            const int b = hex(static_cast<unsigned char>(p[1]));
            if (a < 0 || b < 0) return {};
            bytes.push_back((a << 4) | b);
            p += 2;
        }
    }
    return bytes;
}

uint8_t* Find(const char* pattern, void* module)
{
    if (module && module != GetModuleHandleW(nullptr))
        return nullptr; // only the exe's code
    std::call_once(g_canonicalOnce, [] {
        if (!TextSection(g_text, g_textSize)) return;
        // Other engines have already patched the game by on_ready. A short signature may be entirely gone;
        // looking behind its jump cannot recover it. Match the same exe's relocated file bytes instead.
        g_codeFromFile = ReadOriginalCode(g_canonical, g_codeNote);
        if (!g_codeFromFile) {
            g_canonical.resize(g_textSize);
            Read(g_text, g_canonical.data(), g_textSize);
        }
    });
    std::vector<int> pat = ParsePattern(pattern);
    const uint8_t* text = g_canonical.data();
    const size_t size = g_canonical.size(), n = pat.size();
    if (!n || n > size || pat[0] < 0) return nullptr;
    const uint8_t first = static_cast<uint8_t>(pat[0]);
    const uint8_t* found = nullptr;
    for (size_t offset = 0; offset <= size - n;) {
        const auto* p = static_cast<const uint8_t*>(memchr(text + offset, first, size - n - offset + 1));
        if (!p) break;
        offset = static_cast<size_t>(p - text) + 1;
        size_t k = 1;
        while (k < n && (pat[k] < 0 || p[k] == pat[k])) ++k;
        if (k == n) {
            if (found) return nullptr;
            found = p;
        }
    }
    if (!found && !g_codeFromFile)
        found = jumppatch::FindBehindHook(g_canonical.data(), size, pat.data(), n);
    return found ? g_text + (found - text) : nullptr;
}

