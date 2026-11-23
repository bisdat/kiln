// SPDX-License-Identifier: MIT
// GitHub issue #138 (3), 0.2.0-dev.31: the item icon cache's limits as a client setting (kiln.json "ui": {"item_icons":
// {"max_icons": n, "max_mib": m}}), the reserve it leaves under kiln.ui's 960 overlay images (the placeholder and recently
// freed icons included), large grids, and trimmed icons read at the size drawn once their art is known. The measurement
// behind the defaults (KILN_ICON_MEASURE=<file>) writes dev31/out/issue-138/ICON-CACHE-MEASURE.md.
using System.Runtime.CompilerServices;
using System.Text;
using System.Text.RegularExpressions;
using Xunit;

namespace Kiln;

[Collection(FakeUi.UiSerial.Name)]
public sealed class ItemIconLimitsTests : IDisposable
{
    private long _now = 1_000_000;
    private ulong _next = 100;
    private readonly List<(uint Item, int Bucket)> _decoded = [];
    private readonly HashSet<ulong> _live = [];
    private readonly List<(ulong Handle, long At)> _freedAt = [];
    private int _peakFootprint, _dyingFrom;
    private Func<uint, int, byte[]> _pixels = (_, b) => new byte[b * b * 4];

    public ItemIconLimitsTests() => Fakes();

    private void Fakes()
    {
        ItemIcons.ResetForTests();
        ItemIcons.Synchronous = true;
        ItemIcons.Clock = () => _now;
        ItemIcons.Session = () => 1;
        ItemIcons.Defined = _ => null;
        ItemIcons.GameItem = id => new Guid((int)id.Value, 0, 0, new byte[8]);
        ItemIcons.Decode = (g, bucket) =>
        {
            var item = (uint)BitConverter.ToInt32(g.ToByteArray(), 0);
            _decoded.Add((item, bucket));
            return (_pixels(item, bucket), bucket, bucket);
        };
        ItemIcons.Upload = (_, _, _) =>
        {
            _live.Add(++_next);
            Footprint();
            return _next;
        };
        ItemIcons.Free = h =>
        {
            if (_live.Remove(h)) _freedAt.Add((h, _now));
        };
        ItemIcons.ImageState = _ => UiNative.ImageReady;
    }

    public void Dispose() => ItemIcons.ResetForTests();

    /// <summary>The overlay images the cache holds as kiln.ui counts them: live ones and those freed in the last frame or so
    /// (kiln.ui destroys a freed texture at its next frame; counted here for a full quarter second).</summary>
    private void Footprint()
    {
        while (_dyingFrom < _freedAt.Count && _freedAt[_dyingFrom].At <= _now - ItemIconLimits.DyingMs) _dyingFrom++;
        _peakFootprint = Math.Max(_peakFootprint, _live.Count + _freedAt.Count - _dyingFrom);
    }

    private static string RepoRoot([CallerFilePath] string source = "")
    {
        for (var d = new DirectoryInfo(Path.GetDirectoryName(source) ?? "."); d != null; d = d.Parent)
            if (File.Exists(Path.Combine(d.FullName, "native", "services", "ui", "image_budget.h"))) return d.FullName;
        throw new InvalidOperationException("repository root not found above the test's source");
    }

    [Fact]
    public void The_limits_leave_a_reserve_under_kiln_uis_overlay_images()
    {
        var text = File.ReadAllText(Path.Combine(RepoRoot(), "native", "services", "ui", "image_budget.h"));
        int Num(string name) => int.Parse(Regex.Match(text, $@"constexpr uint32_t {name} = (\d+);").Groups[1].Value);
        Assert.Equal(Num("kDescriptorPoolSize") - Num("kReserved"), ItemIconLimits.OverlayImages);
        // the most icons any setting allows, the placeholder and the icons freed but not destroyed yet: mods keep the rest
        Assert.True(ItemIconLimits.MaxIconsCap + 1 + ItemIconLimits.ChurnReserve + ItemIconLimits.ModReserve <= ItemIconLimits.OverlayImages);
        Assert.True(ItemIconLimits.Default.MaxIcons <= ItemIconLimits.MaxIconsCap);
        Assert.Equal(ItemIconLimits.Default, ItemIconLimits.Default.Clamped());
    }

