// SPDX-License-Identifier: MIT
// The game's item icons in Kiln windows (ItemIcons.cs): which texture an item draws, the cache's limits and retries, the
// placeholder, a world change, and kiln.content's listing of new items (ContentNames). No game: the texture reads, the
// overlay and the clock are the tests' own.
using System.Runtime.CompilerServices;
using System.Text.RegularExpressions;
using Xunit;

namespace Kiln;

[Collection(FakeUi.UiSerial.Name)]
public sealed class ItemIconsTests : IDisposable
{
    private static readonly Guid Logs = Guid.Parse("3f4093ae-75ed-4e5d-bd0b-784ba83b88f5");
    private static readonly Guid Sword = Guid.Parse("dd7f33fd-6383-495b-b365-1138918a9e4c");
    private static readonly Guid Mine = Guid.Parse("6a4e2c11-0b3d-4f62-9a51-0d8e2f7c4b10");

    private long _now = 1_000_000;
    private ulong _next = 100;
    private readonly List<(Guid Texture, int Bucket)> _decoded = [];
    private readonly List<ulong> _freed = [];
    private readonly Dictionary<Guid, int> _served = [];
    private readonly Dictionary<ulong, int> _states = [];  // kiln.ui's image_state of an upload (default: ready)
    private uint _session = 1;

    public ItemIconsTests()
    {
        ItemIcons.ResetForTests();
        ContentNames.ResetForTests();
        ItemIcons.Synchronous = true;
        ItemIcons.Clock = () => _now;
        ItemIcons.Session = () => _session;
        ItemIcons.GameItem = id => id.Value == NewContent.IdOf(Logs) ? Logs : id.Value == NewContent.IdOf(Sword) ? Sword : null;
        ItemIcons.Defined = _ => null;
        ItemIcons.IconState = g => _served.GetValueOrDefault(g);
        ItemIcons.Decode = (g, bucket) =>
        {
            _decoded.Add((g, bucket));
            return g == Guid.Empty ? null : (new byte[bucket * bucket * 4], bucket, bucket);
        };
        ItemIcons.Upload = (px, w, h) => ++_next;
        ItemIcons.Free = h => _freed.Add(h);
        ItemIcons.ImageState = h => _states.GetValueOrDefault(h, ImageReady);
    }

    private const int ImageWaiting = 1, ImageReady = 2, ImageFailed = 3;  // KILN_UI_IMAGE_* (kiln_ui.h)

    public void Dispose()
    {
        ItemIcons.ResetForTests();
        ContentNames.ResetForTests();
    }

    private static ItemId Id(Guid g) => new(NewContent.IdOf(g));

    [Fact]
    public void AnItemDrawsTheTextureTheGameDrawsForIt()
    {
        var defs = new Dictionary<uint, ContentNames.Def>
        {
            [Id(Mine).Value] = new(Id(Mine).Value, Mine, IconGuid: Sword, BaseGuid: Sword, "Frost Edge", new Dictionary<string, string>(), HasIconFile: true),
        };
        Guid? Key(ItemId i, out bool final) => ItemIconKey.Of(i, id => defs.GetValueOrDefault(id), g => _served.GetValueOrDefault(g),
            id => id.Value == Id(Logs).Value ? Logs : null, out final);

        Assert.Equal(Logs, Key(Id(Logs), out var final));                    // a game item: its own GUID, for good
        Assert.True(final);
        Assert.Equal(Sword, Key(Id(Mine), out final));                       // a new item whose PNG is not served yet: its base's
        Assert.False(final);                                                 // ... asked again later
        _served[Mine] = ItemIconKey.Served;
        Assert.Equal(Mine, Key(Id(Mine), out final));                        // served: its own
        Assert.True(final);
        defs[Id(Mine).Value] = defs[Id(Mine).Value] with { HasIconFile = false };
        _served.Clear();
        Assert.Equal(Sword, Key(Id(Mine), out final));                       // no icon file: the base's for good
        Assert.True(final);
        defs[Id(Mine).Value] = defs[Id(Mine).Value] with { IconGuid = null };
        Assert.Null(Key(Id(Mine), out final));                               // not in a world yet: nothing to draw
        Assert.False(final);
        Assert.Null(Key(new ItemId(0x1234), out final));                     // unknown
        Assert.Null(Key(new ItemId(0), out final));

        Assert.Equal(32, ItemIconKey.Bucket(20));
        Assert.Equal(64, ItemIconKey.Bucket(48));
        Assert.Equal(64, ItemIconKey.Bucket(64));
        Assert.Equal(128, ItemIconKey.Bucket(96));
        Assert.Equal(256, ItemIconKey.Bucket(300));
    }

