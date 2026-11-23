// SPDX-License-Identifier: MIT
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;

namespace Kiln.TestKit;

internal sealed unsafe class UiSlotsFake : IKitFake
{
    public string Service => UiSlotsNative.Service;
    private static UiSlotsNative.Table* _table;
    public void Register()
    {
        if (_table == null)
        {
            _table = (UiSlotsNative.Table*)NativeMemory.AllocZeroed((nuint)sizeof(UiSlotsNative.Table));
            _table->Header = new ServiceHeader { Size = (uint)sizeof(UiSlotsNative.Table), Version = 1 };
            _table->Subscribe = &Subscribe;
        }
        KitBackend.Instance.RegisterTable(Service, (nint)_table);
    }
    public void Start(KilnHarness harness) { }
    public void BeforeStep(KilnHarness harness, ulong step) { }
    public void End(KilnHarness harness) { }
    public void AddCoverage(List<CoverageRow> rows) => rows.Add(new CoverageRow("Expanded slot subscriptions", CoverageKind.RecordingFake,
        [UiSlotsNative.Service],
        "Negotiated 120-byte slot frames from UiInventory.Draw, including rarity and level. Legacy kiln.ui.values " +
        "subscribers retain 112-byte records. Not modelled: game drawing, hooks or pixel mapping."));

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int Subscribe(ulong owner, uint recordSize,
        delegate* unmanaged[Cdecl]<UiValuesNative.SlotsFrame*, UiValuesNative.VisibleSlot*, uint, void*, void> fn,
        void* user, UiValuesNative.Registration* output)
    {
        try
        {
            if (recordSize != 120) return FakeTables.InvalidArgument;
            return UiValuesFake.Add(owner, UiValuesAbi.Slots, recordSize, (void*)fn, user, (UiValuesAbi.Registration*)output);
        }
        catch { return FakeTables.Internal; }
    }
}
