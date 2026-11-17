// SPDX-License-Identifier: MIT
// The game's item icons in Kiln windows (ItemIcons.cs): which texture an item draws, the cache's limits and retries, the
// placeholder, a world change, and kiln.content's listing of new items (ContentNames). No game: the texture reads, the
// overlay and the clock are the tests' own.
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
    }

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
        // 600 different icons drawn in the same moment: at most 512 textures; the rest wait on the placeholder, and none that
        // is drawn now is dropped
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
        Assert.Equal(2, gone.Count);                                         // 1 and 2: the oldest that may go
        Assert.Equal(3, t.Count);
        Assert.True(t.TryGet(0, out _) && t.TryGet(3, out _) && t.TryGet(4, out _));
        Assert.Equal((300L, 3), (t.Bytes, t.Textures));
        Assert.False(t.Room(0));                                             // 3 textures: the limit
        Assert.Equal(3, t.Clear().Count);
        Assert.True(t.Room(1000) && !t.Room(1001));
        // entries without a texture (missing icons) do not count against the textures, only their own bound
        for (var i = 0; i < 12; i++) t.Add(100 + i, now: 0).Loading = false;
        Assert.True(t.Room(10));
        Assert.Empty(t.Trim(usedBefore: 1));                                 // 12 = 4 x 3: still within
        t.Add(200, now: 0).Loading = false;
        Assert.Single(t.Trim(usedBefore: 1));
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
}
