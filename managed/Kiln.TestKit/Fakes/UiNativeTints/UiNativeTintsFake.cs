// SPDX-License-Identifier: MIT
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
namespace Kiln.TestKit;

internal sealed class UiNativeTintsState
{
    // Protected by UiValuesFakeState.Lock, along with its shared owner-attributed registrations.
    internal readonly Dictionary<ulong, int> Faults = [];
}
internal sealed unsafe class UiNativeTintsFake : IKitFake
{
    internal const uint List = 12;
    public string Service => UiNativeTints.Service;
    private static UiNativeTints.Table* _table;
    public void Register()
    {
        if (_table == null)
        {
            _table = (UiNativeTints.Table*)NativeMemory.AllocZeroed((nuint)sizeof(UiNativeTints.Table));
            _table->Header = new() { Size = (uint)sizeof(UiNativeTints.Table), Version = 1 };
            _table->Subscribe = &Subscribe; _table->Unsubscribe = &Unsubscribe;
        }
        KitBackend.Instance.RegisterTable(Service, (nint)_table);
        FakeTables.Features([Ui.Inventory.TintCapability]);
    }
    public void Start(KilnHarness h) { }
    public void BeforeStep(KilnHarness h, ulong step) { }
    public void End(KilnHarness h) { }
    public void AddCoverage(List<CoverageRow> rows) => rows.Add(new("Native slot tints (since 0.2.17)", CoverageKind.RecordingFake,
        [Service, Ui.Inventory.TintCapability], "TintWhen runs the managed rules in registration order over a copied slot view; null keeps the prior colour, item filters and disposal apply, five consecutive faults stop a rule. Not modelled: game hooks, native descriptor copying, colour strength, rarity art or drawing."));
    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int Subscribe(ulong owner, uint item, delegate* unmanaged[Cdecl]<UiValuesNative.ItemView*, uint*, void*, int> fn, void* user, UiValuesNative.Registration* r)
    {
        if (r == null || r->Size < sizeof(UiValuesNative.Registration)) return 1;
        r->Ok = 0; r->Reason = 0; r->Handle = 0;
        if (FakeTables.Active(UiNativeTints.Service) is null || FakeTables.Off(Ui.Inventory.TintCapability)) return 3;
        return UiValuesFake.Add(owner, List, item, (void*)fn, user, (UiValuesAbi.Registration*)r);
    }
    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int Unsubscribe(ulong h)
    {
        if (UiValuesFake.State is not { } s || KitBackend.Instance.Harness is not { } harness) return 3;
        lock (s.Lock)
        {
            KitFakes.State<UiNativeTintsState>(harness).Faults.Remove(h);
            return s.Regs.RemoveAll(r => r.Handle == h && r.List == List) != 0 ? 0 : 2;
        }
    }
}
public sealed partial class KilnHarness
{
    /// <summary>Offline native-background tint rules; no game drawing. Since 0.2.17.</summary>
    public HarnessUiNativeTints UiNativeTints => new(this);
}
/// <summary>Evaluates Ui.Inventory.TintWhen without drawing. Since 0.2.17.</summary>
public sealed class HarnessUiNativeTints(KilnHarness harness)
{
    /// <summary>Runs the rules over this copied slot (including plain stacks or empty slots). Since 0.2.17.</summary>
    public unsafe Color TintOf(FakeInventorySlot slot, Color background)
    {
        if (FakeTables.Off(Ui.Inventory.TintCapability)) return background;
        var s = KitFakes.State<UiValuesFakeState>(harness);
        var faults = KitFakes.State<UiNativeTintsState>(harness).Faults;
        UiValuesFakeState.Reg[] rules;
        lock (s.Lock) rules = s.Regs.Where(r => r.List == UiNativeTintsFake.List).ToArray();
        var v = new UiValuesNative.ItemView { Size = (uint)sizeof(UiValuesNative.ItemView), Item = slot.Item,
            Flags = (slot.InventoryPage != 0 ? 4u : 0u) | (slot.ItemEntity != 0 ? 1u : 0u), UnitsPerCharge = 1,
            ClientEntity = slot.ItemEntity, InventoryPage = slot.InventoryPage, SlotIndex = slot.SlotIndex, Count = slot.Count,
            Level = (uint)Math.Max(0, slot.Level), Rarity = slot.Rarity is { } r ? (uint)r : 0xFF };
        var rgba = background.Rgba;
        foreach (var rule in rules)
        {
            lock (s.Lock) if (!s.Regs.Contains(rule) || rule.Item != 0 && rule.Item != slot.Item) continue;
            var edit = rgba;
            var rc = ((delegate* unmanaged[Cdecl]<UiValuesNative.ItemView*, uint*, void*, int>)rule.Fn)(&v, &edit, (void*)rule.User);
            lock (s.Lock)
            {
                if (rc == 0) { rgba = edit; faults.Remove(rule.Handle); }
                else if (rc == 7 && s.Regs.Contains(rule))
                {
                    faults.TryGetValue(rule.Handle, out var n);
                    if (n + 1 >= 5) { s.Regs.Remove(rule); faults.Remove(rule.Handle); }
                    else faults[rule.Handle] = n + 1;
                }
            }
        }
        return Color.FromRgba(rgba);
    }
}
