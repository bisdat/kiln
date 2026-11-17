# One DLL for Kiln and for Ultimate ASI Loader

Kiln does not hand its `KilnApi` to a DLL it did not load. The API arrives through `kiln_native_init` (`native.json`)
or `kiln_plugin_main` (`kiln/plugins`), with an owner Kiln admits and can revoke. A DLL that another loader brought in
has no such owner. Ship a `native.json` so Kiln loads the DLL, and keep the ASI entry for a PC where Kiln is not
installed. One of those two starts the mod.

## Tell the two entries apart

`KilnProxyInfo` is an export of Kiln's proxy (`dinput8.dll`, abi 2 or later). It does not give you the API. It only
says Kiln's proxy is in the process.

```c
#include "kiln_api.h"
#include "kiln_native.h"

static HANDLE g_started;

static int already_started(void) {
    /* the name is this mod's; Kiln does not define one */
    HANDLE held = CreateMutexA(NULL, FALSE, "Local\\YourMod.OneStart");
    if (!held) return 1;
    if (GetLastError() == ERROR_ALREADY_EXISTS) { CloseHandle(held); return 1; }
    g_started = held;
    return 0;
}

/* Kiln calls this when native.json names it. This is the start under Kiln. */
__declspec(dllexport) int KILN_CALL kiln_native_init(const KilnNativeContext* ctx) {
    if (!ctx || ctx->size < sizeof(KilnNativeContext) || !ctx->api) return KILN_NATIVE_INIT_UNSUPPORTED;
    if (already_started()) return KILN_NATIVE_INIT_OK;
    const KilnApi* api = ctx->api;
    /* api->service("kiln.stations", 1), and the rest, with ctx->owner */
    (void)api;
    return KILN_NATIVE_INIT_OK;
}

/* Ultimate ASI Loader calls this when Kiln did not load the DLL. */
__declspec(dllexport) void InitializeASI(void) {
    if (already_started()) return;                 /* kiln_native_init already started it */
    HMODULE proxy = GetModuleHandleA("dinput8.dll");
    KilnProxyInfoFn info = proxy ? (KilnProxyInfoFn)GetProcAddress(proxy, KILN_PROXY_INFO) : NULL;
    if (info && info() && info()->abi >= KILN_PROXY_ABI) {
        /* Kiln is here and did not load this DLL: the profile has no native.json for it.
           Do not look for KilnApi. Ask the player to enable the Kiln mod. */
        return;
    }
    /* no Kiln in the process: the ASI build's own start */
}
```

`native.json` lists that DLL and `"init": "kiln_native_init"`. Kiln calls it once, on the phase's thread, and the
context lives for the process ([native mods](../native-mods.md)). The mutex is taken there first, so a later
`InitializeASI` from an `.asi` copy of the same file returns immediately.

If the profile does not enable the native mod, Kiln's proxy can still be loaded and Ultimate ASI Loader can still load
the `.asi`. `KilnProxyInfo` is how that `.asi` sees Kiln and stays quiet instead of starting a second copy with no API.

A package that contains both the native mod and the same DLL as an `.asi` is imported as the native mod only
([engines](../engines.md)).