    [Fact]
    public void Kiln_json_sets_the_limits_within_bounds()
    {
        var warnings = new List<string>();
        Assert.Equal(ItemIconLimits.Default, ItemIconLimits.Parse("{}"));
        Assert.Equal(ItemIconLimits.Default, ItemIconLimits.Parse("""{"ui": {"look": "kiln"}}"""));
        Assert.Equal(new ItemIconLimits(300, 32), ItemIconLimits.Parse("""{"ui": {"item_icons": {"max_icons": 300, "max_mib": 32}}}"""));
        Assert.Equal(new ItemIconLimits(128, 48), ItemIconLimits.Parse("""{"ui": {"item_icons": {"max_icons": 128}}} // a comment"""));
        Assert.Equal(new ItemIconLimits(ItemIconLimits.MaxIconsCap, ItemIconLimits.MinMib),
            ItemIconLimits.Parse("""{"ui": {"item_icons": {"max_icons": 5000, "max_mib": 1}}}""", warnings.Add));
        Assert.Contains(warnings, w => w.Contains("outside"));
        warnings.Clear();
        Assert.Equal(ItemIconLimits.Default, ItemIconLimits.Parse("""{"ui": {"item_icons": {"max_icons": "lots", "max_mib": 2.5}}}""", warnings.Add));
        Assert.Equal(2, warnings.Count);
        Assert.Equal(ItemIconLimits.Default, ItemIconLimits.Parse("not json"));
        Assert.Equal(ItemIconLimits.Default, ItemIconLimits.Read(Path.Combine(Path.GetTempPath(), "kiln-no-such-dir-" + Guid.NewGuid(), "kiln.json")));
        Assert.Equal(new ItemIconLimits(ItemIconLimits.MinIcons, ItemIconLimits.MaxMibCap), new ItemIconLimits(-4, 9999).Clamped());
    }

    /// <summary>One frame of a grid: every item of [first, first + count) drawn at <paramref name="pixels"/>; how many still show
    /// the placeholder.</summary>
    private int DrawGrid(uint first, int count, float pixels, bool trim = false)
    {
        var placeholder = ItemIcons.Placeholder();
        var waiting = 0;
        for (uint i = 0; i < count; i++)
        {
            var item = new ItemId(first + i);
            if (ItemIcons.Handle(item, trim ? ItemIcons.TrimmedPixels(item, pixels) : pixels, out _, out _) == placeholder) waiting++;
        }
        return waiting;
    }

    [Fact]
    public void A_large_grid_is_held_whole_and_lower_limits_from_kiln_json_apply_while_running()
    {
        // 450 icons on screen at 64 px (16 KiB each): the old 256 left 194 of them on the placeholder for good
        for (var f = 0; f < 3; f++, _now += 16) DrawGrid(1, 450, 64);
        Assert.Equal(0, DrawGrid(1, 450, 64));
        Assert.Equal(450, ItemIcons.Stats.Textures);

        ItemIcons.Configure(new ItemIconLimits(100, 8));
        Assert.Equal((100, 8L << 20), (ItemIcons.MaxEntries, ItemIcons.MaxBytes));
        // the 450 drawn now stay while drawn; once only 50 are drawn the rest go (the first frame drops 350 at once: the
        // churn reserve holds new reads back for a quarter second, then the 50 load)
        _now += 2_000;
        for (var f = 0; f < 70; f++, _now += 16) DrawGrid(1, 50, 64);
        Assert.True(ItemIcons.Stats.Textures <= 100, $"{ItemIcons.Stats.Textures} textures");
        Assert.True(ItemIcons.Stats.Bytes <= 8L << 20);
        Assert.Equal(0, DrawGrid(1, 50, 64));
    }

