// SPDX-License-Identifier: MIT
// hooks-v2-module.dll: "another module" for hooks_v2_test's foreign-module test (test_foreign_module): a DLL that is
// neither the game nor Kiln, like Creative Mode's creative.dll that XHL-Creative-Mode-Fix hooks inside. Its functions
// are hand-assembled so the test knows their instruction boundaries; hooks_module_info() hands their addresses out.
// Built by native/spikes/hooksv2/CMakeLists.txt next to the test; never deployed.
#include <cstdint>

extern "C" {
int mod_add(int);           // x + 1 (push rbx; sub rsp,20h | mov eax,ecx ...): a mid hook goes at mod_add_mid
extern char mod_add_mid[];  // mod_add + 5: "mov eax, ecx"
int mod_twice(int);         // x + 1, for detours
int mod_marked(int);        // x + 7, with a unique byte run for module_scan
}

__asm__(".intel_syntax noprefix\n.text\n"
        ".globl mod_add\n.def mod_add; .scl 2; .type 32; .endef\nmod_add:\n"
        "  push rbx\n  sub rsp, 0x20\n"
        ".globl mod_add_mid\nmod_add_mid:\n"
        "  mov eax, ecx\n  add eax, 1\n  add rsp, 0x20\n  pop rbx\n  ret\n"
        "  int3\n  int3\n  int3\n  int3\n  int3\n  int3\n  int3\n  int3\n  int3\n  int3\n  int3\n  int3\n  int3\n  int3\n"
        "  int3\n  int3\n  int3\n  int3\n  int3\n  int3\n  int3\n  int3\n  int3\n  int3\n  int3\n  int3\n  int3\n  int3\n"
        ".globl mod_twice\n.def mod_twice; .scl 2; .type 32; .endef\nmod_twice:\n"
        "  push rbx\n  sub rsp, 0x20\n  mov eax, ecx\n  add eax, 1\n  add rsp, 0x20\n  pop rbx\n  ret\n"
        "  int3\n  int3\n  int3\n  int3\n  int3\n  int3\n  int3\n  int3\n  int3\n  int3\n  int3\n  int3\n  int3\n  int3\n"
        "  int3\n  int3\n  int3\n  int3\n  int3\n  int3\n  int3\n  int3\n  int3\n  int3\n  int3\n  int3\n  int3\n  int3\n"
        ".globl mod_marked\n.def mod_marked; .scl 2; .type 32; .endef\nmod_marked:\n"
        "  mov r10d, 0x1A2B3C4D\n  mov eax, ecx\n  add eax, 7\n  ret\n"
        "  int3\n  int3\n  int3\n  int3\n  int3\n  int3\n  int3\n  int3\n  int3\n  int3\n  int3\n  int3\n  int3\n  int3\n");

// Read-only data inside the module: an address in it is no hook target.
extern "C" const unsigned char mod_data[64] = {1, 2, 3, 4};

struct HooksModuleInfo {
    void* add;
    void* add_mid;
    void* twice;
    void* marked;
    const void* data;
};

extern "C" __declspec(dllexport) void hooks_module_info(HooksModuleInfo* out) {
    out->add = reinterpret_cast<void*>(&mod_add);
    out->add_mid = mod_add_mid;
    out->twice = reinterpret_cast<void*>(&mod_twice);
    out->marked = reinterpret_cast<void*>(&mod_marked);
    out->data = mod_data;
}