    [Fact]
    public void TheFirstDrawShowsThePlaceholderThenTheIconFromTheCache()
    {
        var h1 = ItemIcons.Handle(Id(Logs), 48, out var w, out _);
        Assert.Equal(ItemIcons.Placeholder(), h1);                           // loaded off the draw: the placeholder this frame
        Assert.Equal(16, w);
        Assert.Equal([(Logs, 64)], _decoded);                                // a 48-pixel icon decodes the 64 level
        var h2 = ItemIcons.Handle(Id(Logs), 48, out w, out var h);
        Assert.NotEqual(h1, h2);
        Assert.Equal((64, 64), (w, h));
        Assert.Equal(h2, ItemIcons.Handle(Id(Logs), 60, out _, out _));      // the same bucket: no second read
        Assert.Single(_decoded);
        ItemIcons.Handle(Id(Logs), 128, out _, out _);                       // a larger draw reads a larger level
        Assert.Equal((Logs, 128), _decoded[^1]);
        Assert.Equal((2, 2), (ItemIcons.Stats.Count, ItemIcons.Stats.Textures));
    }

    [Fact]
    public void AnUnknownItemShowsThePlaceholderAndIsAskedAgainLater()
    {
        var unknown = new ItemId(0x0badf00d);
        var p = ItemIcons.Placeholder();
        Assert.Equal(p, ItemIcons.Handle(unknown, 64, out _, out _));
        Assert.Equal(p, ItemIcons.Handle(unknown, 64, out _, out _));
        Assert.Empty(_decoded);                                              // nothing to read for it
        Assert.Equal(1, ItemIcons.Stats.Missing);
        // a definition arrives from the server: after the retry time it is resolved again
        ItemIcons.GameItem = id => id == unknown ? Sword : null;
        ItemIcons.Handle(unknown, 64, out _, out _);
        Assert.Empty(_decoded);                                              // not yet: 5 s
        _now += 5_000;
        ItemIcons.Handle(unknown, 64, out _, out _);
        Assert.Equal([(Sword, 64)], _decoded);
        Assert.NotEqual(p, ItemIcons.Handle(unknown, 64, out _, out _));
    }

    [Fact]
    public void ATextureThatCannotBeReadOrUploadedDrawsThePlaceholderNeverThrows()
    {
        ItemIcons.GameItem = _ => Guid.Empty;                                 // Decode answers null for it
        var p = ItemIcons.Placeholder();
        Assert.Equal(p, ItemIcons.Handle(new ItemId(5), 64, out _, out _));
        Assert.Equal(p, ItemIcons.Handle(new ItemId(5), 64, out _, out _));
        ItemIcons.GameItem = _ => throw new InvalidOperationException("kiln.assets is gone");
        Assert.Equal(p, ItemIcons.Handle(new ItemId(6), 64, out _, out _));
        Assert.Equal(p, ItemIcons.Handle(new ItemId(6), 64, out _, out _));
        // no overlay image at all: 0 (the draw keeps the layout with an empty box)
        ItemIcons.ResetForTests();
        ItemIcons.Synchronous = true;
        ItemIcons.Upload = (_, _, _) => 0;
        ItemIcons.GameItem = _ => Logs;
        ItemIcons.Decode = (_, b) => (new byte[b * b * 4], b, b);
        ItemIcons.Session = () => 1;
        Assert.Equal(0ul, ItemIcons.Handle(new ItemId(7), 64, out _, out _));
        Assert.Equal(0ul, ItemIcons.Handle(new ItemId(7), 64, out _, out _));
    }

