// SPDX-License-Identifier: MIT
// istext_exec.dll: a Shroudtopia mod whose import address table sits on an executable page (linked with
// /MERGE:.rdata=.text), for the original-code scenario (review of 09a2332b, finding 1). Its DllMain starts a thread that
// keeps running code placed at the end of the code, on the same page as its memcmp import slot, until Activate. While it
// runs, kiln-shroudtopia rewrites that slot before the mod's first Load: a write that drops the page's execute bit even
// for a moment stops that thread with an access violation. Activate stops the thread and logs whether its code was on the
// slot's page, the page's protection at load and now, how often it ran, and where its memcmp import points.
#include <windows.h>

#include <cstdio>
#include <cstring>
#include <string>

#undef WIN32_LEAN_AND_MEAN  // Shroudtopia's header defines it again (kept as upstream has it)
#include <shroudtopia/shroudtopia.h>

extern "C" void* __imp_memcmp;  // this DLL's import address table slot for memcmp

namespace {
ModMetaData kMeta = {"Kiln Exec Import Mod", "original-code: an import table on an executable page", "1.0", "kiln", "0.1.1", true, true};

volatile LONG g_stop = 0;
volatile LONG64 g_spins = 0;
HANDLE g_thread = nullptr;
DWORD g_protect_at_load = 0;
volatile size_t g_size = 32;
unsigned char g_expected[32];

// Last in the code (".text$zzzz"), so next to the merged .rdata and its import table.
#pragma code_seg(push, ".text$zzzz")
__declspec(noinline) DWORD WINAPI spin(void*) {
    while (!g_stop) g_spins = g_spins + 1;
    return 0;
}
#pragma code_seg(pop)

uintptr_t page_of(const void* p) { return reinterpret_cast<uintptr_t>(p) & ~uintptr_t(0xFFF); }

DWORD protect_of(const void* p) {
    MEMORY_BASIC_INFORMATION m{};
    return VirtualQuery(p, &m, sizeof m) ? m.Protect : 0;
}

std::string module_of(const void* p) {
    HMODULE m = nullptr;
    char path[MAX_PATH]{};
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            static_cast<LPCSTR>(p), &m) || !GetModuleFileNameA(m, path, MAX_PATH))
        return "?";
    const char* slash = std::strrchr(path, '\\');
    return slash ? slash + 1 : path;
}

class ExecImportMod : public Mod {
public:
    ModMetaData GetMetaData() override { return kMeta; }
    void Load(ModContext*) override {}
    void Unload(ModContext*) override {}
    void Activate(ModContext* c) override {
        active = true;
        if (!g_thread) return;
        InterlockedExchange(&g_stop, 1);
        const bool stopped = WaitForSingleObject(g_thread, 2000) == WAIT_OBJECT_0;
        CloseHandle(g_thread);
        g_thread = nullptr;
        // The memcmp import is used (and patched) like the other mods': the hooked entry against a signature in its image.
        HMODULE exe = GetModuleHandleW(nullptr);
        const auto* target = reinterpret_cast<const unsigned char*>(GetProcAddress(exe, "st_text_target"));
        const auto* pristine = reinterpret_cast<const unsigned char*>(GetProcAddress(exe, "st_text_expected"));
        int entry = -1;
        if (target && pristine) {
            for (size_t i = 0; i < sizeof g_expected; ++i) g_expected[i] = pristine[i];
            entry = memcmp(target, g_expected, g_size);
        }
        char line[320];
        std::snprintf(line, sizeof line,
                      "exec-import: thread code on the import slot's page %s; page protection 0x%lx at load, 0x%lx now; thread %s "
                      "after %lld runs; memcmp import -> %s; entry %s",
                      page_of(reinterpret_cast<const void*>(&spin)) == page_of(&__imp_memcmp) ? "yes" : "no",
                      g_protect_at_load, protect_of(&__imp_memcmp), stopped ? "stopped" : "DID NOT STOP",
                      static_cast<long long>(g_spins), module_of(__imp_memcmp).c_str(), entry == 0 ? "matches" : "differs");
        c->Log(line);
    }
    void Deactivate(ModContext*) override { active = false; }
    void Update(ModContext*) override {}
};
}  // namespace

extern "C" __declspec(dllexport) Mod* CreateModInstance() { return new ExecImportMod(); }

BOOL WINAPI DllMain(HINSTANCE, DWORD reason, void*) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_protect_at_load = protect_of(&__imp_memcmp);
        g_thread = CreateThread(nullptr, 0, &spin, nullptr, 0, nullptr);
    }
    return TRUE;
}
