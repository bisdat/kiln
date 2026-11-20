// SPDX-License-Identifier: MIT
// Ui.Tooltip against the fake kiln.ui.values table: lines registered for the game's own item detail view (their colour,
// size and place reach the native writer as KilnUiLineFlags), the fallback to the details window on a native service
// without the tooltip slots, value providers (factors multiply, a refusal keeps, a bad factor is a fault) and the error
// rule. The drawing itself is native/core/tests/ui_values_tooltip_test.inc's.
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using Kiln;
using Kiln.FakeUi;
using Xunit;

namespace UiValuesTestSuite;

[Collection(UiSerial.Name)]
public sealed unsafe class UiTooltipTests : IDisposable
{
    private readonly IKilnBackend _previous;
    private static readonly string[] Rows =
        ["ui.values.detail", "ui.values.tooltip_lines", "ui.tooltip.lines", "ui.tooltip.values"];

    public UiTooltipTests()
    {
        _previous = Backend.Current;
        FakeUiValues.Install();
        FakeUiValues.Table->Header.Size = (uint)sizeof(UiValuesNative.TooltipTable);
        Backend.Current = new FakeUiValues.Backend(new HashSet<string>(Rows));
        UiValuesNative.ResetForTests();
        UiValueBridge.ResetForTests();
        UiValueEvents.ResetForTests();
        UiTooltipBridge.ResetForTests();
        UiTooltipNative.ResetForTests();
        Events.Reset();
    }

    public void Dispose()
    {
        UiTooltipNative.ResetForTests();
        UiTooltipBridge.ResetForTests();
        Events.Reset();
        UiValueBridge.ResetForTests();
        UiValueEvents.ResetForTests();
        Backend.Current = _previous;
        UiValuesNative.ResetForTests();
        FakeUiValues.Free();
    }

