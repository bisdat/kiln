// SPDX-License-Identifier: MIT
// The game's own names (GameNames.cs): the readers of its loca collection, item, workshop and recipe data, on bytes laid out as
// client 26430625 lays them out (GameNameLayout.Measured), and Items.DisplayName's order (a new item's definition, the game's
// text in the game's language, the English name Kiln ships).
using System.Buffers.Binary;
using System.Text;
using Xunit;

namespace Kiln;

[Collection("game names")]
public sealed class GameNamesTests : IDisposable
{
    private static readonly GameNameLayout L = GameNameLayout.Measured;

    public GameNamesTests()
    {
        GameNames.ResetForTests();
        DefinedNames.ResetForTests();
    }

    public void Dispose()
    {
        GameNames.ResetForTests();
        DefinedNames.ResetForTests();
    }

    /// <summary>Bytes with blob arrays and strings appended after a fixed head, as the game's resources and blobs are.</summary>
    private sealed class Blob(int head)
    {
        public readonly List<byte> B = new(new byte[head]);
        public int Tail => B.Count;
        public void U32(int at, uint v) { var x = new byte[4]; BinaryPrimitives.WriteUInt32LittleEndian(x, v); for (var i = 0; i < 4; i++) B[at + i] = x[i]; }
        public void I32(int at, int v) => U32(at, unchecked((uint)v));
        public void Bytes(int at, byte[] v) { for (var i = 0; i < v.Length; i++) B[at + i] = v[i]; }
        public int Append(int n) { var at = B.Count; B.AddRange(new byte[n]); return at; }
        /// <summary>A BlobArray field at <paramref name="field"/> pointing at <paramref name="count"/> new elements; their start.</summary>
        public int Array(int field, int count, int stride) { var at = Append(count * stride); I32(field, at - field); U32(field + 4, (uint)count); return at; }
        public void String(int field, string s) { var bytes = Encoding.UTF8.GetBytes(s); var at = Append(bytes.Length); Bytes(at, bytes); I32(field, at - field); U32(field + 4, (uint)bytes.Length); }
        public byte[] Done => B.ToArray();
    }

    private static uint Fnv(Guid g) => GameNameData.Fnv16(g.ToByteArray());

    [Fact]
    public void ReadsALanguageTableOfTheLocaCollection()
    {
        // LocaTagCollectionResource: englishData (32), languages: BlobArray<{LanguageId @0, data @8 (hash 16)}> at 0x20
        var c = new Blob(40);
        var langs = c.Array(L.Languages, 2, L.LanguageStride);
        c.I32(langs + L.LanguageValue, 1);                                   // En_Us
        c.Bytes(langs + L.LanguageData, Enumerable.Repeat((byte)0x11, 16).ToArray());
        c.I32(langs + L.LanguageStride + L.LanguageValue, 3);                // De_De
        c.Bytes(langs + L.LanguageStride + L.LanguageData, Enumerable.Range(1, 16).Select(i => (byte)i).ToArray());
        Assert.Equal(Enumerable.Range(1, 16).Select(i => (byte)i).ToArray(), GameNameData.LanguageBlob(c.Done, L, 3));
        Assert.Null(GameNameData.LanguageBlob(c.Done, L, 9));

        // a language's blob: BlobArray<LocaTagResource 24 B {id @0, text @4}>, the strings behind it
        var t = new Blob(8);
        var tags = t.Array(0, 3, L.TagStride);
        t.U32(tags, 7);
        t.String(tags + L.TagText, "Holzscheite");
        t.U32(tags + L.TagStride, 42);
        t.String(tags + L.TagStride + L.TagText, "<b>Kohlenmeiler</b> ");
        t.U32(tags + 2 * L.TagStride, 0);                                     // no id: left out
        t.String(tags + 2 * L.TagStride + L.TagText, "ignored");
        var table = GameNameData.Table(t.Done, L)!;
        Assert.Equal(2, table.Count);
        Assert.Equal("Holzscheite", table[7]);
        Assert.Equal("Kohlenmeiler", table[42]);                             // markup out, trimmed
        // a string pointing outside the blob: not a table (never a wrong text)
        var bad = t.Done;
        BinaryPrimitives.WriteInt32LittleEndian(bad.AsSpan(tags + L.TagText), 100_000);
        Assert.Null(GameNameData.Table(bad, L));
        Assert.Null(GameNameData.Table(new byte[4], L));
    }

