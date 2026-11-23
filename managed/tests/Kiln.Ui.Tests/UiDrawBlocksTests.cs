// SPDX-License-Identifier: MIT
// UiDraw's calls over kiln.ui.blocks (UiDrawBlocks.cs, 0.2.0-dev.31; review S8): BeginControlledTab, SetNextItemOpen,
// LastItemState and TextField on a fake kiln.ui.blocks table (the flag mapping, the capacity, a value that does not fit or
// holds a NUL shown read-only and never written back), their fallbacks on a Kiln without the table, and the table's layout.
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Text;
using Kiln.FakeUi;
using Xunit;

namespace Kiln.UiDrawBlocksTests;

[Collection(UiSerial.Name)]
public sealed unsafe class UiDrawBlocksTests : IDisposable
{
    private readonly IKilnBackend _previous;
    private readonly UiDraw _ui = new();
    private UiNative.BlocksTable* _table;

    private static readonly List<(string Label, uint Flags)> Tabs = [];
    private static readonly List<(bool Open, uint Condition)> Opens = [];
    private static readonly List<(string Text, nuint Capacity, uint Flags, float W, float H)> Fields = [];
    private static uint _state;
    private static Func<string, string>? _type;

    public UiDrawBlocksTests()
    {
        _previous = Backend.Current;
        FakeUiTable.Install();
        Backend.Current = new FakeUiTable.Backend();
        UiNative.ResetForTests();
        Tabs.Clear();
        Opens.Clear();
        Fields.Clear();
        _state = 0;
        _type = null;
    }

    public void Dispose()
    {
        UiNative.ResetForTests();
        FakeUiTable.Type = null;
        if (_table != null) NativeMemory.Free(_table);
        Backend.Current = _previous;
        FakeUiTable.Free();
    }