    private static readonly List<(ulong Text, uint Flags)> Appended = [];

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int AppendLine(UiValuesNative.LineWriter* w, ulong id, uint flags)
    {
        if (w->Count >= w->Capacity) return UiValuesNative.BufferTooSmall;
        Appended.Add((id, flags));
        w->Count++;
        return 0;
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int Append(UiValuesNative.LineWriter* w, ulong id)
    {
        if (w->Count >= w->Capacity) return UiValuesNative.BufferTooSmall;
        Appended.Add((id, 0xFFFF_FFFF));  // the plain append: no flags at all
        w->Count++;
        return 0;
    }

    private static (int Rc, List<(string Text, uint Flags)> Lines) CallLines(bool styled = true, uint capacity = 16)
    {
        var fn = (delegate* unmanaged[Cdecl]<UiValuesNative.ProviderContext*, UiValuesNative.ItemView*, UiValuesNative.LineWriter*, void*, int>)FakeUiValues.LinesFn;
        UiValuesNative.ProviderContext c = default;
        c.Size = (uint)sizeof(UiValuesNative.ProviderContext);
        c.Text = FakeUiValues.Ops;
        UiValuesNative.ItemView v = default;
        v.Size = (uint)sizeof(UiValuesNative.ItemView);
        v.Item = 0xAA;
        UiValuesNative.LineWriter w = default;
        // an older native writer ends before AppendLine
        w.Size = styled ? (uint)sizeof(UiValuesNative.LineWriter) : (uint)(sizeof(UiValuesNative.LineWriter) - 8);
        w.Capacity = capacity;
        w.Append = &Append;
        if (styled) w.AppendLine = &AppendLine;
        Appended.Clear();
        var rc = fn(&c, &v, &w, (void*)FakeUiValues.LinesUser);
        return (rc, Appended.Select(a => (FakeUiValues.Texts[a.Text], a.Flags)).ToList());
    }

    private static (int Rc, float Scale) CallScale(float scale)
    {
        var fn = (delegate* unmanaged[Cdecl]<UiValuesNative.ProviderContext*, UiValuesNative.ItemView*, float*, void*, int>)FakeUiValues.ValuesFn;
        UiValuesNative.ProviderContext c = default;
        c.Size = (uint)sizeof(UiValuesNative.ProviderContext);
        c.Text = FakeUiValues.Ops;
        UiValuesNative.ItemView v = default;
        v.Size = (uint)sizeof(UiValuesNative.ItemView);
        v.Item = 0xAA;
        var rc = fn(&c, &v, &scale, (void*)FakeUiValues.ValuesUser);
        return (rc, scale);
    }

    [Fact]
    public void The_native_records_have_the_C_layout()
    {
        Assert.Equal(296, sizeof(UiValuesNative.LineWriter));
        Assert.Equal(288, (int)Marshal.OffsetOf<UiValuesNative.LineWriter>(nameof(UiValuesNative.LineWriter.AppendLine)));
        Assert.Equal(152, sizeof(UiValuesNative.TooltipNativeState));
        Assert.Equal(152, sizeof(UiValuesNative.TooltipTable));
    }

    [Fact]
    public void AddLines_registers_for_the_game_tooltip_and_passes_colour_size_and_place()
    {
        using var end = Ui.Tooltip.AddLines(new ItemId(0xAA), (in UiValueContext c, in UiItemView v) =>
            UiTooltipLines.With(new TooltipLine("+12 % fire", TooltipColor.Positive), new TooltipLine("Set: 2 of 4", TooltipColor.Epic, Small: true)));
        Assert.True(end.Ok);
        Assert.Equal(UiValuesNative.LinesInGameTooltip, FakeUiValues.NativeLinesFlags);
        var (rc, lines) = CallLines();
        Assert.Equal(0, rc);
        Assert.Equal([("+12 % fire", 1u), ("Set: 2 of 4", 6u | 0x10u)], lines);
        using var after = Ui.Tooltip.AddLines((in UiValueContext c, in UiItemView v) => UiTooltipLines.With(new TooltipLine("Affix: sharp")), TooltipPlace.AfterStats);
        Assert.Equal([("Affix: sharp", 0x100u)], CallLines().Lines);
        Assert.Equal([("Affix: sharp", 0xFFFF_FFFFu)], CallLines(styled: false).Lines);  // an older writer: text only
    }

    [Fact]
    public void A_refusal_or_too_many_lines_adds_none_and_a_bad_colour_is_a_fault()
    {
        var answer = UiTooltipLines.Refuse("no data yet");
        using var reg = Ui.Tooltip.AddLines((in UiValueContext c, in UiItemView v) => answer);
        Assert.Equal(UiValuesNative.Unavailable, CallLines().Rc);
        answer = UiTooltipLines.With(Enumerable.Range(1, 5).Select(i => new TooltipLine($"line {i}")).ToArray());
        Assert.Equal(UiValuesNative.Unavailable, CallLines(capacity: 4).Rc);
        answer = UiTooltipLines.With(new TooltipLine("x", (TooltipColor)12));
        Assert.Equal(UiValuesNative.Internal, CallLines().Rc);
        answer = UiTooltipLines.With(default(TooltipLine));
        Assert.Equal(UiValuesNative.Internal, CallLines().Rc);
        Assert.Throws<ArgumentNullException>(() => Ui.Tooltip.AddLines(null!));
        Assert.Throws<ArgumentOutOfRangeException>(() => Ui.Tooltip.AddLines((in UiValueContext c, in UiItemView v) => UiTooltipLines.None, (TooltipPlace)7));
    }

    [Fact]
    public void AddLines_on_a_native_service_without_tooltip_slots_feeds_the_details_window()
    {
        FakeUiValues.Table->Header.Size = (uint)sizeof(UiValuesNative.Table);
        UiValuesNative.ResetForTests();
        using var reg = Ui.Tooltip.AddLines((in UiValueContext c, in UiItemView v) =>
            UiTooltipLines.With(new TooltipLine("in the window", TooltipColor.Rare)));
        Assert.True(reg.Ok);
        Assert.Equal(0u, FakeUiValues.NativeLinesFlags);  // registered through tooltip_lines
        Assert.Equal([("in the window", 0xFFFF_FFFFu)], CallLines(styled: false).Lines);
        Assert.False(Ui.Tooltip.InGame);
        Assert.Throws<KilnUnavailableException>(() => Ui.Tooltip.AddValues((in UiValueContext c, in UiItemView v) => UiTooltipScale.None));
    }

    [Fact]
    public void AddValues_factors_multiply_a_refusal_keeps_and_a_bad_factor_is_a_fault()
    {
        var answer = UiTooltipScale.By(1.5f);
        using var reg = Ui.Tooltip.AddValues(new ItemId(0xAA), (in UiValueContext c, in UiItemView v) => answer);
        Assert.Equal((0, 3.0f), CallScale(2.0f));
        answer = UiTooltipScale.Refuse("no instance data");
        Assert.Equal((UiValuesNative.Unavailable, 2.0f), CallScale(2.0f));
        answer = UiTooltipScale.By(float.NaN);
        Assert.Equal(UiValuesNative.Internal, CallScale(2.0f).Rc);
        answer = UiTooltipScale.By(600);
        Assert.Equal(UiValuesNative.Internal, CallScale(2.0f).Rc);  // 1200 in all: over 1000
    }

    [Fact]
    public void Values_without_their_row_throw_and_InGame_follows_the_native_state()
    {
        Backend.Current = new FakeUiValues.Backend(new HashSet<string>(["ui.values.detail", "ui.values.tooltip_lines"]));
        Assert.Equal(Ui.Tooltip.ValuesCapability, Assert.Throws<KilnUnavailableException>(() =>
            Ui.Tooltip.AddValues((in UiValueContext c, in UiItemView v) => UiTooltipScale.None)).Capability);
        Assert.False(Ui.Tooltip.InGame);
        FakeUiValues.NativeLinesOn = 1;
        Assert.True(Ui.Tooltip.InGame);
    }
}
