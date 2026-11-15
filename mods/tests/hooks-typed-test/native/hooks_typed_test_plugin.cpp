// SPDX-License-Identifier: MIT
// hooks-typed-test (native part): typed links of kiln.hooks (detour_typed, chapter 06b) on two game functions the
// server world's setup calls before any managed mod loads: abilities/RegisterNative (five arguments, the fifth, a u8
// argc, on the caller's stack) and time/SetTimeScale (a float in xmm1). A plugin for a lab server only: copy
// kiln-hooks-typed-test.dll to <lab>/kiln/plugins/. The handlers only read the frame and let the game's function run
// (KILN_TYPED_CONTINUE). At the first server Pre the plugin writes what it saw to kiln/logs/hooks-typed-test-boot.json
// (the managed part compares the argc values with tools/impact/data/natives.json) and revokes both links. Every line it
// writes starts with "hooks-typed-test (native):".
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>

#include "kiln_api.h"
#include "services/kiln_hooks.h"

namespace {

const KilnApi* g_api = nullptr;
KilnOwner g_owner = 0;
const KilnHooksApi* H = nullptr;
KilnHandle g_reg_link = 0, g_scale_link = 0;
std::string g_install;  // what the installs said, for the file

KilnStr ks(const char* s) { return KilnStr{s, std::strlen(s)}; }

void say(const char* fmt, ...) {
    char b[1200];
    int n = std::snprintf(b, sizeof b, "hooks-typed-test (native): ");
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(b + n, sizeof b - n, fmt, ap);
    va_end(ap);
    g_api->log(g_owner, KILN_LOG_INFO, KilnStr{b, std::strlen(b)});
}

// RegisterNative's calls: the hash (rdx) and the fifth argument, a u8 on the caller's stack (its upper bytes are the
// caller's garbage: masked here, as the generated managed code masks it)
constexpr int kMax = 512;
struct Reg {
    uint32_t hash;
    uint8_t argc;
    uint64_t raw;
};
Reg g_regs[kMax];
std::atomic<int> g_nregs{0};
std::atomic<int> g_reg_threads_differ{0};
DWORD g_reg_tid = 0;

struct Scale {
    uint64_t daytime;
    float scale;
    int64_t now;
};
Scale g_scales[16];
std::atomic<int> g_nscales{0};

uintptr_t KILN_CALL on_register_native(KilnCallFrame* f, void*) {
    const int i = g_nregs.fetch_add(1);
    if (i < kMax) g_regs[i] = Reg{static_cast<uint32_t>(f->gpr[1]), static_cast<uint8_t>(f->stack[0]), f->stack[0]};
    const DWORD tid = GetCurrentThreadId();
    if (i == 0) g_reg_tid = tid;
    else if (tid != g_reg_tid) g_reg_threads_differ.fetch_add(1);
    return KILN_TYPED_CONTINUE;
}

uintptr_t KILN_CALL on_set_time_scale(KilnCallFrame* f, void*) {
    const int i = g_nscales.fetch_add(1);
    float scale;
    std::memcpy(&scale, f->xmm[1], 4);
    if (i < 16) g_scales[i] = Scale{f->gpr[0], scale, static_cast<int64_t>(f->gpr[2])};
    return KILN_TYPED_CONTINUE;
}

// the descriptors as sigdb/symbols/server.d/dev28-06b-typed-hooks.json has them
KilnSigArg kRegArgs[] = {{ks("registry"), KILN_SIG_PTR, 8}, {ks("hash"), KILN_SIG_U32, 4}, {ks("fn"), KILN_SIG_PTR, 8},
                         {ks("name"), KILN_SIG_PTR, 8}, {ks("argc"), KILN_SIG_U8, 1}};
KilnSigArg kScaleArgs[] = {{ks("daytime"), KILN_SIG_PTR, 8}, {ks("scale"), KILN_SIG_F32, 4}, {ks("ecsNow"), KILN_SIG_S64, 8}};

void install(const char* symbol, KilnSigArg* args, uint32_t argc, uint32_t ret_kind, uint32_t ret_size, KilnTypedFn fn,
             KilnHandle* out) {
    uintptr_t target = 0;
    if (H->symbol(ks(symbol), &target) != KILN_OK || !target) {
        say("FAIL %s does not resolve", symbol);
        g_install += std::string("\"") + symbol + "\": \"unresolved\", ";
        return;
    }
    KilnSig sig{};
    sig.size = sizeof sig;
    sig.argc = argc;
    sig.args = args;
    sig.ret_kind = ret_kind;
    sig.ret_size = ret_size;
    KilnHookOrder order{};
    order.size = sizeof order;
    order.label = ks(symbol);
    const KilnResult r = H->detour_typed(g_owner, target, &order, &sig, fn, nullptr, out);
    if (r == KILN_OK) {
        say("typed link on %s at rva 0x%llx installed at boot", symbol, static_cast<unsigned long long>(target - H->image_base()));
        g_install += std::string("\"") + symbol + "\": \"ok\", ";
        return;
    }
    char why[600] = "";
    size_t len = 0;
    if (H->typed_refusal(why, sizeof why, &len) != KILN_OK) why[0] = 0;
    say("FAIL typed link on %s refused (%d): %s", symbol, static_cast<int>(r), why);
    g_install += std::string("\"") + symbol + "\": \"refused\", ";
}

bool g_written = false;

void KILN_CALL on_pre(const KilnTickInfo* tick, void*) {
    if (g_written || tick->world_kind != 0) return;
    g_written = true;
    // the links did their part at setup: revoke them (owner-scoped; the continuation stays valid for calls in flight)
    if (g_reg_link) H->revoke(g_reg_link);
    if (g_scale_link) H->revoke(g_scale_link);
    char dir[1024];
    size_t len = 0;
    if (g_api->path(KILN_PATH_LOGS, dir, sizeof dir - 1, &len) != KILN_OK || len >= sizeof dir - 64) {
        say("FAIL no logs folder");
        return;
    }
    dir[len] = 0;
    std::string path = std::string(dir) + "\\hooks-typed-test-boot.json";
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) {
        say("FAIL cannot write %s", path.c_str());
        return;
    }
    const int n = std::min(g_nregs.load(), kMax);
    std::fprintf(f, "{\"install\": {%s\"done\": true}, \"register_native_calls\": %d, \"register_native_other_threads\": %d,\n",
                 g_install.c_str(), g_nregs.load(), g_reg_threads_differ.load());
    std::fprintf(f, " \"register_native\": [");
    for (int i = 0; i < n; ++i)
        std::fprintf(f, "%s{\"hash\": \"0x%08x\", \"argc\": %u, \"raw\": \"0x%llx\"}", i ? ", " : "", g_regs[i].hash, g_regs[i].argc,
                     static_cast<unsigned long long>(g_regs[i].raw));
    std::fprintf(f, "],\n \"set_time_scale\": [");
    const int m = std::min(g_nscales.load(), 16);
    for (int i = 0; i < m; ++i)
        std::fprintf(f, "%s{\"daytime\": \"0x%llx\", \"scale\": %.4f, \"now\": %lld}", i ? ", " : "",
                     static_cast<unsigned long long>(g_scales[i].daytime), g_scales[i].scale, static_cast<long long>(g_scales[i].now));
    std::fprintf(f, "]}\n");
    std::fclose(f);
    say("at the first server step: RegisterNative ran %d time(s) through the typed link (%d on another thread than the "
        "first), SetTimeScale %d time(s); written to %s", g_nregs.load(), g_reg_threads_differ.load(), g_nscales.load(), path.c_str());
}

}  // namespace

