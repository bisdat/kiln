// SPDX-License-Identifier: MIT
// istext_named.dll / istext_other.dll: Shroudtopia mods shaped like Item Skill Descriptions 0.3.0 for the original-code
// scenario (inbox 26c). Built with the DLL CRT, so memcmp is an import from vcruntime140, as in the real mod. Activate
// compares 32 bytes at the exe's st_text_target entry, through that import, with a signature in its own image (the
// entry's bytes before any hook, from st_text_expected: what the real mod carries as constants), and hooks the function
// only when they match (with safetyhook, built into this DLL, standing in for the real mod's bundled MinHook: both copy
// an entry jump they find into their trampoline as an absolute jump). It also logs where its memcmp import points, the
// same compare against a stack copy, an ordinary memcmp of its own buffers, and the compare again after its own hook.
// istext_other (IST_OTHER) is the same mod under another name, which no compat rule names.
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
// The expected bytes live in this DLL's image, as the real mod's static signature (DLL +0x84c0) does.
unsigned char g_expected[32];

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
        for (size_t i = 0; i < sizeof g_expected; ++i) g_expected[i] = pristine[i];
        // The same bytes on the stack: only a signature in the mod's own image gets the file's answer.
        unsigned char on_stack[32];
        for (size_t i = 0; i < sizeof on_stack; ++i) on_stack[i] = pristine[i];
        // Two buffers of this mod's own (contents the compiler cannot see, so memcmp is called), the second one above:
        // the shim must answer as the CRT does for anything that is not the game's code.
        unsigned char a[4], b[4];
        for (size_t i = 0; i < sizeof a; ++i) a[i] = b[i] = pristine[i];
        b[3] = static_cast<unsigned char>(a[3] + 1);
        const int plain = memcmp(a, b, g_plain_size);
        const int entry = memcmp(target, g_expected, g_entry_size);
        const int stack = memcmp(target, on_stack, g_entry_size);
        char line[256];
        std::snprintf(line, sizeof line, "original-code: memcmp import -> %s; entry %s; a stack copy %s; plain memcmp %s",
                      module_of(__imp_memcmp).c_str(), entry == 0 ? "matches" : "differs", stack == 0 ? "matches" : "differs",
                      plain < 0 ? "below" : plain == 0 ? "equal" : "above");
        c->Log(line);
        if (entry != 0) {
            c->Log("original-code: the entry was changed by another mod; not hooking it");
            return;
        }
        g_hook = safetyhook::create_inline(const_cast<unsigned char*>(target), reinterpret_cast<void*>(&detour));
        c->Log(g_hook ? "original-code: hooked st_text_target" : "original-code: hooking st_text_target FAILED");
        // Checked again with its own hook in: the shim never hides a hook of the asking mod.
        c->Log(memcmp(target, g_expected, g_entry_size) == 0 ? "original-code: after its own hook the entry matches"
                                                             : "original-code: after its own hook the entry differs");
    }
    void Deactivate(ModContext*) override { active = false; }
    void Update(ModContext*) override {}
};
}  // namespace

extern "C" __declspec(dllexport) Mod* CreateModInstance() { return new OriginalCodeMod(); }
