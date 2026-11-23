// SPDX-License-Identifier: MIT
using System.Collections.Concurrent;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Text;
namespace Kiln;

public static partial class Ui
{
    public static partial class Inventory
    {
        /// <summary>A game-drawn button in the storage chest's own action row. Since 0.2.17.</summary>
        public const string ChestButtonCapability = "ui.chest.buttons";

        /// <summary>Adds a native game entry to the storage chest's action row, not a Kiln overlay panel. Its caption is
        /// literal UTF-8 (1..128 bytes); dispose and register again to change/localize it. Four active buttons total;
        /// captions must be unique among them. Uses the game's entry drawing, input and focus; no vanilla input action is
        /// repurposed. The callback runs on the game's UI thread after the chest builder, with that chest's entity.
        /// A client request is never authority: the server must validate access and distance before acting. Not shown on
        /// commerce/factory screens or while the chest identity is unknown. Dispose/unload removes it; five consecutive
        /// callback faults also remove it. Visual placement and controller navigation remain unverified in-game.
        /// Since 0.2.17.</summary>
        [ClientOnly, AnyThread, RequiresCapability(ChestButtonCapability)]
        public static IDisposable AddChestButton(string caption, Action<InventoryScreen> handler) => UiNativeChest.Subscribe(caption, handler);
    }
}

internal static unsafe class UiNativeChest
{
    internal const string Service = "kiln.ui.chest";
    [StructLayout(LayoutKind.Sequential)]
    internal struct Press { public uint Size, Container, LocalPlayer, Reserved; public ulong Session, Frame; }
    [StructLayout(LayoutKind.Sequential)]
    internal struct Table
    {
        public ServiceHeader Header;
        public delegate* unmanaged[Cdecl]<ulong, NativeStr, delegate* unmanaged[Cdecl]<Press*, void*, int>, void*, UiValuesNative.Registration*, int> Subscribe;
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
    private sealed record Button(Action<InventoryScreen> Fn, ModContext? Mod)
    { public ulong Handle; public int Faults; public Registration? Registration; }
    private static readonly ConcurrentDictionary<long, Button> Buttons = new();
    private static long _next;

    internal static IDisposable Subscribe(string caption, Action<InventoryScreen> handler)
    {
        ArgumentNullException.ThrowIfNull(caption); ArgumentNullException.ThrowIfNull(handler);
        if (string.IsNullOrWhiteSpace(caption) || caption.Contains('\0') || Encoding.UTF8.GetByteCount(caption) > 128)
            throw new ArgumentException("caption must contain 1..128 UTF-8 bytes without NUL", nameof(caption));
        UiValueBridge.RequireCapability(Ui.Inventory.ChestButtonCapability);
        var t = T;
        if (t == null) throw new KilnUnavailableException(Ui.Inventory.ChestButtonCapability, "kiln.ui.chest is not available");
        var mod = UiModCalls.Caller ?? ModContext.OwnerOf(handler);
        UiModCalls.RequireAlive(mod);
        var id = Interlocked.Increment(ref _next);
        var button = new Button(handler, mod);
        Buttons[id] = button;
        var r = new UiValuesNative.Registration { Size = (uint)sizeof(UiValuesNative.Registration) };
        var rc = t->Subscribe(UiValueBridge.OwnerOf(mod), Utf8.Of(caption), &OnPress, (void*)id, &r);
        if (rc != 0 || r.Ok == 0)
        {
            Buttons.TryRemove(id, out _);
            if (rc == UiValuesNative.Unavailable) throw new KilnUnavailableException(Ui.Inventory.ChestButtonCapability, "native chest row is unavailable");
            throw new InvalidOperationException($"AddChestButton refused: {UiValueBridge.ReasonText(r.Reason)} ({rc}); captions must be unique, at most four buttons");
        }
        button.Handle = r.Handle;
        var handle = new Registration(id, mod, button); button.Registration = handle;
        if (mod is not null) { mod.Retiring += handle.Dispose; if (!mod.Alive) handle.Dispose(); }
        if (!Buttons.ContainsKey(id)) handle.Dispose();
        return handle;
    }
    private sealed class Registration(long id, ModContext? mod, Button button) : IDisposable
    {
        private int _done;
        public void Dispose()
        {
            if (Interlocked.Exchange(ref _done, 1) != 0) return;
            if (mod is not null) mod.Retiring -= Dispose;
            Buttons.TryRemove(id, out _);
            if (button.Handle != 0 && T != null) T->Unsubscribe(button.Handle);
        }
    }
    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int OnPress(Press* p, void* user)
    {
        try
        {
            if (p == null || p->Size < sizeof(Press) || p->Container == 0) return UiValuesNative.InvalidArgument;
            if (!Buttons.TryGetValue((long)user, out var b) || b.Mod is { Alive: false }) return 0;
            var screen = new InventoryScreen(InventoryScreenKind.Chest, HudScreen.StorageChest, new EntityId(p->Container));
            if (!UiModCalls.Invoke(b.Mod, b.Fn, screen, "native chest button"))
            {
                if (Interlocked.Increment(ref b.Faults) >= 5)
                { Buttons.TryRemove((long)user, out _); b.Registration?.Dispose(); }
                return UiValuesNative.Internal;
            }
            b.Faults = 0; return 0;
        }
        catch { return UiValuesNative.Internal; }
    }
}