    private void InstallBlocks()
    {
        _table = (UiNative.BlocksTable*)NativeMemory.AllocZeroed((nuint)sizeof(UiNative.BlocksTable));
        _table->Header = new ServiceHeader { Size = (uint)sizeof(UiNative.BlocksTable), Version = 1 };
        _table->BeginTabItemEx = &BeginTabItemEx;
        _table->SetNextItemOpen = &SetNextItemOpen;
        _table->ItemState = &ItemState;
        _table->InputTextEx = &InputTextEx;
        UiNative.SetBlocksForTests(_table);
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static byte BeginTabItemEx(NativeStr label, uint flags)
    {
        Tabs.Add((label.ToString(), flags));
        return label.ToString() == "Bag" ? (byte)1 : (byte)0;
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static void SetNextItemOpen(byte open, uint condition) => Opens.Add((open != 0, condition));

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static uint ItemState() => _state;

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static byte InputTextEx(NativeStr label, byte* buf, nuint cap, uint flags, float w, float h)
    {
        var len = 0;
        while ((nuint)len < cap && buf[len] != 0) len++;
        var text = Encoding.UTF8.GetString(buf, len);
        Fields.Add((text, cap, flags, w, h));
        if (_type?.Invoke(text) is not { } typed) return 0;
        var bytes = Encoding.UTF8.GetBytes(typed);
        var n = Math.Min(bytes.Length, (int)cap - 1);
        bytes.AsSpan(0, n).CopyTo(new Span<byte>(buf, n));
        buf[n] = 0;
        return 1;
    }

    [Fact]
    public void The_table_is_the_headers_layout()
    {
        Assert.Equal(40, sizeof(UiNative.BlocksTable));
        Assert.Equal(8, (int)Marshal.OffsetOf<UiNative.BlocksTable>(nameof(UiNative.BlocksTable.BeginTabItemEx)));
        Assert.Equal(16, (int)Marshal.OffsetOf<UiNative.BlocksTable>(nameof(UiNative.BlocksTable.SetNextItemOpen)));
        Assert.Equal(24, (int)Marshal.OffsetOf<UiNative.BlocksTable>(nameof(UiNative.BlocksTable.ItemState)));
        Assert.Equal(32, (int)Marshal.OffsetOf<UiNative.BlocksTable>(nameof(UiNative.BlocksTable.InputTextEx)));
    }

    [Fact]
    public void Tabs_trees_and_the_item_state_go_to_kiln_ui_blocks()
    {
        InstallBlocks();
        Assert.True(_ui.HasBlockWidgets);
        Assert.True(_ui.BeginControlledTab("Bag", select: true));
        Assert.False(_ui.BeginControlledTab("Map", select: false));
        Assert.Equal(new (string, uint)[] { ("Bag", 1u), ("Map", 0u) }, Tabs);
        _ui.SetNextItemOpen(true);
        _ui.SetNextItemOpen(false);
        Assert.Equal(new (bool, uint)[] { (true, 0u), (false, 0u) }, Opens);  // KUI_COND_ALWAYS only
        _state = 1 | 8;
        Assert.Equal(UiItemState.Active | UiItemState.Edited, _ui.LastItemState);
        _state = 2 | 4;
        Assert.Equal(UiItemState.Deactivated | UiItemState.DeactivatedAfterEdit, _ui.LastItemState);
    }

    [Fact]
    public void A_text_field_maps_its_flags_and_hands_the_whole_value_with_room_for_maxBytes()
    {
        InstallBlocks();
        var value = "Grüße";
        Assert.False(_ui.TextField("##n", ref value, 16));
        Assert.Equal(("Grüße", (nuint)17, 0u), (Fields[^1].Text, Fields[^1].Capacity, Fields[^1].Flags));
        _ui.TextField("##n", ref value, 16, UiTextFieldFlags.EnterReturns);
        Assert.Equal(1u, Fields[^1].Flags);
        _ui.TextField("##n", ref value, 16, UiTextFieldFlags.ReadOnly);
        Assert.Equal(2u, Fields[^1].Flags);
        _ui.TextField("##n", ref value, 16, UiTextFieldFlags.Multiline | UiTextFieldFlags.EnterReturns, 120, 60);
        Assert.Equal((5u, 120f, 60f), (Fields[^1].Flags, Fields[^1].W, Fields[^1].H));
        _type = t => t + "!";
        Assert.True(_ui.TextField("##n", ref value, 16));
        Assert.Equal("Grüße!", value);
        // read-only: an edit (if ImGui made one) never comes back
        Assert.False(_ui.TextField("##n", ref value, 16, UiTextFieldFlags.ReadOnly));
        Assert.Equal("Grüße!", value);
    }

    [Fact]
    public void A_value_that_does_not_fit_or_holds_a_NUL_is_read_only_and_never_written_back()
    {
        InstallBlocks();
        _type = t => t + "x";
        var long_ = new string('a', 40);
        Assert.False(_ui.TextField("##n", ref long_, 16));
        Assert.Equal(new string('a', 40), long_);
        Assert.Equal(2u, Fields[^1].Flags & 2u);
        Assert.Equal(16, Fields[^1].Text.Length);
        var nul = "before\0after";
        Assert.False(_ui.TextField("##n", ref nul, 64));
        Assert.Equal("before\0after", nul);
        Assert.Equal(2u, Fields[^1].Flags & 2u);
    }

    [Fact]
    public void Without_kiln_ui_blocks_the_calls_fall_back_and_say_so()
    {
        UiNative.SetBlocksForTests(null);
        Assert.False(_ui.HasBlockWidgets);
        _ui.SetNextItemOpen(true);  // nothing to call: no throw
        Assert.Empty(Opens);
        Assert.Equal(UiItemState.None, _ui.LastItemState);
        // a text field is kiln.ui's own InputText (the fake records the "##v" fields)
        var value = "abc";
        FakeUiTable.Type = t => t + "d";
        Assert.True(_ui.TextField("##v", ref value, 32));
        Assert.Equal("abcd", value);
        Assert.Equal(("abc", 33), (FakeUiTable.Fields[^1].Text, FakeUiTable.Fields[^1].Capacity));
        // read-only there: not drawn as an editable field, and nothing changes
        var fixed_ = "fixed";
        Assert.False(_ui.TextField("##v", ref fixed_, 32, UiTextFieldFlags.ReadOnly));
        Assert.Equal("fixed", fixed_);
        Assert.Empty(Fields);
    }
}
