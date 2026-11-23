// SPDX-License-Identifier: MIT
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Text;
namespace Kiln.TestKit;

internal sealed class UiNativeChestState
{
    // Protected by UiValuesFakeState.Lock.
    internal readonly Dictionary<ulong, string> Captions = [];
    internal readonly Dictionary<ulong, int> Faults = [];
}
internal sealed unsafe class UiNativeChestFake : IKitFake
{
    internal const uint List = 13;
    public string Service => UiNativeChest.Service;
    private static UiNativeChest.Table* _table;
    public void Register()
    {
        if (_table == null)
        {
            _table = (UiNativeChest.Table*)NativeMemory.AllocZeroed((nuint)sizeof(UiNativeChest.Table));
            _table->Header = new() { Size = (uint)sizeof(UiNativeChest.Table), Version = 1 };
            _table->Subscribe = &Subscribe; _table->Unsubscribe = &Unsubscribe;
        }
        KitBackend.Instance.RegisterTable(Service, (nint)_table);
        FakeTables.Features([Ui.Inventory.ChestButtonCapability]);
    }
    public void Start(KilnHarness h) { }
    public void BeforeStep(KilnHarness h, ulong step) { }
    public void End(KilnHarness h) { }
    public void AddCoverage(List<CoverageRow> rows) => rows.Add(new("Native chest buttons (since 0.2.17)", CoverageKind.RecordingFake,
        [Service, Ui.Inventory.ChestButtonCapability], "AddChestButton registrations share the ui.values owner budget; four active captions, unique and length-checked. UiChest.Buttons/Press model a storage chest's native actions and invoke the real managed callback with the current container; disposal and five faults remove it. Not modelled: native row drawing, focus/navigation, native text budgets, game thread/session timing or hooks."));
    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int Subscribe(ulong owner, NativeStr caption, delegate* unmanaged[Cdecl]<UiNativeChest.Press*, void*, int> fn,
        void* user, UiValuesNative.Registration* r)
    {
        if (r == null || r->Size < sizeof(UiValuesNative.Registration)) return 1;
        r->Ok = 0; r->Reason = 0; r->Handle = 0;
        if (owner == 0 || fn == null || caption.Ptr == null || caption.Len == 0 || caption.Len > 128) return 1;
        string text;
        try { text = new UTF8Encoding(false, true).GetString(caption.Ptr, (int)caption.Len); }
        catch (DecoderFallbackException) { return 1; }
        if (text.Contains('\0')) return 1;
        if (FakeTables.Active(UiNativeChest.Service) is not { } harness || FakeTables.Off(Ui.Inventory.ChestButtonCapability) || UiValuesFake.State is not { } s) return 3;
        var state = KitFakes.State<UiNativeChestState>(harness);
        lock (s.Lock)
        {
            var current = s.Regs.Where(x => x.List == List).ToArray();
            if (current.Length >= 4) { r->Reason = 8; return 0; }
            if (current.Any(x => state.Captions.TryGetValue(x.Handle, out var c) && c == text)) { r->Reason = 1; return 0; }
            var rc = UiValuesFake.Add(owner, List, 0, (void*)fn, user, (UiValuesAbi.Registration*)r);
            if (rc == 0 && r->Ok != 0) state.Captions[r->Handle] = text;
            return rc;
        }
    }
    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int Unsubscribe(ulong handle)
    {
        if (KitBackend.Instance.Harness is not { } h || UiValuesFake.State is not { } s) return 3;
        lock (s.Lock)
        {
            var state = KitFakes.State<UiNativeChestState>(h);
            state.Captions.Remove(handle); state.Faults.Remove(handle);
            return s.Regs.RemoveAll(r => r.List == List && r.Handle == handle) != 0 ? 0 : 2;
        }
    }
}
public sealed partial class KilnHarness
{
    /// <summary>Offline native chest-button registrations and activations. Since 0.2.17.</summary>
    public HarnessUiChest UiChest => new(this);
}
/// <summary>Models registrations and callbacks, not the game's row renderer. Since 0.2.17.</summary>
public sealed class HarnessUiChest(KilnHarness harness)
{
    private UiValuesFakeState Values => KitFakes.State<UiValuesFakeState>(harness);
    private UiNativeChestState State => KitFakes.State<UiNativeChestState>(harness);
    private uint Container
    {
        get
        {
            if (FakeTables.Off(Ui.Inventory.ChestButtonCapability)) return 0;
            var inventory = KitFakes.State<UiInventoryFakeState>(harness);
            lock (inventory.Lock) return inventory.Screen == HudScreen.StorageChest ? inventory.Container : 0;
        }
    }
    /// <summary>Captions shown for the open storage chest, in registration order. Empty for any other screen or an unknown container. Since 0.2.17.</summary>
    public IReadOnlyList<string> Buttons
    {
        get
        {
            if (Container == 0) return [];
            var values = Values;
            lock (values.Lock) return values.Regs.Where(r => r.List == UiNativeChestFake.List).Select(r => State.Captions[r.Handle]).ToArray();
        }
    }
    /// <summary>Activates one visible button by its zero-based registration order. False: no chest/action or callback fault.
    /// Runs synchronously on the calling thread, not a game UI thread. Since 0.2.17.</summary>
    public unsafe bool Press(int index)
    {
        ArgumentOutOfRangeException.ThrowIfNegative(index);
        var container = Container;
        if (container == 0) return false;
        var values = Values;
        UiValuesFakeState.Reg rule;
        lock (values.Lock)
        {
            var buttons = values.Regs.Where(r => r.List == UiNativeChestFake.List).ToArray();
            if (index >= buttons.Length) return false;
            rule = buttons[index];
        }
        var press = new UiNativeChest.Press { Size = (uint)sizeof(UiNativeChest.Press), Container = container, Session = 1 };
        var rc = ((delegate* unmanaged[Cdecl]<UiNativeChest.Press*, void*, int>)rule.Fn)(&press, (void*)rule.User);
        lock (values.Lock)
        {
            if (rc == 0) State.Faults.Remove(rule.Handle);
            else if (rc == 7 && values.Regs.Contains(rule))
            {
                State.Faults.TryGetValue(rule.Handle, out var n);
                if (n + 1 >= 5) { values.Regs.Remove(rule); State.Faults.Remove(rule.Handle); State.Captions.Remove(rule.Handle); }
                else State.Faults[rule.Handle] = n + 1;
            }
        }
        return rc == 0;
    }
}
