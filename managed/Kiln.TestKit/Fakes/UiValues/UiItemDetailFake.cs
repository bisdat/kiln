// SPDX-License-Identifier: MIT
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Text;

namespace Kiln.TestKit;

internal sealed class UiItemDetailFakeState
{
    internal readonly object Lock = new();
    internal readonly List<UiItemDetailSnapshot> Snapshots = [];
    internal ulong Frame;
}

internal sealed unsafe class UiItemDetailFake : IKitFake
{
    public string Service => UiItemDetailNative.Service;
    private static UiItemDetailNative.Table* _table;
    public void Register()
    {
        if (_table == null)
        {
            _table = (UiItemDetailNative.Table*)NativeMemory.AllocZeroed((nuint)sizeof(UiItemDetailNative.Table));
            _table->Header = new ServiceHeader { Size = (uint)sizeof(UiItemDetailNative.Table), Version = 1 };
            _table->Copy = &Copy;
        }
        KitBackend.Instance.RegisterTable(Service, (nint)_table);
        FakeTables.Features([Ui.Values.ItemDetailCapability]);
    }
    public void Start(KilnHarness harness) { }
    public void BeforeStep(KilnHarness harness, ulong step) { }
    public void End(KilnHarness harness) { }
    public void AddCoverage(List<CoverageRow> rows) => rows.Add(new CoverageRow("Captured item detail", CoverageKind.RecordingFake,
        [UiItemDetailNative.Service, Ui.Values.ItemDetailCapability],
        "Owned game-panel text/value recordings, exact instance/variant lookup and 64-entry eviction. UiItemDetail.Capture/Clear " +
        "supply captures and model a world change. Not modelled: game text construction, hooks, fonts or drawing."));

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int Copy(ulong owner, UiItemDetailNative.Query* q, UiItemDetailNative.Snapshot* output,
        UiItemDetailNative.Line* lines, uint capacity)
    {
        try
        {
            if (owner == 0 || q == null || q->Size < sizeof(UiItemDetailNative.Query) || (q->Entity == 0 && q->Item == 0) ||
                output == null || output->Size < sizeof(UiItemDetailNative.Snapshot) || (capacity != 0 && lines == null)) return FakeTables.InvalidArgument;
            if (FakeTables.Off(Ui.Values.ItemDetailCapability)) return FakeTables.Unavailable;
            *output = new() { Size = (uint)sizeof(UiItemDetailNative.Snapshot) };
            if (FakeTables.Active(UiItemDetailNative.Service) is not { } h) return FakeTables.Unavailable;
            var st = KitFakes.State<UiItemDetailFakeState>(h);
            lock (st.Lock)
            {
                foreach (var s in st.Snapshots)
                {
                    if ((q->Entity != 0 && q->Entity != s.Entity.Value) || (q->Item != 0 && q->Item != s.Item.Value) ||
                        (q->Level != uint.MaxValue && q->Level != (uint)s.Level) ||
                        (q->Rarity != uint.MaxValue && q->Rarity != (uint?)s.Rarity)) continue;
                    *output = new() { Size = (uint)sizeof(UiItemDetailNative.Snapshot), Ok = 1, Count = (uint)s.Lines.Count,
                        Item = s.Item.Value, Entity = s.Entity.Value, Level = (uint)s.Level, Rarity = s.Rarity is { } r ? (uint)r : uint.MaxValue,
                        Session = s.Session, Frame = s.Frame, CaptureMs = s.CaptureMs };
                    if (capacity < output->Count) { output->Ok = 0; return 5; }
                    for (var i = 0; i < s.Lines.Count; i++)
                    {
                        var row = s.Lines[i];
                        lines[i] = new() { Size = (uint)sizeof(UiItemDetailNative.Line), Flags = row.Value.HasValue ? 1u : 0,
                            Value = row.Value ?? 0, TextBytes = (uint)Encoding.UTF8.GetByteCount(row.Text) };
                        Encoding.UTF8.GetBytes(row.Text, new Span<byte>(lines[i].Text, (int)lines[i].TextBytes));
                    }
                    return 0;
                }
            }
            return 0;
        }
        catch { return FakeTables.Internal; }
    }
}

public sealed partial class KilnHarness
{
    /// <summary>Recordings of the game's item-detail text and values (no game renderer is simulated). Since 0.2.0-dev.30.5.</summary>
    public HarnessUiItemDetail UiItemDetail => new(this);
}

/// <summary>Explicit game-panel captures for Ui.Values.ItemDetail. Text and numeric metadata are
/// supplied by the test, never computed by a pretend game. Clear models leaving the client world. Since 0.2.0-dev.30.5.</summary>
public sealed class HarnessUiItemDetail
{
    private static readonly Encoding StrictUtf8 = new UTF8Encoding(false, true);
    private readonly KilnHarness _harness;
    internal HarnessUiItemDetail(KilnHarness harness) => _harness = harness;

    /// <summary>Records a completed panel (owned copy). Same bounds as native: 64 rows, 4096 UTF-8 bytes per row,
    /// 32768 total bytes, 64 cached instances/variants. An entity replaces its previous variant. Invalid recordings
    /// throw an argument exception; this fake does not simulate native text-builder failures. Since 0.2.0-dev.30.5.</summary>
    public void Capture(ItemId item, EntityId entity, int level, ItemRarity? rarity, params UiItemDetailLine[] lines)
    {
        ArgumentNullException.ThrowIfNull(lines);
        UiItemDetailBridge.Validate(entity, item, level, rarity);
        if (item.IsNone) throw new ArgumentException("item is required", nameof(item));
        if (lines.Length is 0 or > 64 || lines.Any(l => l is null || l.Text is null || l.Text.Contains('\0') ||
            StrictUtf8.GetByteCount(l.Text) > 4096 || l.Value is { } n && !float.IsFinite(n)) ||
            lines.Sum(l => StrictUtf8.GetByteCount(l.Text)) > 32768) throw new ArgumentException("invalid item-detail recording", nameof(lines));
        var st = KitFakes.State<UiItemDetailFakeState>(_harness);
        lock (st.Lock)
        {
            st.Snapshots.RemoveAll(s => !entity.IsNone ? s.Entity == entity : s.Entity.IsNone && s.Item == item && s.Level == level && s.Rarity == rarity);
            st.Snapshots.Insert(0, new(item, entity, level, rarity, 1, ++st.Frame, (ulong)Environment.TickCount64, Array.AsReadOnly(lines.ToArray())));
            if (st.Snapshots.Count > 64) st.Snapshots.RemoveAt(64);
        }
    }
    /// <summary>Forgets every recorded panel, as on a client world change. Since 0.2.0-dev.30.5.</summary>
    public void Clear()
    {
        var st = KitFakes.State<UiItemDetailFakeState>(_harness);
        lock (st.Lock) st.Snapshots.Clear();
    }
}