    [Fact]
    public void Scrolling_fast_never_holds_more_overlay_images_than_the_limit_and_its_reserve()
    {
        ItemIcons.Configure(new ItemIconLimits(ItemIconLimits.MaxIconsCap, ItemIconLimits.MaxMibCap));
        // 600 visible at once over 60000 items, a new 600 every frame (each drawn once a pass): once icons are a second
        // old, every new one needs one to go: maximal churn
        for (var f = 0; f < 120; f++, _now += 16)
        {
            DrawGrid((uint)(1 + f * 600 % 60000), 600, 64);
            Footprint();
        }
        Assert.True(_freedAt.Count > 300, $"{_freedAt.Count} freed");
        Assert.True(_peakFootprint <= ItemIconLimits.MaxIconsCap + 1 + ItemIconLimits.ChurnReserve,
            $"{_peakFootprint} overlay images at once");
        for (int i = 0, from = 0; i < _freedAt.Count; i++)
        {
            while (_freedAt[from].At <= _freedAt[i].At - ItemIconLimits.DyingMs) from++;
            Assert.True(i - from + 1 <= ItemIconLimits.ChurnReserve, $"{i - from + 1} freed within {ItemIconLimits.DyingMs} ms");
        }
    }

    /// <summary>A bucket x bucket picture whose art is a centred square of <paramref name="share"/> of its side.</summary>
    private static byte[] Art(int bucket, float share)
    {
        var px = new byte[bucket * bucket * 4];
        var side = (int)Math.Round(bucket * share);
        var o = (bucket - side) / 2;
        for (var y = o; y < o + side; y++)
            for (var x = o; x < o + side; x++)
                px[(y * bucket + x) * 4 + 3] = 255;
        return px;
    }

    [Theory]
    [InlineData(0.25f, 40, new[] { 128, 256 })]  // small art: 2x would stretch it; read at 160 -> the 256 level
    [InlineData(0.75f, 40, new[] { 128, 64 })]   // large art: 2x is oversized; 53 -> the 64 level
    [InlineData(0.5f, 40, new[] { 128 })]        // as guessed: no second read
    [InlineData(0f, 40, new[] { 128 })]          // nothing opaque (not known): stays at twice the size
    public void A_trimmed_icon_is_read_at_the_size_drawn_once_its_art_is_known(float share, float pixels, int[] buckets)
    {
        _pixels = (_, b) => share > 0 ? Art(b, share) : new byte[b * b * 4];
        for (var f = 0; f < 4; f++, _now += 16) DrawGrid(7, 1, pixels, trim: true);
        Assert.Equal(buckets, _decoded.Select(d => d.Bucket).ToArray());
        Assert.Equal(share > 0 ? pixels / share : pixels * 2, ItemIcons.TrimmedPixels(new ItemId(7), pixels), 0.01f);
        Assert.Equal(pixels * 2, ItemIcons.TrimmedPixels(new ItemId(8), pixels));  // another item: not known yet
        Assert.Equal(0, DrawGrid(7, 1, pixels, trim: true));
    }

    [Fact]
    public void Art_share_matches_kiln_uis_opaque_bounds()
    {
        Assert.Equal(0f, ItemIcons.ArtShareOf(new byte[16 * 16 * 4], 16, 16));
        Assert.Equal(0.5f, ItemIcons.ArtShareOf(Art(64, 0.5f), 64, 64));
        var faint = Art(64, 1f);
        for (var i = 3; i < faint.Length; i += 4) faint[i] = 15;  // under alpha 16: margin
        Assert.Equal(0f, ItemIcons.ArtShareOf(faint, 64, 64));
        var tall = new byte[10 * 20 * 4];
        tall[(19 * 10 + 9) * 4 + 3] = 255;
        tall[3] = 255;
        Assert.Equal(1f, ItemIcons.ArtShareOf(tall, 10, 20));
        Assert.Equal(0.125f, ItemIcons.ArtShareOf(Art(64, 1f / 64), 64, 64));  // a dot: clamped to 1/8
        Assert.Equal(0f, ItemIcons.ArtShareOf(new byte[8], 16, 16));          // short buffer
    }

