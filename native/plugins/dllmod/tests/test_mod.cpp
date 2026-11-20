// SPDX-License-Identifier: MIT
// Test packages for kiln-dllmod's host test (tests/host_test.cpp), written for Kiln from the package interface (ABI 1):
// exports EnshroudedModConfigure / EnshroudedModInitialize with C linkage, return 1 for success. Variants:
//   (none)        a mod with settings: Configure must come first, for the same world Initialize then gets
//   TM_PLAIN      a mod without settings (Initialize only)
//   TM_FAIL       Initialize returns 0
//   TM_FAULT      Initialize faults
//   TM_LIB        a library package (no Initialize): exports dllmod_test_lib_value
//   TM_USES_LIB   a mod that imports dllmod_test_lib_value from the library DLL (loads only after it)
// test_mod_report() says what the host did: "C<world>|<settings>" for Configure, "I<size>,<abi>,<world>" for
// Initialize, in call order.
#include <windows.h>

#include <cstdint>
#include <cstring>
#include <string>

struct Context {
    uint32_t size;
    uint32_t abi_version;
    const char* world_id;
    void(__cdecl* write_log)(const char*);
};

static std::string g_report;
static char g_world[33] = {};
static bool g_configured = false;

extern "C" __declspec(dllexport) const char* test_mod_report() { return g_report.c_str(); }

#if defined(TM_LIB)
extern "C" __declspec(dllexport) int dllmod_test_lib_value() { return 42; }
#else

#if defined(TM_USES_LIB)
extern "C" __declspec(dllimport) int dllmod_test_lib_value();
#endif

#if !defined(TM_PLAIN) && !defined(TM_FAIL) && !defined(TM_FAULT) && !defined(TM_USES_LIB)
extern "C" __declspec(dllexport) int __cdecl EnshroudedModConfigure(const char* world_id, const char* settings) {
    if (!world_id || !settings || strnlen(world_id, 33) != 32) return 0;
    std::memcpy(g_world, world_id, 33);
    g_configured = true;
    g_report += std::string("C") + world_id + "|" + settings + ";";
    return 1;
}
#endif

extern "C" __declspec(dllexport) int __cdecl EnshroudedModInitialize(const Context* ctx) {
    if (!ctx || ctx->size < sizeof(Context) || ctx->abi_version != 1 || !ctx->world_id || !ctx->write_log) return 0;
    g_report += "I" + std::to_string(ctx->size) + "," + std::to_string(ctx->abi_version) + "," + ctx->world_id + ";";
#if !defined(TM_PLAIN) && !defined(TM_FAIL) && !defined(TM_FAULT) && !defined(TM_USES_LIB)
    if (!g_configured || std::strcmp(g_world, ctx->world_id) != 0) return 0;
#endif
#if defined(TM_FAULT)
    volatile int* p = nullptr;
    *p = 1;
#endif
#if defined(TM_FAIL)
    ctx->write_log("test mod refuses to start");
    return 0;
#endif
#if defined(TM_USES_LIB)
    if (dllmod_test_lib_value() != 42) return 0;
    ctx->write_log("the library answered 42");
#endif
    ctx->write_log("test mod started\r\n");
    return 1;
}
#endif
