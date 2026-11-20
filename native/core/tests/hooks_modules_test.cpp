// SPDX-License-Identifier: MIT
// kiln.hooks open_module's PE reading (services/hooks_modules.h) on the host: which ranges of a mapped module are
// executable code (the only places in another module a hook may go), and which names are Kiln's own modules.
//   KILN_OUT=<dir> native/core/tests/test-hooks-modules.sh
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "services/hooks_modules.h"

using namespace kiln::hooks::modules;

namespace {
int g_fail = 0, g_pass = 0;
void check(bool ok, const char* what, const std::string& detail = "") {
    std::printf("%s %s%s%s\n", ok ? "PASS" : "FAIL", what, detail.empty() ? "" : ": ", detail.c_str());
    ok ? ++g_pass : ++g_fail;
}

template <class T>
void put(std::vector<uint8_t>& b, size_t off, T v) {
    if (b.size() < off + sizeof v) b.resize(off + sizeof v);
    std::memcpy(b.data() + off, &v, sizeof v);
}

struct Sec {
    const char* name;
    uint32_t va, vsize, rawsize, chars;
};

// A mapped PE32+ header page: MZ, PE at 0x80, an optional header of 240 bytes, then the sections.
std::vector<uint8_t> image(const std::vector<Sec>& secs, uint32_t size_of_image = 0x10000, uint16_t machine = 0x8664,
                           uint16_t magic = 0x20B) {
    std::vector<uint8_t> b(0x1000, 0);
    put<uint16_t>(b, 0, 0x5A4D);
    put<uint32_t>(b, 0x3C, 0x80);
    put<uint32_t>(b, 0x80, 0x00004550);
    put<uint16_t>(b, 0x84, machine);
    put<uint16_t>(b, 0x86, static_cast<uint16_t>(secs.size()));
    put<uint16_t>(b, 0x94, 240);
    put<uint16_t>(b, 0x98, magic);
    put<uint32_t>(b, 0x98 + 56, size_of_image);
    size_t s = 0x98 + 240;
    for (const Sec& x : secs) {
        std::memcpy(b.data() + s, x.name, std::min<size_t>(8, std::strlen(x.name)));
        put<uint32_t>(b, s + 8, x.vsize);
        put<uint32_t>(b, s + 12, x.va);
        put<uint32_t>(b, s + 16, x.rawsize);
        put<uint32_t>(b, s + 36, x.chars);
        s += 40;
    }
    return b;
}

constexpr uint32_t kText = 0x60000020;   // code | execute | read
constexpr uint32_t kRdata = 0x40000040;  // initialized data | read
constexpr uint32_t kExecOnly = 0x20000000;
}  // namespace

int main() {
    std::string err;
    {
        const auto b = image({{".text", 0x1000, 0x2345, 0x2400, kText}, {".rdata", 0x4000, 0x800, 0x800, kRdata},
                              {".hook", 0x5000, 0, 0x200, kExecOnly}, {".data", 0x6000, 0x100, 0x200, 0xC0000040}});
        ModuleCode m;
        const bool ok = parse_code(b.data(), b.size(), m, err);
        check(ok && m.size_of_image == 0x10000 && m.code.size() == 2, "two executable sections of four", err);
        check(ok && m.code[0].rva == 0x1000 && m.code[0].size == 0x2345, ".text: VirtualSize is its mapped size");
        check(ok && m.code[1].rva == 0x5000 && m.code[1].size == 0x200, "a section with VirtualSize 0 takes SizeOfRawData");
        check(in_code(m, 0x1000) && in_code(m, 0x1000 + 0x2344) && !in_code(m, 0x1000 + 0x2345),
              "the first and last byte of .text are code, the byte after it is not");
        check(!in_code(m, 0) && !in_code(m, 0x200), "the headers are not code");
        check(!in_code(m, 0x4000) && !in_code(m, 0x4100) && !in_code(m, 0x6000), "read-only and writable data are not code");
        check(in_code(m, 0x51FF) && !in_code(m, 0x5200), "an execute-only section is code");
        check(!in_code(m, 0xFFFFFFFFull + 0x1000), "an rva past 4 GiB is not code (no wrap)");
    }
    {
        const auto b = image({{".text", 0xF000, 0x4000, 0x4000, kText}}, 0x10000);
        ModuleCode m;
        check(parse_code(b.data(), b.size(), m, err) && m.code.size() == 1 && m.code[0].size == 0x1000,
              "a section reaching past SizeOfImage is cut at it");
    }
    {
        const auto b = image({{".text", 0x1000, 0x100, 0x200, kText}}, 0x10000, 0x14C, 0x10B);
        ModuleCode m;
        check(!parse_code(b.data(), b.size(), m, err) && err.find("x64") != std::string::npos, "a 32-bit image is refused", err);
    }
    {
        const auto b = image({{".rdata", 0x1000, 0x100, 0x200, kRdata}});
        ModuleCode m;
        check(!parse_code(b.data(), b.size(), m, err) && err == "no executable section", "an image without code is refused", err);
    }
    {
        auto b = image({{".text", 0x1000, 0x100, 0x200, kText}});
        ModuleCode m;
        check(!parse_code(b.data(), 0x40, m, err), "headers cut short are refused", err);
        check(!parse_code(b.data(), 0x98 + 240 + 20, m, err) && err == "truncated section table", "a cut section table is refused", err);
        b[0] = 'X';
        check(!parse_code(b.data(), b.size(), m, err) && err == "no MZ header", "no MZ, no image", err);
        check(!parse_code(nullptr, 0, m, err), "no bytes, no image");
        auto c = image({{".text", 0x1000, 0x100, 0x200, kText}});
        put<uint32_t>(c, 0x3C, 0xFFFFFFF0u);
        check(!parse_code(c.data(), c.size(), m, err) && err == "no PE header", "a hostile e_lfanew is refused", err);
    }
    check(kiln_own_name("kiln-core.dll") && kiln_own_name("Kiln-Rules.DLL") && kiln_own_name("kiln-flame.dll"),
          "kiln-core.dll and kiln-*.dll are Kiln's own modules (any case)");
    check(!kiln_own_name("creative.dll") && !kiln_own_name("KilnTweaks.dll") && !kiln_own_name("kiln-.dll") &&
              !kiln_own_name("kiln-x.exe"),
          "creative.dll, KilnTweaks.dll, kiln-.dll and kiln-x.exe are not");
    std::printf("hooks_modules_test: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
