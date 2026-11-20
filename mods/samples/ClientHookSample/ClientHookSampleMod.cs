// SPDX-License-Identifier: MIT
using Kiln;
using OwnVolume = Kiln.Sigs.ClientHookSample.ItemVolume;

namespace ClientHookSample;

/// <summary>
/// A client mod's own typed hook (docs/kiln/modding/client-hooks.md), the advanced tier: the mod's sigdb/client.json names
/// the game client's GetItemVolume by its bytes and describes its signature; Kiln checks both against the running exe
/// (Symbols.LoadShipped), and Kiln.Sigs.ClientHookSample.ItemVolume is the class the build generated from it. The handler
/// lets the game answer and only counts the calls: what a client hook changes stays in this player's game (the server
/// and other players never see it), so a real client hook changes what this game shows or how it feels, never what the
/// server decides. Prefer a Kiln API where one exists (Ui.Inventory, Ui.Screens, Ui.Values, registries): Kiln keeps
/// those working across game updates; this fragment has to be updated by the mod's author after one.
/// </summary>
public sealed class ClientHookSampleMod : Mod
{
    private HookHandle? _hook;
    private long _calls;

    public override void OnLoad()
    {
        if (!Capabilities.Is("hooks.typed"))
        {
            Log.Warn($"client-hook-sample: typed hooks are off here: {Capabilities.Why("hooks.typed")}");
            return;
        }
        try
        {
            var s = Symbols.LoadShipped();  // sigdb/client.json in a game client (sigdb/server.json on a server)
            Log.Info($"client-hook-sample: own symbols {s.SymbolsOk}/{s.Symbols} resolved");
        }
        catch (Exception e)
        {
            Log.Warn($"client-hook-sample: symbol fragment not loaded: {e.Message}");
            return;
        }
        if (!Capabilities.Is(OwnVolume.Capability))
        {
            // the descriptor does not fit this game build (a game update): Kiln refuses, nothing is patched
            Log.Warn($"client-hook-sample: {OwnVolume.Capability} is off: {Capabilities.Why(OwnVolume.Capability)}");
            return;
        }
        var r = OwnVolume.Detour(Count);
        if (!r.Ok)
        {
            Log.Warn($"client-hook-sample: not hooked: {r.Reason}");
            return;
        }
        _hook = r.Handle;
        Log.Info(r.Pending ? "client-hook-sample: the hook goes in at the next safe tick (every thread checked first)"
                           : "client-hook-sample: hooked");
    }

    // Runs on the thread that calls the function (a game worker too): touch only the call in flight and your own fields.
    private void Count(ref OwnVolume.Call c)
    {
        c.CallOriginal();  // the game answers; its result goes back unchanged
        var n = Interlocked.Increment(ref _calls);
        if (n == 1 || n % 100 == 0)
            Log.Info($"client-hook-sample: GetItemVolume called {n} time(s) in this game; the last answer {c.Result.Value} (has {c.Result.Has})");
    }

    public override void OnUnload()
    {
        _hook?.Dispose();  // Kiln removes it with the mod anyway
        Log.Info($"client-hook-sample: {Interlocked.Read(ref _calls)} call(s) seen");
    }
}
