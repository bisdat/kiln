// SPDX-License-Identifier: MIT
// Only the rule-selected mod's IAT calls this shim. The game and every other DLL keep their imports and code.
#include "shroudtopia_internal.h"
#include "../../compat/original_code_service.h"
#include "original_code.h"
#include "memcmp_import.h"
#include <atomic>
#include <fstream>
#include <mutex>
#include <psapi.h>

namespace kshroud::original {
namespace {
View g_view;
std::once_flag g_once;
std::atomic<bool> g_used{false};
std::atomic<const kiln::compat::OriginalCodeApi*> g_rules{nullptr};

bool copy(void* to, const void* from, size_t size) {
    SIZE_T read = 0;
    return ReadProcessMemory(GetCurrentProcess(), from, to, size, &read) && read == size;
}

void read_original() {
    // Ordinary reads, rather than dereferencing a mapped file: an I/O error cannot fault in a foreign memcmp.
    wchar_t path[32768]{};
    const DWORD n = GetModuleFileNameW(nullptr, path, static_cast<DWORD>(std::size(path)));
    if (!n || n >= std::size(path)) return;
    std::ifstream in(std::filesystem::path(path), std::ios::binary | std::ios::ate);
    if (!in) return;
    const auto length = in.tellg();
    if (length <= 0 || length > 512ll * 1024 * 1024) return;
    std::vector<uint8_t> file(static_cast<size_t>(length));
    in.seekg(0);
    if (!in.read(reinterpret_cast<char*>(file.data()), static_cast<std::streamsize>(file.size()))) return;
    kiln::pe::FileText ft;
    std::string why;
    if (!kiln::pe::parse_file_text(file.data(), file.size(), ft, why) || ft.text_vsize > 512ull * 1024 * 1024) return;
    auto* base = reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr));
    IMAGE_DOS_HEADER dos{};
    IMAGE_NT_HEADERS64 nt{};
    if (!copy(&dos, base, sizeof dos) || dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew < 0 ||
        dos.e_lfanew > 4096 || !copy(&nt, base + dos.e_lfanew, sizeof nt) || nt.Signature != IMAGE_NT_SIGNATURE ||
        nt.FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 || nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC ||
        nt.FileHeader.TimeDateStamp != ft.timestamp || nt.OptionalHeader.SizeOfImage != ft.size_of_image) return;
    uint32_t file_pe = 0;
    std::memcpy(&file_pe, file.data() + 0x3C, sizeof file_pe);
    uint16_t file_sections = 0, file_opt_size = 0;
    std::memcpy(&file_sections, file.data() + size_t(file_pe) + 6, 2);
    std::memcpy(&file_opt_size, file.data() + size_t(file_pe) + 20, 2);
    if (file_pe != static_cast<uint32_t>(dos.e_lfanew) || file_sections != nt.FileHeader.NumberOfSections ||
        file_opt_size != nt.FileHeader.SizeOfOptionalHeader) return;
    // Only executable, file-backed .text from this same loaded image is eligible.
    const size_t table = size_t(dos.e_lfanew) + 24 + nt.FileHeader.SizeOfOptionalHeader;
    bool executable = false;
    for (size_t i = 0; i < nt.FileHeader.NumberOfSections; ++i) {
        IMAGE_SECTION_HEADER sec{}, disk{};
        const size_t at = table + i * sizeof sec;
        if (at > file.size() || sizeof disk > file.size() - at || !copy(&sec, base + at, sizeof sec)) return;
        std::memcpy(&disk, file.data() + at, sizeof disk);
        if (std::memcmp(sec.Name, ".text\0\0\0", 8) != 0) continue;
        executable = (sec.Characteristics & disk.Characteristics & IMAGE_SCN_MEM_EXECUTE) &&
            sec.VirtualAddress == ft.text_rva && sec.Misc.VirtualSize == ft.text_vsize;
    }
    if (!executable) return;
    View view;
    view.base = reinterpret_cast<uintptr_t>(base);
    view.image_size = ft.size_of_image;
    view.text_rva = ft.text_rva;
    view.bytes.resize(ft.text_vsize, 0);
    std::memcpy(view.bytes.data(), file.data() + ft.text_raw, ft.file_bytes());
    kiln::pe::relocate_text(view.bytes.data(), view.bytes.size(), ft, view.base);
    g_view = std::move(view);
}

const uint8_t* operand(const void* p, size_t size) {
    const uintptr_t address = reinterpret_cast<uintptr_t>(p), text = g_view.base + g_view.text_rva;
    if (g_view.bytes.empty() || !size || address < text || address - text >= g_view.bytes.size() ||
        size > g_view.bytes.size() - (address - text)) return nullptr;
    DWORD64 image = 0;
    // PE unwind metadata supplies a real function start. Leaf code without it keeps ordinary memcmp.
    const auto* entry = RtlLookupFunctionEntry(address, &image, nullptr);
    RUNTIME_FUNCTION fn{};
    if (image != g_view.base || !entry || !copy(&fn, entry, sizeof fn)) return nullptr;
    uintptr_t destination = 0;
    return g_view.range(address, size, image + fn.BeginAddress, image + fn.EndAddress, &copy, &destination);
}

int __cdecl compare(const void* a, const void* b, size_t size) {
    const auto* original_a = operand(a, size);
    const auto* original_b = operand(b, size);
    const int result = std::memcmp(original_a ? original_a : a, original_b ? original_b : b, size);
    if ((original_a || original_b) && !g_used.exchange(true)) {
        const auto address = reinterpret_cast<uintptr_t>(original_a ? a : b);
        try {
            if (const auto* rules = g_rules.load()) rules->used(static_cast<uint32_t>(address - g_view.base));
        } catch (...) {} // a log failure cannot change memcmp's result
    }
    return result;
}
} // namespace

size_t install(HMODULE module, const kiln::compat::OriginalCodeApi* rules) {
    try {
        std::call_once(g_once, [] { try { read_original(); } catch (...) {} });
        if (g_view.bytes.empty()) return 0;
        MODULEINFO info{};
        if (!K32GetModuleInformation(GetCurrentProcess(), module, &info, sizeof info)) return 0;
        g_rules.store(rules); // publish before the shim pointer; other mods may already be calling it
        return patch_memcmp(static_cast<uint8_t*>(info.lpBaseOfDll), info.SizeOfImage,
            reinterpret_cast<uintptr_t>(&compare), &copy, [](void* slot, uintptr_t value) {
                DWORD old = 0;
                if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) return false;
                InterlockedExchangePointer(static_cast<void* volatile*>(slot), reinterpret_cast<void*>(value));
                DWORD ignored = 0;
                VirtualProtect(slot, sizeof(void*), old, &ignored);
                return true;
            });
    } catch (...) {
        return 0; // allocations or file I/O failed: keep the mod's normal check
    }
}
} // namespace kshroud::original
