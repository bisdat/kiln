// SPDX-License-Identifier: MIT
using System.Collections.Concurrent;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;

namespace Kiln;

public static partial class Ui
{
    public static partial class Inventory
    {
        /// <summary>Native background colour rules for the game's own inventory slots. Since 0.2.17.</summary>
        public const string TintCapability = "ui.slots.tint";

        /// <summary>Changes the game's own slot background, not an overlay mark. The rule sees a copied item view on the
        /// game's UI thread; return null to leave the earlier colour. Includes empty slots and stacks without an entity:
        /// match InventoryPage/SlotIndex when needed. Rules run in registration order; later colours win. The game's
        /// colour strength, rarity art, icon, focus and input remain its own. Dispose or unload to remove the rule.
        /// Five consecutive exceptions remove it. No gameplay writes or network authority. Since 0.2.17.</summary>
        [ClientOnly, AnyThread, RequiresCapability(TintCapability)]
        public static IDisposable TintWhen(Func<UiItemView, Color?> rule, ItemId item = default) => UiNativeTints.Subscribe(rule, item);
    }
}

internal static unsafe class UiNativeTints
{
    internal const string Service = "kiln.ui.tints";
    [StructLayout(LayoutKind.Sequential)]
    internal struct Table
    {
        public ServiceHeader Header;
        public delegate* unmanaged[Cdecl]<ulong, uint, delegate* unmanaged[Cdecl]<UiValuesNative.ItemView*, uint*, void*, int>, void*, UiValuesNative.Registration*, int> Subscribe;
        public delegate* unmanaged[Cdecl]<ulong, int> Unsubscribe;
    }
    internal static Table* T
    {
        get
        {
            var t = Services.Get<Table>(Service, 1);
            return t != null && t->Header.Size >= sizeof(Table) && t->Subscribe != null && t->Unsubscribe != null ? t : null;
        }
    }
    private sealed record Rule(Func<UiItemView, Color?> Fn, ModContext? Mod) { public ulong Handle; public int Faults; public Registration? Registration; }
    private static readonly ConcurrentDictionary<long, Rule> Rules = new();
    private static long _next;

    internal static IDisposable Subscribe(Func<UiItemView, Color?> rule, ItemId item)
    {
        ArgumentNullException.ThrowIfNull(rule);
        UiValueBridge.RequireCapability(Ui.Inventory.TintCapability);
        var t = T;
        if (t == null) throw new KilnUnavailableException(Ui.Inventory.TintCapability, "kiln.ui.tints is not available");
        var mod = UiModCalls.Caller ?? ModContext.OwnerOf(rule);
        UiModCalls.RequireAlive(mod);
        var id = Interlocked.Increment(ref _next);
        var entry = new Rule(rule, mod);
        Rules[id] = entry;
        var r = new UiValuesNative.Registration { Size = (uint)sizeof(UiValuesNative.Registration) };
        var rc = t->Subscribe(UiValueBridge.OwnerOf(mod), item.Value, &OnTint, (void*)id, &r);
        if (rc != 0 || r.Ok == 0)
        {
            Rules.TryRemove(id, out _);
            if (rc == UiValuesNative.Unavailable) throw new KilnUnavailableException(Ui.Inventory.TintCapability, "native slot appearance is unavailable");
            throw new InvalidOperationException($"TintWhen refused: {UiValueBridge.ReasonText(r.Reason)} ({rc})");
        }
        entry.Handle = r.Handle;
        var handle = new Registration(id, mod, entry);
        entry.Registration = handle;
        if (mod is not null) { mod.Retiring += handle.Dispose; if (!mod.Alive) handle.Dispose(); }
        if (!Rules.ContainsKey(id)) handle.Dispose();
        return handle;
    }
    private sealed class Registration(long id, ModContext? mod, Rule rule) : IDisposable
    {
        private int _done;
        public void Dispose()
        {
            if (Interlocked.Exchange(ref _done, 1) != 0) return;
            if (mod is not null) mod.Retiring -= Dispose;
            Rules.TryRemove(id, out _);
            if (rule.Handle != 0 && T != null) T->Unsubscribe(rule.Handle);
        }
    }
    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int OnTint(UiValuesNative.ItemView* v, uint* rgba, void* user)
    {
        try
        {
            if (v == null || v->Size < sizeof(UiValuesNative.ItemView) || rgba == null) return UiValuesNative.InvalidArgument;
            if (!Rules.TryGetValue((long)user, out var r) || r.Faults >= 5 || r.Mod is { Alive: false }) return 0;
            Color? color = null;
            if (!UiModCalls.Invoke(r.Mod, item => color = r.Fn(item), UiValueBridge.ViewOf(v), "native slot tint"))
            {
                if (Interlocked.Increment(ref r.Faults) >= 5)
                {
                    Rules.TryRemove((long)user, out _);
                    r.Registration?.Dispose();
                }
                return UiValuesNative.Internal;
            }
            r.Faults = 0;
            if (color is { } c) *rgba = c.Rgba;
            return 0;
        }
        catch { return UiValuesNative.Internal; }
    }
}