extern "C" __declspec(dllexport) KilnResult KILN_CALL kiln_plugin_main(const KilnApi* api, KilnOwner owner, KilnPluginInfo* info) {
    if (!api || api->abi_version != KILN_ABI_VERSION) return KILN_ERR_UNAVAILABLE;
    g_api = api;
    g_owner = owner;
    info->id = ks("hooks-typed-test");
    info->version = ks("0.1.0");
    H = static_cast<const KilnHooksApi*>(api->service(ks(KILN_HOOKS_SERVICE), 2));
    if (!H || H->header.size < offsetof(KilnHooksApi, typed_refusal) + sizeof(void*)) {
        H = nullptr;
        say("FAIL kiln.hooks has no typed links (table too small)");
        return KILN_OK;
    }
    if (!api->feature(ks("hooks.typed"))) say("FAIL the capability hooks.typed is off");
    install("abilities/RegisterNative", kRegArgs, 5, KILN_SIG_BOOL, 1, &on_register_native, &g_reg_link);
    install("time/SetTimeScale", kScaleArgs, 3, KILN_SIG_VOID, 0, &on_set_time_scale, &g_scale_link);
    KilnHandle h = 0;
    api->subscribe_tick(owner, KILN_TICK_PRE, &on_pre, nullptr, &h);
    return KILN_OK;
}
