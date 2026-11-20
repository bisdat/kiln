// SPDX-License-Identifier: MIT
// GemForgeSources (ui.gemforge.sources, 0.2.0-dev.29.15, the client's chest gems in the GEMS grid) against the fake
// kiln.ui.values table: the layout of the appended slots and records, the rows and payer rows it passes, and every
// refusal as a (false, reason) result. The hooks themselves are the native host test's:
// native/core/tests/ui_values_sources_test.inc.
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using Kiln;
using Kiln.FakeUi;
using Xunit;

namespace UiValuesTestSuite;

[Collection(UiSerial.Name)]
public sealed unsafe class GemForgeSourcesTests : IDisposable
{
    private readonly IKilnBackend _previous;
    private readonly FakeUiValues.Backend _backend;
    private static readonly List<(ulong Owner, UiValuesNative.GemSourceRow[] Rows, UiValuesNative.GemSourcePayer[] Payers)> Calls = [];
    private static int Rc;

    public GemForgeSourcesTests()
    {
        _previous = Backend.Current;
        FakeUiValues.Install();
        _backend = new FakeUiValues.Backend(["ui.gemforge.inspect", "gems.sockets", "ui.gemforge.sources"]);
        Backend.Current = _backend;
        UiValuesNative.ResetForTests();
        UiValueBridge.ResetForTests();
        Calls.Clear();
        Rc = UiValuesNative.Ok;
        var t = (UiValuesNative.SourcesTable*)FakeUiValues.Table;
        t->Base.Base.Base.Header.Size = (uint)sizeof(UiValuesNative.SourcesTable);
        t->GemForgeSources = &Sources;
        t->GemForgeSourcesState = &State;
    }

    public void Dispose()
    {
        UiValueBridge.ResetForTests();
        Backend.Current = _previous;
        UiValuesNative.ResetForTests();
        FakeUiValues.Free();
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int Sources(ulong owner, UiValuesNative.GemSourceRow* rows, uint count, UiValuesNative.GemSourcePayer* payers, uint payerCount)
    {
        lock (Calls) Calls.Add((owner, new Span<UiValuesNative.GemSourceRow>(rows, (int)count).ToArray(),
            new Span<UiValuesNative.GemSourcePayer>(payers, (int)payerCount).ToArray()));
        return Rc;
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int State(UiValuesNative.GemSourcesState* s)
    {
        if (s->Size < (uint)sizeof(UiValuesNative.GemSourcesState)) return UiValuesNative.InvalidArgument;
        s->Installed = 1;
        s->WantedCount = 2;
        s->Wanted[0] = 0x2b0cf1d5;
        s->Wanted[1] = 7;
        return UiValuesNative.Ok;
    }

    [Fact]
    public void Layout_matches_the_native_header()
    {
        Assert.Equal(112, (int)Marshal.OffsetOf<UiValuesNative.SourcesTable>(nameof(UiValuesNative.SourcesTable.GemForgeSources)));
        Assert.Equal(120, (int)Marshal.OffsetOf<UiValuesNative.SourcesTable>(nameof(UiValuesNative.SourcesTable.GemForgeSourcesState)));
        Assert.Equal(128, sizeof(UiValuesNative.SourcesTable));
        Assert.Equal(48, sizeof(UiValuesNative.GemSourceRow));
        Assert.Equal(32, (int)Marshal.OffsetOf<UiValuesNative.GemSourceRow>(nameof(UiValuesNative.GemSourceRow.Token)));
        Assert.Equal(16, sizeof(UiValuesNative.GemSourcePayer));
        Assert.Equal(200, sizeof(UiValuesNative.GemSourcesState));
    }

    [Fact]
    public void Set_passes_the_rows_and_payers_under_Kilns_own_owner()
    {
        var rows = new[] { new GemForgeSourceRow(0x9001, 4, 113, 1202, 7, 2, 1, 0xabc, 12345) };
        var payers = new[] { new GemForgeSourcePayer(1, true, 0x2b0cf1d5, 40) };
        Assert.Equal((true, (string?)null), GemForgeSources.Set(rows, payers));
        var (owner, r, p) = Assert.Single(Calls);
        Assert.Equal(99ul, owner);  // no mod: Kiln's own kiln.ui.values owner
        var row = Assert.Single(r);
        Assert.Equal((48u, 0x9001u, (byte)4, 113u, 1202u, 7u, (byte)2, (byte)1, 0xabcul, 12345ul),
            (row.Size, row.Page, row.Slot, row.Item, row.Pide, row.Level, row.Rarity, row.Group, row.Token, row.ExpiresMs));
        var payer = Assert.Single(p);
        Assert.Equal((16u, (byte)1, (byte)1, 0x2b0cf1d5u, 40u), (payer.Size, payer.Group, payer.PlayerFirst, payer.Item, payer.Count));
        var st = GemForgeSources.State();
        Assert.NotNull(st);
        Assert.True(st!.Value.Installed);
        Assert.Equal(new uint[] { 0x2b0cf1d5, 7 }, st.Value.Wanted);
    }

    [Fact]
    public void Refusals_are_reasons_and_an_older_table_is_no_call()
    {
        Rc = UiValuesNative.Unavailable;
        var (ok, reason) = GemForgeSources.Set([new GemForgeSourceRow(0x9001, 4, 113, 1202, 7, 2, 0, 1, 1)], []);
        Assert.False(ok);
        Assert.Contains("ui.gemforge.sources is off", reason);
        Rc = UiValuesNative.InvalidArgument;
        Assert.False(GemForgeSources.Set([new GemForgeSourceRow(0x9001, 4, 113, 1202, 7, 2, 0, 1, 1)], []).Ok);
        Assert.False(GemForgeSources.Set(new GemForgeSourceRow[257], []).Ok);  // over 256: refused here
        Assert.Equal(2, Calls.Count);
        FakeUiValues.Table->Header.Size = (uint)sizeof(UiValuesNative.SocketsTable);
        Assert.Contains("no gemforge_sources", GemForgeSources.Set([], []).Reason);
        Assert.Null(GemForgeSources.State());
        Assert.Equal(2, Calls.Count);
    }
}
