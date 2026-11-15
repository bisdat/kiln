// SPDX-License-Identifier: MIT
using Kiln;
using Kiln.Sigs.Items;

namespace TypedHookVolume;

/// <summary>A typed hook of the game function items/GetItemVolume (OptS32 GetItemVolume(const ItemInfo* item), its struct
/// returned through a hidden buffer): the game answers first (<c>CallOriginal</c>), then the handler doubles the
/// volume it gave. The game reads that volume when a fuel transfer (a watering can at a well) is capped, so a can holds
/// twice the water. The handler runs on the thread that calls the function; it touches only the call in flight.</summary>
public sealed class TypedHookVolumeMod : Mod
{
    private HookHandle? _hook;
    private int _enabled = 1;
    private long _controlCalls, _enabledCalls, _enabledEdits;

    public override void OnLoad()
    {
        Context.Messages.On("typed-proof", m =>
        {
            if (m.Sender != "client-pass" || m.Payload is not long[] { Length: 2 } q) return;
            if (q[1] is 0 or 1) Volatile.Write(ref _enabled, (int)q[1]);
            // the hook's state: 1 installed, 0 pending (installs at a safe tick), 2 refused, 3 removed, -1 no hook asked for
            Mods.Send(m.Sender, "typed-proof-result", new long[] { q[0], Volatile.Read(ref _enabled),
                Interlocked.Read(ref _controlCalls), Interlocked.Read(ref _enabledCalls), Interlocked.Read(ref _enabledEdits),
                _hook is { } h ? (long)h.State : -1 });
        });
        // the typed link kind, and this function's descriptor fitting its code on this game build
        if (!Capabilities.Is("hooks.typed") || !Capabilities.Is(GetItemVolume.Capability))
        {
            Log.Warn($"typed hooks are off here: {Capabilities.Why("hooks.typed")}; {Capabilities.Why(GetItemVolume.Capability)}");
            return;
        }
        var r = GetItemVolume.Detour(Doubled);
        if (!r.Ok)
        {
            Log.Warn($"GetItemVolume was not hooked: {r.Reason}");  // a game update changed the function
            return;
        }
        _hook = r.Handle;
        // Pending: asked for during a step, it goes in at the next safe tick (a late refusal is logged by Kiln and its
        // handle's State says Refused, with the Reason)
        Log.Info(r.Pending ? "every item's volume is doubled once the hook goes in at the next safe tick"
                           : $"every item's volume doubled (GetItemVolume of a null item: {GetItemVolume.Invoke(0).Value})");
    }

    private void Doubled(ref GetItemVolume.Call c)
    {
        var enabled = Volatile.Read(ref _enabled) != 0;
        c.CallOriginal();
        if (c.Item == 0 || c.Result.Has == 0) return;
        if (!enabled) { Interlocked.Increment(ref _controlCalls); return; }
        Interlocked.Increment(ref _enabledCalls);
        var before = c.Result.Value;
        c.Result.Value *= 2;
        if (c.Result.Value != before) Interlocked.Increment(ref _enabledEdits);
    }

    public override void OnUnload() => _hook?.Remove();  // also removed by Kiln when the mod unloads
}