    [Fact]
    public void ReadsItemWorkshopAndRecipeNames()
    {
        var nameTag = Guid.Parse("0b6a3b1e-5b0f-4c8b-9c55-3a9e1f2d7c10");
        var item = new byte[0x60];
        nameTag.ToByteArray().CopyTo(item, L.ItemName);
        Assert.Equal(Fnv(nameTag), GameNameData.ItemNameTag(item, L));
        Assert.Equal(0u, GameNameData.ItemNameTag(new byte[0x60], L));       // no name: tag 0
        Assert.Equal(0u, GameNameData.ItemNameTag(new byte[0x50], L));       // too short

        // WorkshopRegistryResource: npcs @0, workshops @8: WorkshopResource 56 B {workshopId @0x14, name @0x18, item @0x1c}
        var w = new Blob(24);
        var ws = w.Array(L.Workshops, 2, L.WorkshopStride);
        w.U32(ws + L.WorkshopId, 0x4da88f73); w.U32(ws + L.WorkshopName, 99); w.U32(ws + L.WorkshopItem, 0x1234);
        w.U32(ws + L.WorkshopStride + L.WorkshopId, 0x56d94f8); w.U32(ws + L.WorkshopStride + L.WorkshopName, 100); w.U32(ws + L.WorkshopStride + L.WorkshopItem, 0x15e4e52e);
        var shops = GameNameData.Workshops(w.Done, L);
        Assert.Equal((99u, 0x1234u), shops[0x4da88f73]);
        Assert.Equal((100u, 0x15e4e52eu), shops[0x56d94f8]);

        // RecipeRegistryResource: recipes @0: RecipeInfo 168 B {recipeId @0x10, output @0x30 (24 B {item @0x10}), recipeName @0x68}
        var r = new Blob(8);
        var rs = r.Array(L.Recipes, 2, L.RecipeStride);
        r.U32(rs + L.RecipeId, 0x8e78753c);
        r.Bytes(rs + L.RecipeName, nameTag.ToByteArray());
        var outs = r.Array(rs + L.RecipeOutput, 2, L.OutputStride);
        r.U32(outs + L.OutputItem, 0x162cb6d2);
        r.U32(outs + L.OutputStride + L.OutputItem, 0x2b0cf1d5);
        r.U32(rs + L.RecipeStride + L.RecipeId, 0x222fcad4);                // no name, no outputs
        var recipes = GameNameData.Recipes(r.Done, L);
        Assert.Equal((Fnv(nameTag), 0x162cb6d2u), recipes[0x8e78753c]);
        Assert.Equal((0u, 0u), recipes[0x222fcad4]);

        Assert.Equal("de-de", GameNameData.LanguageKey("De_De"));
        Assert.Equal("es-419", GameNameData.LanguageKey("Es_419"));
        Assert.Equal(0u, GameNameData.Fnv16(new byte[16]));
    }

    [Fact]
    public void DisplayNameIsTheDefinitionsThenTheGamesThenTheShippedEnglishName()
    {
        var logs = new ItemId(722268629);                                     // Material_T1_Wood_Logs, "Wood Logs" in items.tsv
        var charcoal = new ItemId(0x162cb6d2);
        var mine = new ItemId(0xdeadbeef);
        // nothing loaded (a server, or before the read): the English name Kiln ships; an unknown id: null
        GameNames.UseForTests(L, new(), new(), new(), "de-de", texts: null);
        Assert.Equal("Wood Logs", Items.DisplayName(logs));
        Assert.Null(Items.DisplayName(mine));
        Assert.Null(Items.DisplayName(new ItemId(0)));
        // the game's own text in the game's language
        GameNames.UseForTests(L, new() { [logs.Value] = Guid.NewGuid(), [charcoal.Value] = Guid.NewGuid() }, new(), new(), "de-de",
            texts: new() { [77] = "Holzscheite" }, itemTags: new() { [logs.Value] = 77, [charcoal.Value] = 78 });
        Assert.Equal("Holzscheite", Items.DisplayName(logs));
        Assert.Equal("Charcoal", Items.DisplayName(charcoal));               // no text for its tag: the shipped English name
        // a new item: its definition's name, before anything else
        DefinedNames.Override = (id, lang) => id == mine.Value ? $"Mein Ding ({lang})" : null;
        Assert.Equal("Mein Ding (de-de)", Items.DisplayName(mine));
        Assert.Equal("Holzscheite", Items.DisplayName(logs));
    }

    [Fact]
    public void WorkshopAndRecipeLookupsAnswerFromTheGamesData()
    {
        GameNames.UseForTests(L, new(), new() { [0x15e4e52e] = (5u, 0x15e4e52eu) }, new() { [0x8e78753c] = (6u, 0x162cb6d2u) }, "en-us",
            texts: new() { [5] = "Charcoal Kiln", [6] = "" });
        Assert.Equal((5u, new ItemId(0x15e4e52e)), GameNames.Workshop(0x15e4e52e));
        Assert.Null(GameNames.Workshop(1));
        Assert.Equal((6u, new ItemId(0x162cb6d2)), GameNames.Recipe(0x8e78753c));
        Assert.Equal("Charcoal Kiln", GameNames.Text(5));
        Assert.Null(GameNames.Text(6));                                       // an empty text is no name
        Assert.Null(GameNames.Text(0));
    }

    [Fact]
    public void ARetriedReadWaitsLongerEachTimeUpToItsLimit()
    {
        // the game's names (30 s, doubling to 10 min) and their language tables (5 s to 5 min) back off with RetryDelay
        Assert.Equal(0, RetryDelay.Of(0, 30_000, 600_000));
        Assert.Equal([30_000L, 60_000, 120_000, 240_000, 480_000, 600_000, 600_000],
            Enumerable.Range(1, 7).Select(n => RetryDelay.Of(n, 30_000, 600_000)));
        Assert.Equal(300_000, RetryDelay.Of(1_000, 5_000, 300_000));         // no overflow however many failures
    }
}
