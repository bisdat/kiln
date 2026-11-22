// SPDX-License-Identifier: MIT
using Kiln;
using Kiln.FakeUi;
using Xunit;

public sealed class UiItemDetailTests
{
    [Fact]
    public void Fallback_is_explicit_and_width_is_validated()
    {
        var previous = Backend.Current;
        FakeUiTable.Install(); UiNative.ResetForTests();
        UiValuesTestSuite.FakeUiValues.Caps = 0;
        Backend.Current = new UiValuesTestSuite.FakeUiValues.Backend([]);
        try
        {
            var draw = new UiDraw();
            var result = draw.ItemDetail(new ItemId(0x12345678), 140);
            Assert.False(result.Ok);
            Assert.Contains(FakeUiTable.Texts, s => s.Contains("not captured", StringComparison.OrdinalIgnoreCase));
            Assert.Throws<ArgumentOutOfRangeException>(() => draw.ItemDetail(new ItemId(1), float.NaN));
            Assert.Throws<ArgumentOutOfRangeException>(() => draw.ItemDetail(new ItemId(1), -1));
            Assert.Throws<ArgumentException>(() => draw.ItemDetail(ItemId.None));
        }
        finally { Backend.Current = previous; FakeUiTable.Free(); UiNative.ResetForTests(); }
    }

    [Fact]
    public void Text_element_wrapping_preserves_lines_and_respects_width()
    {
        var rows = UiTooltipWrap.Wrap(["first", new string('x', 100), "Ü / test"], 70, s => s.Length * 7);
        Assert.All(rows, row => Assert.True(row.Length <= 10));
        Assert.Equal(100, rows.Where(r => r.StartsWith('x')).Sum(r => r.Length));
    }
}
