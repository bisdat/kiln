// SPDX-License-Identifier: MIT
// kiln.hooks: the executable sections of a loaded module, read from its mapped PE headers, for open_module (hooks
// inside another mod's or a loader's DLL; kiln_hooks.h). Platform independent (no Windows headers): the host test
// native/core/tests/hooks_modules_test.cpp builds it as is; hooks.cpp feeds it the module's first page.
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace kiln::hooks::modules {

struct CodeRange {
    uint32_t rva = 0, size = 0;  // one executable section as mapped: [rva, rva + size)
};

struct ModuleCode {
    uint32_t size_of_image = 0;
    std::vector<CodeRange> code;  // in section order
};

namespace detail {
template <class T>
inline bool read(const uint8_t* p, size_t n, size_t off, T* out) {
    if (off > n || n - off < sizeof(T)) return false;
    std::memcpy(out, p + off, sizeof(T));
    return true;
}
}  // namespace detail

constexpr uint32_t kScnMemExecute = 0x20000000;  // IMAGE_SCN_MEM_EXECUTE
constexpr uint32_t kScnCntCode = 0x00000020;     // IMAGE_SCN_CNT_CODE

// Parses a mapped PE32+ image's headers (`n` bytes from the module base) into its executable sections. A section
// counts when it is marked executable (or holds code); its mapped size is VirtualSize (SizeOfRawData when that is 0),
// cut at SizeOfImage. False (with `error`) for anything that is not an x64 PE image with at least one such section.
inline bool parse_code(const uint8_t* headers, size_t n, ModuleCode& out, std::string& error) {
    using detail::read;
    out = ModuleCode{};
    uint16_t mz = 0, machine = 0, nsec = 0, optsz = 0, magic = 0;
    uint32_t pe = 0, sig = 0;
    if (!headers || !read(headers, n, 0, &mz) || mz != 0x5A4D) return error = "no MZ header", false;
    if (!read(headers, n, 0x3C, &pe) || pe < 0x40 || !read(headers, n, pe, &sig) || sig != 0x00004550)
        return error = "no PE header", false;
    const size_t coff = static_cast<size_t>(pe) + 4, opt = coff + 20;
    if (!read(headers, n, coff, &machine) || !read(headers, n, coff + 2, &nsec) || !read(headers, n, coff + 16, &optsz) ||
        !read(headers, n, opt, &magic))
        return error = "truncated COFF header", false;
    if (machine != 0x8664 || magic != 0x20B) return error = "not an x64 (PE32+) image", false;
    if (optsz < 60 || !read(headers, n, opt + 56, &out.size_of_image) || out.size_of_image == 0)
        return error = "truncated optional header", false;
    const size_t first = opt + optsz;
    for (uint16_t i = 0; i < nsec; ++i) {
        const size_t s = first + static_cast<size_t>(i) * 40;
        uint32_t vsize = 0, va = 0, rawsize = 0, chars = 0;
        if (!read(headers, n, s + 8, &vsize) || !read(headers, n, s + 12, &va) || !read(headers, n, s + 16, &rawsize) ||
            !read(headers, n, s + 36, &chars))
            return error = "truncated section table", false;
        if (!(chars & (kScnMemExecute | kScnCntCode))) continue;
        uint64_t size = vsize ? vsize : rawsize;
        if (va >= out.size_of_image || size == 0) continue;
        if (va + size > out.size_of_image) size = out.size_of_image - va;
        out.code.push_back(CodeRange{va, static_cast<uint32_t>(size)});
    }
    if (out.code.empty()) return error = "no executable section", false;
    return true;
}

// Whether `rva` lies in one of the executable sections.
inline bool in_code(const ModuleCode& m, uint64_t rva) {
    for (const CodeRange& r : m.code)
        if (rva >= r.rva && rva - r.rva < r.size) return true;
    return false;
}

// Kiln's own modules: kiln-core.dll and kiln-<service|plugin>.dll (the same rule as the hook map's "own" jumps).
inline bool kiln_own_name(std::string name) {
    for (char& c : name) c = static_cast<char>((c >= 'A' && c <= 'Z') ? c - 'A' + 'a' : c);
    return name.size() > 9 && name.rfind("kiln-", 0) == 0 && name.compare(name.size() - 4, 4, ".dll") == 0;
}

}  // namespace kiln::hooks::modules