    [Fact]
    public void TheCacheStaysWithinItsLimitsDroppingIconsNotDrawnForASecond()
    {
        ItemIcons.GameItem = id => new Guid((int)id.Value, 0, 0, new byte[8]);
        // 600 different icons drawn in the same moment: at most MaxEntries textures; the rest wait on the placeholder, and none
        // that is drawn now is dropped
        for (uint i = 1; i <= 600; i++) ItemIcons.Handle(new ItemId(i), 32, out _, out _);
        Assert.Equal(ItemIcons.MaxEntries, ItemIcons.Stats.Textures);
        Assert.Empty(_freed);
        Assert.Equal(ItemIcons.Placeholder(), ItemIcons.Handle(new ItemId(600), 32, out _, out _));
        // two seconds later only 10 of them are still drawn, and 100 new ones: old ones go, oldest first, the 10 stay
        _now += 2_000;
        for (uint i = 1; i <= 10; i++) ItemIcons.Handle(new ItemId(i), 32, out _, out _);
        for (uint i = 601; i <= 700; i++) ItemIcons.Handle(new ItemId(i), 32, out _, out _);
        for (var round = 0; round < 3; round++)
        {
            _now += 300;  // the waiting ones ask again
            for (uint i = 601; i <= 700; i++) ItemIcons.Handle(new ItemId(i), 32, out _, out _);
            for (uint i = 1; i <= 10; i++) ItemIcons.Handle(new ItemId(i), 32, out _, out _);
        }
        Assert.True(ItemIcons.Stats.Textures <= ItemIcons.MaxEntries, $"{ItemIcons.Stats.Textures} textures held");
        Assert.NotEmpty(_freed);
        for (uint i = 1; i <= 10; i++) Assert.NotEqual(ItemIcons.Placeholder(), ItemIcons.Handle(new ItemId(i), 32, out _, out _));
        for (uint i = 601; i <= 700; i++) Assert.NotEqual(ItemIcons.Placeholder(), ItemIcons.Handle(new ItemId(i), 32, out _, out _));
        // bytes: 256-pixel icons are 256 KiB each; 24 MiB hold 96
        for (uint i = 1000; i < 1200; i++)
        {
            _now += 1_500;
            ItemIcons.Handle(new ItemId(i), 256, out _, out _);
        }
        Assert.True(ItemIcons.Stats.Bytes <= ItemIcons.MaxBytes, $"{ItemIcons.Stats.Bytes} bytes held");
        Assert.True(ItemIcons.Stats.Count <= 4 * ItemIcons.MaxEntries);
    }

    [Fact]
    public void ANewItemShowsItsBaseIconUntilItsOwnIsServedThenItsOwn()
    {
        var def = new ContentNames.Def(Id(Mine).Value, Mine, IconGuid: Sword, BaseGuid: Sword, "Frost Edge", new Dictionary<string, string>(), HasIconFile: true);
        ItemIcons.Defined = id => id == def.Id ? def : null;
        ItemIcons.Handle(Id(Mine), 64, out _, out _);
        var alias = ItemIcons.Handle(Id(Mine), 64, out _, out _);
        Assert.Equal([(Sword, 64)], _decoded);                               // the base's (the Bronze Sword's) texture meanwhile
        _served[Mine] = ItemIconKey.Served;                                  // kiln.assets serves the mod's PNG now
        Assert.Equal(alias, ItemIcons.Handle(Id(Mine), 64, out _, out _));   // not before the retry time
        _now += 5_000;
        Assert.Equal(alias, ItemIcons.Handle(Id(Mine), 64, out _, out _));   // read again; the old picture this frame
        Assert.Equal((Mine, 64), _decoded[^1]);
        var own = ItemIcons.Handle(Id(Mine), 64, out _, out _);
        Assert.NotEqual(alias, own);
        Assert.Contains(alias, _freed);                                      // the base's copy is freed
        _now += 60_000;
        ItemIcons.Handle(Id(Mine), 64, out _, out _);
        Assert.Equal(2, _decoded.Count);                                     // its own icon is for good: no more reads
    }

    [Fact]
    public void ANewClientWorldDropsEveryIcon()
    {
        ItemIcons.Handle(Id(Logs), 64, out _, out _);
        var icon = ItemIcons.Handle(Id(Logs), 64, out _, out _);
        _session = 2;
        _now += 1_500;                                                       // the session is looked at once a second
        ItemIcons.Handle(Id(Sword), 64, out _, out _);
        Assert.Contains(icon, _freed);
        Assert.NotEqual(icon, ItemIcons.Handle(Id(Logs), 64, out _, out _)); // read again
    }

