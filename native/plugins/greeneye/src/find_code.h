// SPDX-License-Identifier: MIT
// Included by hot.cpp and the host test, with TextSection, ImageSpan, ReadOriginalCode, ReadLive, ReadPointer and
// jumppatch available.
uint8_t* g_text = nullptr;
size_t g_textSize = 0;
std::once_flag g_canonicalOnce;
std::vector<uint8_t> g_canonical; // .text as it was before our hooks (made on first find)
bool g_codeFromFile = false;
std::string g_codeNote;
uintptr_t g_imageBase = 0; // the loaded exe: "outside the image" for the hook forms
size_t g_imageSize = 0;

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

// The code find matches, made once, by the first find or read.
void PrepareCode()
{
    std::call_once(g_canonicalOnce, [] {
        if (!TextSection(g_text, g_textSize)) return;
        ImageSpan(g_imageBase, g_imageSize);
        // Other engines have already patched the game by on_ready. A short signature may be entirely gone;
        // looking behind its jump cannot recover it. Match the same exe's relocated file bytes instead.
        try {
            g_codeFromFile = ReadOriginalCode(g_canonical, g_codeNote);
        } catch (...) {
            // /EHa also catches mapped-file I/O faults and unwinds ReadOriginalCode's handles. Find is called
            // outside SafeStart too (the startup diagnostic and reload safe point), so guard acquisition here.
            g_codeFromFile = false;
            g_codeNote = "cannot read the exe code";
        }
        if (!g_codeFromFile) {
            g_canonical.resize(g_textSize);
            ReadLive(g_text, g_canonical.data(), g_textSize);
        }
    });
}

uint8_t* Find(const char* pattern, void* module)
{
    if (module && module != GetModuleHandleW(nullptr))
        return nullptr; // only the exe's code
    PrepareCode();
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

// host->read: the code find matches (kiln#157). The live bytes with Greeneye's own hooks laid back and, while that code
// is the exe file's, the file's bytes under every other recognised hook: a jump to outside the image with the nops
// behind it, or a call sent outside the image (the forms OriginalText keeps the file's bytes under). Kiln's own
// detours, hosted ShroudForge's and other loaders' hooks count, also ones written after the code was read. Up to
// 0.2.0-dev.31.25 find matched the file's bytes but read returned such a hook's: a mod that found a call or a global
// load by its pattern and read the operand inside the match got the hook's bytes, so Hollow Halls Progression called
// its signatures unsupported beside hosted ShroudForge on 31.24. A byte another mod changed in place stays the live
// one, as in find; so do all bytes of a place changed too much to sort (as OriginalText), and every byte when the code
// came from the live snapshot.
void Read(const void* p, void* out, size_t n)
{
    if (!ReadLive(p, out, n)) return;
    PrepareCode();
    const auto at = reinterpret_cast<uintptr_t>(p), text = reinterpret_cast<uintptr_t>(g_text);
    const size_t size = g_canonical.size();
    if (!g_codeFromFile || !text || !size || at >= text + size || at + n <= text) return;
    const uintptr_t lo = (std::max)(at, text), hi = (std::min)(at + n, text + size);
    // A hook covering the first or last byte starts or ends nearby (a jump of up to 14 bytes, 20 nops): classify a
    // window with ample margin, so the bytes at its edges never decide the range read.
    constexpr size_t kMargin = 4096;
    const uintptr_t from = lo - text > kMargin ? lo - kMargin : text;
    const uintptr_t to = text + size - hi > kMargin ? hi + kMargin : text + size;
    std::vector<uint8_t> window(to - from);
    if (!ReadLive(reinterpret_cast<const void*>(from), window.data(), window.size())) return;
    std::vector<kiln::pe::DiffRun> changed; // in-place changes: bytes no hook form explains
    const auto d = kiln::pe::other_differences(g_canonical.data() + (from - text), window.data(), window.size(), from,
        g_imageBase, g_imageSize, nullptr, 0, false, &ReadPointer, &changed, text, size);
    size_t recorded = 0;
    for (const auto& run : changed) recorded += run.len;
    if (recorded != d.bytes) return;
    auto* dst = static_cast<uint8_t*>(out) + (lo - at);
    std::memcpy(dst, g_canonical.data() + (lo - text), hi - lo);
    for (const auto& run : changed) {
        const uintptr_t a = (std::max)(from + run.rva, lo), b = (std::min)(from + run.rva + run.len, hi);
        if (a < b) std::memcpy(dst + (a - lo), window.data() + (a - from), b - a);
    }
}