    // ---- the measurement behind the defaults (KILN_ICON_MEASURE=<markdown file>; otherwise does nothing) ----

    private sealed record Scenario(string Name, int Visible, float Pixels, bool Trim, float Share);

    [Fact]
    public void Measure_defaults()
    {
        var path = Environment.GetEnvironmentVariable("KILN_ICON_MEASURE");
        if (string.IsNullOrEmpty(path)) return;
        var sb = new StringBuilder();
        sb.AppendLine("| limits | grid | icons on screen | level read | steady placeholders | reads | peak textures | peak MiB | peak overlay images |");
        sb.AppendLine("|---|---|---|---|---|---|---|---|---|");
        Scenario[] scenarios =
        [
            new("64 px plain, DPI 1", 120, 64, false, 0),
            new("64 px plain, DPI 1", 300, 64, false, 0),
            new("64 px plain, DPI 1", 450, 64, false, 0),
            new("40 px trimmed, DPI 1 (art 0.6)", 120, 40, true, 0.6f),
            new("40 px trimmed, DPI 1 (art 0.6)", 300, 40, true, 0.6f),
            new("40 px trimmed, DPI 1 (art 0.6)", 450, 40, true, 0.6f),
            new("40 px trimmed, DPI 2 (art 0.6)", 120, 80, true, 0.6f),
            new("40 px trimmed, DPI 2 (art 0.6)", 300, 80, true, 0.6f),
            new("64 px plain, DPI 2", 300, 128, false, 0),
        ];
        (string, ItemIconLimits)[] limits = [("256 / 24 MiB (0.2.0-dev.30)", new(256, 24)), ("512 / 48 MiB", new(512, 48)), ("512 / 64 MiB", new(512, 64))];
        foreach (var (name, l) in limits)
            foreach (var s in scenarios)
            {
                Fakes();
                _live.Clear();
                _freedAt.Clear();
                _decoded.Clear();
                _peakFootprint = 0;
                _dyingFrom = 0;
                var share = s.Share;
                _pixels = (_, b) => share > 0 ? Art(b, share) : new byte[b * b * 4];
                ItemIcons.Configure(l);
                long peakBytes = 0;
                var peakTextures = 0;
                var steady = 0;
                // a list of 1200 items, 10 a row, scrolled one row every 8 frames down and back (600 frames, ~10 s)
                for (var f = 0; f < 600; f++, _now += 16)
                {
                    var row = f / 8 % 120;
                    if (f / 8 / 120 % 2 == 1) row = 119 - row;
                    var first = (uint)(1 + Math.Min(row * 10, 1200 - s.Visible));
                    var waiting = DrawGrid(first, s.Visible, s.Pixels, s.Trim);
                    if (f >= 300) steady += waiting;
                    Footprint();
                    peakBytes = Math.Max(peakBytes, ItemIcons.Stats.Bytes);
                    peakTextures = Math.Max(peakTextures, ItemIcons.Stats.Textures);
                }
                var level = s.Trim ? ItemIconKey.Bucket(s.Pixels / s.Share) : ItemIconKey.Bucket(s.Pixels);
                sb.AppendLine($"| {name} | {s.Name} | {s.Visible} | {level} | {steady / 300.0:0.#} per frame | {_decoded.Count} | {peakTextures} | " +
                              $"{peakBytes / 1048576.0:0.#} | {_peakFootprint} |");
            }
        File.WriteAllText(path, sb.ToString());
    }
}
