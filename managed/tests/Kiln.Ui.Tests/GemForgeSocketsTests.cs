// SPDX-License-Identifier: MIT
// GemForgeSockets.Widen (gems.sockets, the client's Gem Forge lists) against the fake kiln.ui.values table: the size
// guard, the owner and mask it passes, and every refusal as a (false, reason) result. The mask logic itself (owner
// union, 0 = vanilla, refused bits, the hooks) is the native host test's: native/core/tests/ui_values_sockets_test.inc.
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using Kiln;
using Kiln.FakeUi;
using Xunit;

namespace UiValuesTestSuite;

[Collection(UiSerial.Name)]
public sealed unsafe class GemForgeSocketsTests : IDisposable
{
    private readonly IKilnBackend _previous;
    private readonly FakeUiValues.Backend _backend;
    private static readonly List<(ulong Owner, uint Mask)> Calls = [];
    private static int Rc;

    public GemForgeSocketsTests()
    {
        _previous = Backend.Current;
        FakeUiValues.Install();
        _backend = new FakeUiValues.Backend(["ui.gemforge.inspect", "gems.sockets"]);
        Backend.Current = _backend;
        UiValuesNative.ResetForTests();
        UiValueBridge.ResetForTests();
        Calls.Clear();
        Rc = UiValuesNative.Ok;
        var t = (UiValuesNative.SocketsTable*)FakeUiValues.Table;
        t->Base.Base.Header.Size = (uint)sizeof(UiValuesNative.SocketsTable);
        t->GemForgeSockets = &Sockets;
    }

    public void Dispose()
    {
        UiValueBridge.ResetForTests();
        Backend.Current = _previous;
        UiValuesNative.ResetForTests();
        FakeUiValues.Free();
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int Sockets(ulong owner, uint mask)
    {
        lock (Calls) Calls.Add((owner, mask));
        return Rc;
    }

    [Fact]
    public void Table_layout_appends_one_slot_after_the_gem_forge_snapshot()
    {
        Assert.Equal(104, (int)Marshal.OffsetOf<UiValuesNative.SocketsTable>(nameof(UiValuesNative.SocketsTable.GemForgeSockets)));
        Assert.Equal(112, sizeof(UiValuesNative.SocketsTable));
        Assert.Equal(sizeof(UiValuesNative.GemForgeTable), (int)Marshal.OffsetOf<UiValuesNative.SocketsTable>(nameof(UiValuesNative.SocketsTable.GemForgeSockets)));
    }

    [Fact]
    public void Widen_passes_the_mask_and_the_callers_owner()
    {
        Assert.Equal((true, (string?)null), GemForgeSockets.Widen(0x2));
        Assert.Equal((true, (string?)null), GemForgeSockets.Widen(0));
        Assert.Equal([(99ul, 0x2u), (99ul, 0u)], Calls);  // no mod on the stack: Kiln's own kiln.ui.values owner
    }

    [Fact]
    public void Widen_refuses_bits_outside_the_gear_categories_without_calling_native()
    {
        foreach (var bad in new uint[] { 1u << 14, 1u << 15, 0x8002, 0x8000_0000 })
        {
            var (ok, reason) = GemForgeSockets.Widen(bad);
            Assert.False(ok);
            Assert.Contains("0..13", reason);
        }
        Assert.Empty(Calls);
    }

    [Fact]
    public void Native_refusals_become_reasons_not_exceptions()
    {
        Rc = UiValuesNative.Unavailable;
        var (ok, reason) = GemForgeSockets.Widen(0x2);
        Assert.False(ok);
        Assert.Contains("gems.sockets is off", reason);
        Rc = UiValuesNative.InvalidArgument;
        Assert.False(GemForgeSockets.Widen(0x2).Ok);
        Rc = UiValuesNative.Revoked;
        Assert.Equal((false, "the mod is unloading"), GemForgeSockets.Widen(0x2));
        Rc = UiValuesNative.Internal;
        Assert.Contains("(7)", GemForgeSockets.Widen(0x2).Reason);
        Assert.Equal(4, Calls.Count);
    }

    [Fact]
    public void An_older_table_or_none_is_a_reason_and_no_call()
    {
        FakeUiValues.Table->Header.Size = (uint)sizeof(UiValuesNative.GemForgeTable);
        var (ok, reason) = GemForgeSockets.Widen(0x2);
        Assert.False(ok);
        Assert.Contains("no gemforge_sockets", reason);
        Assert.NotEqual(0, (nint)UiValuesNative.T);  // the older table itself stays usable
        FakeUiValues.Table->Header.Size = (uint)sizeof(UiValuesNative.SocketsTable);
        _backend.TableHidden = true;
        UiValuesNative.ResetForTests();
        (ok, reason) = GemForgeSockets.Widen(0x2);
        Assert.False(ok);
        Assert.Contains("client only", reason);
        Assert.Empty(Calls);
    }

    [Fact]
    public void Kiln_Abilities_may_call_the_internal_entry()
    {
        var friends = typeof(GemForgeSockets).Assembly.GetCustomAttributes(typeof(InternalsVisibleToAttribute), false)
            .Cast<InternalsVisibleToAttribute>().Select(a => a.AssemblyName);
        Assert.Contains("Kiln.Abilities", friends);
    }
}
