# One DLL for Kiln and for Ultimate ASI Loader

Kiln does not hand its `KilnApi` to a DLL it did not load. The API arrives through `kiln_native_init` (`native.json`)
or `kiln_plugin_main` (`kiln/plugins`), with an owner Kiln admits and can revoke. A DLL that another loader brought in
has no such owner. Ship a `native.json` so Kiln loads the DLL, and keep the ASI entry for a PC where Kiln is not
installed. One of those two starts the mod.

## Tell the two entries apart

`KilnProxyInfo` is an export of Kiln's proxy (`dinput8.dll`, abi 2 or later). It does not give you the API. It only
says Kiln's proxy is in the process.

```c
#include <windows.h>

#include "kiln_api.h"
#include "kiln_native.h"

static int already_started(void) {
    /* the name is this mod's; Kiln does not define one */
    HANDLE held = CreateMutexA(NULL, FALSE, "Local\\YourMod.OneStart");
    if (!held) return 1;
    if (GetLastError() == ERROR_ALREADY_EXISTS) { CloseHandle(held); return 1; }
    return 0; /* the handle stays open for the life of the process: that keeps the name taken */
}

/* Kiln calls this when native.json names it. This is the start under Kiln. */
__declspec(dllexport) int KILN_CALL kiln_native_init(const KilnNativeContext* ctx) {
    if (!ctx || ctx->size < sizeof(KilnNativeContext) || !ctx->api) return KILN_NATIVE_INIT_UNSUPPORTED;
    if (already_started()) {
        /* Another copy runs already, without the API (the .asi build, started where Kiln's proxy was not in the
           process). Report it: KILN_NATIVE_INIT_OK would show the mod as running under Kiln. */
        static const char why[] = "another copy of this mod started first, without the Kiln API (its .asi build?)";
        ctx->api->log(ctx->owner, KILN_LOG_ERROR, (KilnStr){why, sizeof why - 1});
        return KILN_NATIVE_INIT_FAILED;
    }
    const KilnApi* api = ctx->api;
    /* api->service("kiln.stations", 1), and the rest, with ctx->owner */
    (void)api;
    return KILN_NATIVE_INIT_OK;
}

/* Ultimate ASI Loader calls this when Kiln did not load the DLL. */
__declspec(dllexport) void InitializeASI(void) {
    HMODULE proxy = GetModuleHandleA("dinput8.dll");
    KilnProxyInfoFn info = proxy ? (KilnProxyInfoFn)GetProcAddress(proxy, KILN_PROXY_INFO) : NULL;
    if (info && info() && info()->abi >= KILN_PROXY_ABI) {
        /* Kiln is in the process. Return before the mutex, so kiln_native_init can still start.
           Do not look for KilnApi. Ask the player to enable the Kiln mod. */
        return;
    }
    if (already_started()) return;                 /* this ASI entry already started it */
    /* no Kiln in the process: the ASI build's own start. The mutex is taken only here. */
}
```

`kiln_native_init` returns `KILN_NATIVE_INIT_FAILED` when the mutex is taken: Kiln marks the mod "failed", and
kiln.log has the line. Returning `KILN_NATIVE_INIT_OK` there would show it as running while it runs without the API.

`native.json` lists that DLL and `"init": "kiln_native_init"`. Kiln calls it once, on the phase's thread, and the
context lives for the process ([native mods](../native-mods.md)). That start takes the mutex. `InitializeASI` takes
it only when Kiln's proxy is not in the process and this entry is the one that starts. If the proxy is present it
returns first, without creating the mutex, so a later `kiln_native_init` can still start.

## Load order

The check in `InitializeASI` relies on Kiln's proxy being loaded before Ultimate ASI Loader. Kiln loads Ultimate ASI
Loader itself, as `xinput9_1_0.dll` next to the game ([engines](../engines.md)):
- The proxy (`dinput8.dll`, a DLL the game imports) loads it in its own `DllMain`, from `kiln/chain.ini`: `[chain]`
  when Kiln's installer moved it there, `[profile]` when a profile's `.asi` package brought it. The proxy loads
  `[chain]`, then `dinput8-chain.ini`, then `[profile]`.
- When `chain.ini [late]` names it, the proxy does not load it. `kiln-core.dll` loads it at the end of its boot, after
  Kiln's services, plugins and the native mods of the early and boot phases ran.

Either way the proxy is in the process before Ultimate ASI Loader starts, so `KilnProxyInfo` answers by the time it
calls `InitializeASI`. An Ultimate ASI Loader that is not in `kiln/chain.ini` (the game loads it through its own imports)
is outside this: then Windows decides the order, and the check can miss the proxy.

If the profile does not enable the native mod, Kiln's proxy can still be loaded and Ultimate ASI Loader can still load
the `.asi`. `KilnProxyInfo` is how that `.asi` sees Kiln and stays quiet instead of starting a second copy with no API.

A package that contains both the native mod and the same DLL as an `.asi` is imported as the native mod only
([engines](../engines.md)).
