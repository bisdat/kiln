// SPDX-License-Identifier: MIT
// Only a rule-selected mod's IAT calls this shim. All state is retained with the pinned host and mod DLLs.
#include "shroudtopia_internal.h"
#include "../../compat/original_code_service.h"
#include "original_code.h"
#include "memcmp_import.h"
#include <atomic>
#include <mutex>
#include <psapi.h>

namespace kshroud::original {
namespace {
struct Code {
    uintptr_t base = 0;
    uint32_t image_size = 0, text_rva = 0, text_size = 0;
};
struct Module {
    uintptr_t base;
    size_t size;
    const kiln::compat::OriginalCodeApi* rules;
    Module* next;
    Code code;
    bool contains(uintptr_t p, size_t n = 1) const {
        return p >= base && p - base < size && n <= size - (p - base);
    }
};
// Publish immutable records before publishing any shim pointer. Readers never lock or allocate.
std::atomic<Module*> g_modules{nullptr};
std::atomic<bool> g_used{false};
std::mutex g_install;

bool copy(void* to, const void* from, size_t size) {
    SIZE_T read = 0;
    return ReadProcessMemory(GetCurrentProcess(), from, to, size, &read) && read == size;
}

bool operand(const void* p, const void* signature, size_t size, const Module& mod, uint32_t* rva) {
    const uintptr_t address = reinterpret_cast<uintptr_t>(p), base = mod.code.base;
    const uintptr_t text = base + mod.code.text_rva;
    // The real checker uses a static signature in its own DLL. Heap copies and two game ranges stay ordinary.
    if (!size || size > 4096 || address < text || address - text >= mod.code.text_size ||
        size > mod.code.text_size - (address - text) || !mod.contains(reinterpret_cast<uintptr_t>(signature), size)) return false;
    DWORD64 image = 0;
    const auto* entry = RtlLookupFunctionEntry(address, &image, nullptr);
    RUNTIME_FUNCTION fn{};
    if (image != base || !entry || !copy(&fn, entry, sizeof fn) || fn.BeginAddress >= fn.EndAddress || fn.EndAddress > mod.code.image_size ||
        address < base + fn.BeginAddress || address >= base + fn.EndAddress ||
        address - (base + fn.BeginAddress) >= 34 || size > base + fn.EndAddress - address) return false;
    const uintptr_t begin = base + fn.BeginAddress;
    const size_t offset = address - begin;
    const size_t span = std::min<size_t>(fn.EndAddress - fn.BeginAddress, std::max<size_t>(34, offset + size));
    std::array<uint8_t, 4096 + 34> live{}, original{};
    // Core owns the validated, relocated file view already. Only this bounded range is copied.
    if (!mod.rules->read(begin, original.data(), span) || !copy(live.data(), reinterpret_cast<const void*>(begin), span))
        return false;
    uintptr_t target = 0;
    if (!entry_range(original.data(), live.data(), span, offset, size, begin, base, mod.code.image_size, &target))
        return false;
    if (mod.contains(target)) return false;
    // MinHook's entry normally jumps to an allocated FF25 relay, which then jumps into the owning DLL.
    std::array<uint8_t, 14> relay{};
    if (!copy(relay.data(), reinterpret_cast<const void*>(target), relay.size())) return false;
    uintptr_t detour = 0;
    if (kiln::pe::entry_jump(relay.data(), relay.size(), target, base, mod.code.image_size, &detour) &&
        mod.contains(detour)) return false;
    std::array<uint8_t, 4096> expected{};
    if (!copy(expected.data(), signature, size) || std::memcmp(original.data() + offset, expected.data(), size) != 0)
        return false;
    *rva = static_cast<uint32_t>(address - base);
    return true;
}

bool read_code(Code& code, const kiln::compat::OriginalCodeApi* rules) {
    auto* base = reinterpret_cast<const uint8_t*>(GetModuleHandleW(nullptr));
    IMAGE_DOS_HEADER dos{};
    IMAGE_NT_HEADERS64 nt{};
    if (!copy(&dos, base, sizeof dos) || dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew < 0 ||
        dos.e_lfanew > 4096 || !copy(&nt, base + dos.e_lfanew, sizeof nt) || nt.Signature != IMAGE_NT_SIGNATURE ||
        nt.FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 || nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC)
        return false;
    const size_t table = size_t(dos.e_lfanew) + 24 + nt.FileHeader.SizeOfOptionalHeader;
    if (table > nt.OptionalHeader.SizeOfImage ||
        nt.FileHeader.NumberOfSections > (nt.OptionalHeader.SizeOfImage - table) / sizeof(IMAGE_SECTION_HEADER)) return false;
    for (size_t i = 0; i < nt.FileHeader.NumberOfSections; ++i) {
        IMAGE_SECTION_HEADER sec{};
        if (!copy(&sec, base + table + i * sizeof sec, sizeof sec)) return false;
        if (std::memcmp(sec.Name, ".text\0\0\0", 8) != 0) continue;
        uint8_t probe = 0;
        if (!(sec.Characteristics & IMAGE_SCN_MEM_EXECUTE) || !sec.Misc.VirtualSize ||
            sec.VirtualAddress >= nt.OptionalHeader.SizeOfImage ||
            sec.Misc.VirtualSize > nt.OptionalHeader.SizeOfImage - sec.VirtualAddress ||
            !rules->read(reinterpret_cast<uintptr_t>(base) + sec.VirtualAddress, &probe, 1)) return false;
        code = {reinterpret_cast<uintptr_t>(base), nt.OptionalHeader.SizeOfImage, sec.VirtualAddress, sec.Misc.VirtualSize};
        return true;
    }
    return false;
}

int __cdecl compare(const void* a, const void* b, size_t size) {
    uint32_t rva = 0;
    for (const Module* mod = g_modules.load(); mod; mod = mod->next) {
        if (!operand(a, b, size, *mod, &rva) && !operand(b, a, size, *mod, &rva)) continue;
        if (!g_used.exchange(true)) {
            try { mod->rules->used(rva); } catch (...) {} // logging cannot change the answer
        }
        return 0;
    }
    return std::memcmp(a, b, size);
}
} // namespace

size_t install(HMODULE module, const kiln::compat::OriginalCodeApi* rules) {
    try {
        if (!module || !rules || rules->header.version < kiln::compat::kOriginalCodeVersion || !rules->read) return 0;
        std::lock_guard lock(g_install);
        MODULEINFO info{};
        if (!K32GetModuleInformation(GetCurrentProcess(), module, &info, sizeof info)) return 0;
        for (const Module* m = g_modules.load(); m; m = m->next)
            if (m->base == reinterpret_cast<uintptr_t>(info.lpBaseOfDll)) return 0;
        Code code;
        if (!read_code(code, rules)) return 0;
        auto* record = new Module{reinterpret_cast<uintptr_t>(info.lpBaseOfDll), info.SizeOfImage, rules, g_modules.load(), code};
        g_modules.store(record); // kept for process lifetime, even if an import write fails
        return patch_memcmp(static_cast<uint8_t*>(info.lpBaseOfDll), info.SizeOfImage,
            reinterpret_cast<uintptr_t>(&compare), &copy, [](void* slot, uintptr_t value) {
                MEMORY_BASIC_INFORMATION page{};
                if (!VirtualQuery(slot, &page, sizeof page) || page.State != MEM_COMMIT ||
                    (page.Protect & (PAGE_GUARD | PAGE_NOACCESS))) return false;
                const DWORD writable = (page.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE |
                                                       PAGE_EXECUTE_WRITECOPY)) ? PAGE_EXECUTE_READWRITE : PAGE_READWRITE;
                DWORD old = 0;
                if (!VirtualProtect(slot, sizeof(void*), writable, &old)) return false;
                InterlockedExchangePointer(static_cast<void* volatile*>(slot), reinterpret_cast<void*>(value));
                DWORD ignored = 0;
                if (!VirtualProtect(slot, sizeof(void*), old, &ignored) &&
                    !VirtualProtect(slot, sizeof(void*), old, &ignored))
                    H.logf(KILN_LOG_WARN, "shroudtopia: could not restore an import page's protection");
                return true;
            });
    } catch (...) {
        return 0; // allocation failure leaves the mod's normal check in place
    }
}
} // namespace kshroud::original