    [Fact]
    public void ThePlaceholderIsANeutralFrame()
    {
        var px = ItemIcons.PlaceholderPixels();
        Assert.Equal(16 * 16 * 4, px.Length);
        Assert.Equal(0, px[3]);                                              // the corner is clear
        Assert.Equal(90, px[(1 * 16 + 0) * 4 + 3]);                          // the edge
        Assert.Equal(36, px[(8 * 16 + 8) * 4 + 3]);                          // the inside, faint
        Assert.All(Enumerable.Range(0, 256), i => Assert.True(px[i * 4] == px[i * 4 + 1] && px[i * 4] == px[i * 4 + 2]));  // grey
    }

    [Fact]
    public void TheTablesTrimNeverTakesWhatIsLoadingOrStillDrawn()
    {
        var t = new IconTable<int>(maxEntries: 3, maxBytes: 1000);
        for (var i = 0; i < 5; i++)
        {
            var e = t.Add(i, now: i * 10);
            e.Loading = i == 0;                                              // 0 still loads
            t.SetBytes(e, 100);
        }
        Assert.Equal(5, t.Textures);
        var gone = t.Trim(usedBefore: 35);                                   // 4 (used at 40) stays drawn
        Assert.Equal(2, gone!.Count);                                        // 1 and 2: the oldest that may go
        Assert.Equal(3, t.Count);
        Assert.True(t.TryGet(0, out _) && t.TryGet(3, out _) && t.TryGet(4, out _));
        Assert.Equal((300L, 3), (t.Bytes, t.Textures));
        Assert.False(t.Room(0));                                             // 3 textures: the limit
        Assert.Equal(3, t.Clear().Count);
        Assert.True(t.Room(1000) && !t.Room(1001));
        // entries without a texture (missing icons) do not count against the textures, only their own bound
        for (var i = 0; i < 12; i++) t.Add(100 + i, now: 0).Loading = false;
        Assert.True(t.Room(10));
        Assert.Null(t.Trim(usedBefore: 1));                                  // 12 = 4 x 3: still within
        t.Add(200, now: 0).Loading = false;
        Assert.Single(t.Trim(usedBefore: 1)!);
    }

    [Fact]
    public void ContentNamesReadsNewItemsFromKilnContentsListing()
    {
        var listing = $$"""
        [
          {"kind": "item", "guid": "{{Mine}}", "id": "0x{{NewContent.IdOf(Mine):x8}}", "name": "frost_edge", "state": "registered",
           "displayName": "Frost Edge", "iconGuid": "{{Sword}}", "baseGuid": "{{Sword}}", "iconFile": "icons/frost.png",
           "definition": { "guid": "{{Mine}}", "texts": { "de": { "name": "Frostkante" }, "pt-BR": { "name": "Gume de Gelo" }, "*": { "caption": "x" } } } },
          {"kind": "item", "guid": "{{Logs}}", "id": "0x1", "name": "pending_thing", "state": "pending", "iconGuid": "00000000-0000-0000-0000-000000000000"},
          {"kind": "recipe", "guid": "{{Sword}}", "id": "0x2", "name": "a_recipe", "state": "registered"}
        ]
        """;
        var defs = ContentNames.Parse(listing);
        Assert.Equal(2, defs.Count);
        var d = defs[NewContent.IdOf(Mine)];
        Assert.Equal((Mine, (Guid?)Sword, (Guid?)Sword, true), (d.Guid, d.IconGuid, d.BaseGuid, d.HasIconFile));
        Assert.Equal("Frostkante", ContentNames.NameFor(d, "de-de"));        // the primary language
        Assert.Equal("Gume de Gelo", ContentNames.NameFor(d, "pt-br"));      // the whole key, any case
        Assert.Equal("Frost Edge", ContentNames.NameFor(d, "fr-fr"));        // else its display name
        var pending = defs[NewContent.IdOf(Logs)];
        Assert.Null(pending.IconGuid);                                       // all zero: not in a world yet
        Assert.Null(ContentNames.NameFor(pending, "en-us"));                 // names nothing: its base item's name is used

        // Find never waits: the first ask starts the read, FindNow (a worker) waits for it
        ContentNames.ListingOverride = () => listing;
        Assert.Equal(Mine, ContentNames.FindNow(NewContent.IdOf(Mine))?.Guid);
        Assert.Equal(Mine, ContentNames.Find(NewContent.IdOf(Mine))?.Guid);
        Assert.Null(ContentNames.FindNow(0x5));                              // read less than 2 s ago: not again
    }

