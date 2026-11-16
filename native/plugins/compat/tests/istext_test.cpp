// SPDX-License-Identifier: MIT
// istext_named.dll / istext_other.dll: Shroudtopia mods shaped like Item Skill Descriptions 0.3.0 for the original-code
// scenario (inbox 26c). Built with the DLL CRT, so memcmp is an import from vcruntime140, as in the real mod. Activate
// compares 32 bytes at the exe's st_text_target entry with st_text_expected (the entry before any hook: what the real mod
// carries as constants) through that import, and hooks the function only when they match (with safetyhook, built into
// this DLL, standing in for the real mod's bundled MinHook: both copy an entry jump they find into their trampoline as an
// absolute jump). It logs where its memcmp import points and an ordinary memcmp of its own buffers. istext_other (IST_OTHER)
// is the same mod under another name, which no compat rule names.
#include <windows.h>

#include <cstdio>
#include <cstring>
#include <string>

#undef WIN32_LEAN_AND_MEAN  // Shroudtopia's header defines it again (kept as upstream has it)
#include <shroudtopia/shroudtopia.h>

#include "safetyhook.hpp"

extern "C" void* __imp_memcmp;  // this DLL's import address table slot for memcmp

namespace {
#ifdef IST_OTHER
ModMetaData kMeta = {"Kiln Live Code Mod", "original-code control: no rule names it", "1.0", "kiln", "0.1.1", true, true};
constexpr char kTrace = 'L';
#else
ModMetaData kMeta = {"Kiln Original Code Mod", "original-code: checks the entry, then hooks", "1.0", "kiln", "0.1.1", true, true};
constexpr char kTrace = 'M';
#endif

using TraceFn = void (*)(char);
TraceFn g_trace = nullptr;
safetyhook::InlineHook g_hook;
int detour(int a) {
    if (g_trace) g_trace(kTrace);
    return g_hook.call<int>(a);
}

// Sizes the compiler cannot see: memcmp stays a call through the import, as in the real mod (no inlined compare).
volatile size_t g_entry_size = 32, g_plain_size = 4;

std::string module_of(const void* p) {
    HMODULE m = nullptr;
    wchar_t path[MAX_PATH]{};
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            static_cast<LPCWSTR>(p), &m) || !GetModuleFileNameW(m, path, MAX_PATH))
        return "?";
    const wchar_t* name = path;
    for (const wchar_t* c = path; *c; ++c)
        if (*c == L'\\' || *c == L'/') name = c + 1;
    char out[MAX_PATH]{};
    WideCharToMultiByte(CP_UTF8, 0, name, -1, out, sizeof out, nullptr, nullptr);
    return out;
}

class OriginalCodeMod : public Mod {
public:
    ModMetaData GetMetaData() override { return kMeta; }
    void Load(ModContext*) override {}
    void Unload(ModContext*) override {}
    void Activate(ModContext* c) override {
        active = true;  // as Shroudtopia mods do: the loader calls Activate again while this is false
        if (g_hook) return;
        HMODULE exe = GetModuleHandleW(nullptr);
        const auto* target = reinterpret_cast<const unsigned char*>(GetProcAddress(exe, "st_text_target"));
        const auto* pristine = reinterpret_cast<const unsigned char*>(GetProcAddress(exe, "st_text_expected"));
        g_trace = reinterpret_cast<TraceFn>(GetProcAddress(exe, "st_trace"));
        if (!target || !pristine || !g_trace) {
            c->Log("original-code: the host exe has no st_text_target");
            return;
        }
        unsigned char expected[32];
        for (size_t i = 0; i < sizeof expected; ++i) expected[i] = pristine[i];
        // Two buffers of this mod's own (contents the compiler cannot see, so memcmp is called), the second one above:
        // the shim must answer as the CRT does for anything that is not the game's code.
        unsigned char a[4], b[4];
        for (size_t i = 0; i < sizeof a; ++i) a[i] = b[i] = pristine[i];
        b[3] = static_cast<unsigned char>(a[3] + 1);
        const int plain = memcmp(a, b, g_plain_size);
        const int entry = memcmp(target, expected, g_entry_size);
        char line[256];
        std::snprintf(line, sizeof line, "original-code: memcmp import -> %s; entry %s; plain memcmp %s",
                      module_of(__imp_memcmp).c_str(), entry == 0 ? "matches" : "differs",
                      plain < 0 ? "below" : plain == 0 ? "equal" : "above");
        c->Log(line);
        if (entry != 0) {
            c->Log("original-code: the entry was changed by another mod; not hooking it");
            return;
        }
        g_hook = safetyhook::create_inline(const_cast<unsigned char*>(target), reinterpret_cast<void*>(&detour));
        c->Log(g_hook ? "original-code: hooked st_text_target" : "original-code: hooking st_text_target FAILED");
    }
    void Deactivate(ModContext*) override { active = false; }
    void Update(ModContext*) override {}
};
}  // namespace

extern "C" __declspec(dllexport) Mod* CreateModInstance() { return new OriginalCodeMod(); }
