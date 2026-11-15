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

    public override void OnLoad()
    {
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
        Log.Info($"every item's volume doubled (GetItemVolume of a null item: {GetItemVolume.Invoke(0).Value})");
    }

    private static void Doubled(ref GetItemVolume.Call c)
    {
        c.CallOriginal();
        if (c.Item != 0 && c.Result.Has != 0) c.Result.Value *= 2;  // a real item that has a volume
    }

    public override void OnUnload() => _hook?.Remove();  // also removed by Kiln when the mod unloads
}