    // ---- review of 0.2.0-dev.29.15: the overlay's image budget, failed uploads, wasted reads ----

    private static string RepoRoot([CallerFilePath] string source = "")
    {
        foreach (var start in new[] { AppContext.BaseDirectory, Path.GetDirectoryName(source) ?? "" })
            for (var d = new DirectoryInfo(start); d != null; d = d.Parent)
                if (File.Exists(Path.Combine(d.FullName, "native", "services", "ui", "vk.cpp")) &&
                    Directory.Exists(Path.Combine(d.FullName, "managed", "tests", "Kiln.Ui.Tests")))
                    return d.FullName;
        throw new InvalidOperationException("repository root not found above the test assembly or its source");
    }

    /// <summary>kiln.ui's image budget (native/services/ui/image_budget.h): the images it lets exist at once.</summary>
    private static int OverlayMaxImages()
    {
        var text = File.ReadAllText(Path.Combine(RepoRoot(), "native", "services", "ui", "image_budget.h"));
        int Num(string name) => int.Parse(Regex.Match(text, $@"constexpr uint32_t {name} = (\d+);").Groups[1].Value);
        return Num("kDescriptorPoolSize") - Num("kReserved");
    }

    [Fact]
    public void TheCacheStaysWellUnderTheOverlaysImageBudget()
    {
        // the overlay's descriptor pool held 64 sets while this cache allowed 512 textures: past 64 a texture got no set and
        // was drawn with a null one. The cache (plus the placeholder) takes at most half of what kiln.ui lets exist: the rest
        // are mods' own pictures (Ui.LoadImage) and Kiln's.
        var max = OverlayMaxImages();
        Assert.True(ItemIcons.MaxEntries + 1 <= max / 2, $"{ItemIcons.MaxEntries} icons + the placeholder against {max} overlay images");

        // 700 distinct icons, drawn 100 at a time as a list scrolls, then all at once: never more images than the cache allows
        var live = new HashSet<ulong>();
        var refused = 0;
        var most = 0;
        ItemIcons.Upload = (_, _, _) =>
        {
            if (live.Count >= max)
            {
                refused++;
                return 0;
            }
            live.Add(++_next);
            most = Math.Max(most, live.Count);
            return _next;
        };
        ItemIcons.Free = h => live.Remove(h);
        ItemIcons.GameItem = id => new Guid((int)id.Value, 0, 0, new byte[8]);
        for (var frame = 0; frame < 400; frame++)
        {
            _now += 16;
            var first = (uint)(frame / 10 * 25 % 700);
            for (uint i = 0; i < 100; i++) ItemIcons.Handle(new ItemId(1 + (first + i) % 700), 64, out _, out _);
        }
        for (var frame = 0; frame < 50; frame++)
        {
            _now += 16;
            for (uint i = 1; i <= 700; i++) ItemIcons.Handle(new ItemId(i), 64, out _, out _);
        }
        Assert.Equal(0, refused);
        Assert.True(most <= ItemIcons.MaxEntries + 1, $"{most} overlay images at once");
    }

    [Fact]
    public void AnIconIsDrawnOnlyOnceTheOverlayUploadedItAndAFailedUploadIsFreedAndRetriedLater()
    {
        ItemIcons.Upload = (_, _, _) =>
        {
            _states[++_next] = ImageWaiting;  // a new image waits for the next frames' uploads
            return _next;
        };
        var placeholder = ItemIcons.Placeholder();
        _states[placeholder] = ImageReady;
        Assert.Equal(placeholder, ItemIcons.Handle(Id(Logs), 64, out _, out _));
        var icon = _next;
        Assert.Equal(placeholder, ItemIcons.Handle(Id(Logs), 64, out _, out _));  // uploaded, not drawn yet
        _states[icon] = ImageReady;
        Assert.Equal(icon, ItemIcons.Handle(Id(Logs), 64, out _, out _));

        // no descriptor set (the pool was full): never drawn, freed, the placeholder; read again later, later each time
        ItemIcons.Handle(Id(Sword), 64, out _, out _);
        var failed = _next;
        _states[failed] = ImageFailed;
        Assert.Equal(placeholder, ItemIcons.Handle(Id(Sword), 64, out _, out _));
        Assert.Contains(failed, _freed);
        var reads = _decoded.Count;
        _now += 1_000;
        Assert.Equal(placeholder, ItemIcons.Handle(Id(Sword), 64, out _, out _));
        Assert.Equal(reads, _decoded.Count);                                 // not at once
        _now += 5_000;
        ItemIcons.Handle(Id(Sword), 64, out _, out _);
        Assert.Equal(reads + 1, _decoded.Count);                             // after 5 s
        _states[_next] = ImageFailed;
        ItemIcons.Handle(Id(Sword), 64, out _, out _);                       // fails again
        _now += 6_000;
        ItemIcons.Handle(Id(Sword), 64, out _, out _);
        Assert.Equal(reads + 1, _decoded.Count);                             // the next wait is longer: 10 s
        _now += 5_000;
        ItemIcons.Handle(Id(Sword), 64, out _, out _);
        Assert.Equal(reads + 2, _decoded.Count);
    }

    [Fact]
    public void AnIconOutsideItsWindowIsNotLoaded()
    {
        // the window's rectangle and an icon's box at the cursor, screen pixels
        Assert.True(ItemIcons.OnScreen(110, 120, 64, 64, 100, 100, 400, 300));
        Assert.True(ItemIcons.OnScreen(60, 120, 64, 64, 100, 100, 400, 300));   // half in on the left
        Assert.False(ItemIcons.OnScreen(110, 420, 64, 64, 100, 100, 400, 300)); // scrolled below the window
        Assert.False(ItemIcons.OnScreen(110, 30, 64, 64, 100, 100, 400, 300));  // scrolled above it
        Assert.False(ItemIcons.OnScreen(520, 120, 64, 64, 100, 100, 400, 300));
        Assert.True(ItemIcons.OnScreen(110, 9000, 64, 64, 100, 100, 0, 0));     // no window size known: drawn
    }

    [Fact]
    public void NoTextureIsReadWhileTheCacheIsFullOfIconsDrawnNow()
    {
        ItemIcons.GameItem = id => new Guid((int)id.Value, 0, 0, new byte[8]);
        for (uint i = 1; i <= ItemIcons.MaxEntries; i++) ItemIcons.Handle(new ItemId(i), 32, out _, out _);
        Assert.Equal(ItemIcons.MaxEntries, ItemIcons.Stats.Textures);
        var reads = _decoded.Count;
        // one more, while every icon is drawn this second: it waits on the placeholder without reading its texture
        for (var frame = 0; frame < 20; frame++)
        {
            _now += 100;
            for (uint i = 1; i <= ItemIcons.MaxEntries + 1; i++) ItemIcons.Handle(new ItemId(i), 32, out _, out _);
        }
        Assert.Equal(reads, _decoded.Count);
        Assert.Equal(ItemIcons.Placeholder(), ItemIcons.Handle(new ItemId((uint)ItemIcons.MaxEntries + 1), 32, out _, out _));
    }

    [Fact]
    public void AMissingIconIsLookedForLessOftenEachTime()
    {
        ItemIcons.GameItem = _ => Guid.Empty;                                // a game item whose texture cannot be read
        ItemIcons.Handle(new ItemId(5), 64, out _, out _);
        Assert.Single(_decoded);
        var at = new List<long>();
        var start = _now;
        for (var s = 0; s < 2_000; s++)
        {
            _now += 1_000;
            var before = _decoded.Count;
            ItemIcons.Handle(new ItemId(5), 64, out _, out _);
            if (_decoded.Count > before) at.Add((_now - start) / 1_000);
        }
        // 60 s, then 120 s, 240 s, 480 s, then every 10 minutes
        Assert.Equal([60, 180, 420, 900], at.Take(4));
        Assert.Equal(600, at[4] - at[3]);
    }

    [Fact]
    public void ADrawOfACachedIconAllocatesNothing()
    {
        ItemIcons.Handle(Id(Logs), 64, out _, out _);
        ItemIcons.Handle(Id(Logs), 64, out _, out _);
        var before = GC.GetAllocatedBytesForCurrentThread();
        for (var i = 0; i < 1_000; i++) ItemIcons.Handle(Id(Logs), 64, out _, out _);
        Assert.Equal(0, GC.GetAllocatedBytesForCurrentThread() - before);
        var t = new IconTable<int>(maxEntries: 3, maxBytes: 1000);
        t.Add(1, now: 0).Loading = false;
        Assert.Null(t.Trim(usedBefore: 10));                                 // within the limits: nothing made
    }

    [Fact]
    public void AnIconLargerThanTheWholeCacheIsNeverReadAgain()
    {
        ItemIcons.Decode = (g, _) =>
        {
            _decoded.Add((g, 0));
            return (new byte[4], 4096, 4096);                                // 64 MiB of pixels: over the 24 MiB the cache holds
        };
        var uploads = 0;
        ItemIcons.Upload = (_, _, _) =>
        {
            uploads++;
            return ++_next;
        };
        var placeholder = ItemIcons.Placeholder();
        uploads = 0;
        for (var s = 0; s < 100; s++)
        {
            _now += 60_000;
            Assert.Equal(placeholder, ItemIcons.Handle(Id(Logs), 256, out _, out _));
        }
        Assert.Single(_decoded);
        Assert.Equal(0, uploads);
    }

    [Fact]
    public void AnUploadThatThrowsOrRunsOutOfMemoryLeavesNothingStuck()
    {
        var throws = true;
        ItemIcons.Upload = (_, _, _) => throws ? throw new InvalidOperationException("kiln.ui went away") : ++_next;
        Assert.Equal(0ul, ItemIcons.Placeholder());                          // its own upload throws: none, no exception
        throws = false;
        _now += 5_000;
        var placeholder = ItemIcons.Placeholder();                           // made a little later
        Assert.NotEqual(0ul, placeholder);
        throws = true;
        Assert.Equal(placeholder, ItemIcons.Handle(Id(Logs), 64, out _, out _));
        throws = false;
        _now += 5_000;
        ItemIcons.Handle(Id(Logs), 64, out _, out _);
        Assert.NotEqual(placeholder, ItemIcons.Handle(Id(Logs), 64, out _, out _));  // read again, drawn

        // out of memory while reading: the request is dropped, never thrown at the draw, and asked again later
        var oom = true;
        ItemIcons.Decode = (g, b) => oom ? throw new OutOfMemoryException() : (new byte[b * b * 4], b, b);
        Assert.Equal(placeholder, ItemIcons.Handle(Id(Sword), 64, out _, out _));
        oom = false;
        _now += 5_000;
        ItemIcons.Handle(Id(Sword), 64, out _, out _);
        Assert.NotEqual(placeholder, ItemIcons.Handle(Id(Sword), 64, out _, out _));
    }

    [Fact]
    public void ANewItemsBaseIconIsNotReadAgainWhileItsOwnIsNotServed()
    {
        var def = new ContentNames.Def(Id(Mine).Value, Mine, IconGuid: Sword, BaseGuid: Sword, "Frost Edge", new Dictionary<string, string>(), HasIconFile: true);
        ItemIcons.Defined = id => id == def.Id ? def : null;
        ItemIcons.Handle(Id(Mine), 64, out _, out _);
        for (var s = 0; s < 120; s++)
        {
            _now += 1_000;
            ItemIcons.Handle(Id(Mine), 64, out _, out _);
        }
        Assert.Single(_decoded);                                             // looked at again, the same texture: not read again
    }

    [Fact]
    public void KilnContentsListingIsReadLessOftenWhileItDoesNotChange()
    {
        var reads = 0;
        var now = 1_000_000L;
        ContentNames.Clock = () => now;
        ContentNames.ListingOverride = () =>
        {
            reads++;
            return "[]";
        };
        // a draw of a game item asks for a new item first: every few seconds for a minute
        for (var s = 0; s < 60; s++)
        {
            now += 1_000;
            ContentNames.FindNow(0x1234);
        }
        Assert.InRange(reads, 4, 7);                                         // 2 s, 4 s, 6 s, 10 s, 18 s, 34 s ...: not 30
        // the listing changes (a world loads its new items): quick again
        ContentNames.ListingOverride = () =>
        {
            reads++;
            return reads < 100 ? $"[{{\"kind\":\"recipe\",\"guid\":\"{Guid.NewGuid()}\",\"id\":\"0x2\"}}]" : "[]";
        };
        now += 200_000;
        reads = 0;
        for (var s = 0; s < 10; s++)
        {
            now += 2_000;
            ContentNames.FindNow(0x1234);
        }
        Assert.Equal(10, reads);
    }
}
