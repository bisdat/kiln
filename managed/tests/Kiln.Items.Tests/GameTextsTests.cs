// SPDX-License-Identifier: MIT
// The game's texts in a chosen language (0.2.0-dev.31.13, mod API 0.2.22): Kiln.Api's GameTextData readers on loca bytes laid
// out as client 26430625 lays them out (GameTextLayout.Measured), the per-language store (GameTextStore), and the public
// members built on it: Content.TextLanguages / TryGetTexts / LoadTextsAsync / TextIn, Items.TextTags / Description /
// DisplayName(item, language). Invented texts and ids.
using System.Buffers.Binary;
using System.Text;
using Xunit;

namespace Kiln;

/// <summary>Bytes with blob arrays and strings appended after a fixed head, as the game's resources and blobs are.</summary>
internal sealed class TestBlob(int head)
{
    public readonly List<byte> B = new(new byte[head]);
    public void U32(int at, uint v) { var x = new byte[4]; BinaryPrimitives.WriteUInt32LittleEndian(x, v); for (var i = 0; i < 4; i++) B[at + i] = x[i]; }
    public void I32(int at, int v) => U32(at, unchecked((uint)v));
    public void U8(int at, byte v) => B[at] = v;
    public void Bytes(int at, byte[] v) { for (var i = 0; i < v.Length; i++) B[at + i] = v[i]; }
    public int Append(int n) { var at = B.Count; B.AddRange(new byte[n]); return at; }
    /// <summary>A BlobArray field at <paramref name="field"/> pointing at <paramref name="count"/> new elements; their start.</summary>
    public int Array(int field, int count, int stride) { var at = Append(count * stride); I32(field, at - field); U32(field + 4, (uint)count); return at; }
    public void String(int field, string s) => Raw(field, Encoding.UTF8.GetBytes(s));
    public void Raw(int field, byte[] bytes) { var at = Append(bytes.Length); Bytes(at, bytes); I32(field, at - field); U32(field + 4, (uint)bytes.Length); }
    public byte[] Done => B.ToArray();
}

/// <summary>A scripted text source: the languages and tables a test gives, counting reads.</summary>
internal sealed class ScriptedTexts : IGameTextSource
{
    public string? Why;
    public readonly Dictionary<string, Dictionary<uint, string>> Tables = new(StringComparer.Ordinal);
    public int Reads;
    public Func<string, Dictionary<uint, string>?>? Read2;
    public ManualResetEventSlim? Gate;

    public string? Unavailable => Why;
    public IReadOnlyList<string> Languages => Why is null ? [.. Tables.Keys] : [];

    public Dictionary<uint, string>? Read(string language, out string reason)
    {
        Gate?.Wait(TimeSpan.FromSeconds(10));
        Interlocked.Increment(ref Reads);
        if (Tables.TryGetValue(language, out var t))
        {
            reason = "";
            return new Dictionary<uint, string>(t);
        }
        reason = GameTextStore.NotShipped(language, Languages);
        return null;
    }
}

[Collection("game names")]
public sealed class GameTextsTests : IDisposable
{
    private static readonly GameTextLayout L = GameTextLayout.Measured;

    public GameTextsTests()
    {
        GameTextStore.ResetForTests();
        GameItemTexts.ResetForTests();
        GameNames.ResetForTests();
        DefinedNames.ResetForTests();
        GameLanguageSource.Override = () => "de-de";
    }

    public void Dispose()
    {
        GameTextStore.ResetForTests();
        GameItemTexts.ResetForTests();
        GameNames.ResetForTests();
        DefinedNames.ResetForTests();
        GameLanguageSource.ResetForTests();
        Texts.Reset();
    }

    private static ScriptedTexts Client()
    {
        var s = new ScriptedTexts();
        s.Tables["en-us"] = new() { [7] = "Wood Logs", [8] = "<b>Chopped</b> wood.", [9] = "" };
        s.Tables["de-de"] = new() { [7] = "Holzscheite", [8] = "<b>Gehacktes</b> Holz." };
        s.Tables["pt-br"] = new() { [7] = "Toras" };
        GameTextStore.UseSource(s);
        return s;
    }

    private static T Eventually<T>(Func<T?> f) where T : class
    {
        var until = DateTime.UtcNow.AddSeconds(10);
        while (DateTime.UtcNow < until)
        {
            if (f() is { } v) return v;
            Thread.Sleep(5);
        }
        throw new TimeoutException("no answer within 10 s");
    }

    private static T EventuallyValue<T>(Func<T?> f) where T : struct
    {
        var until = DateTime.UtcNow.AddSeconds(10);
        while (DateTime.UtcNow < until)
        {
            if (f() is { } v) return v;
            Thread.Sleep(5);
        }
        throw new TimeoutException("no answer within 10 s");
    }

    // ---- readers -------------------------------------------------------------------------------------------------

    [Fact]
    public void The_measured_layout_is_the_clients_reflection()
    {
        // tools/kfc types on client 26430625: LocaTagCollectionResource.languages @0x20, LocaTagCollectionResourceLanguage 40 B
        // {language @0, data @8}, LocaTagResource 24 B {id @0, text @4}
        Assert.Equal(new GameTextLayout(0x20, 40, 0, 8, 24, 0, 4), L);
        Assert.True(L.Valid);
        Assert.False((L with { TagText = 20 }).Valid);   // a string header past its row
    }

    [Fact]
    public void A_collection_lists_each_language_with_its_blob_hash()
    {
        var c = new TestBlob(40);
        var langs = c.Array(L.Languages, 3, L.LanguageStride);
        for (var i = 0; i < 3; i++)
        {
            c.I32(langs + i * L.LanguageStride + L.LanguageValue, new[] { 1, 3, 16 }[i]);
            c.Bytes(langs + i * L.LanguageStride + L.LanguageData, Enumerable.Repeat((byte)(i + 1), 16).ToArray());
        }
        var list = GameTextData.Languages(c.Done, L)!;
        Assert.Equal([1L, 3, 16], list.Select(x => x.Value));
        Assert.Equal(Enumerable.Repeat((byte)2, 16), list[1].Hash);
        var bad = c.Done;
        BinaryPrimitives.WriteUInt32LittleEndian(bad.AsSpan(L.Languages + 4), 99);  // more rows than the bytes hold
        Assert.Null(GameTextData.Languages(bad, L));
        Assert.Equal("de-de", GameTextData.LanguageKey("De_De"));
        Assert.Equal("es-419", GameTextData.LanguageKey("Es_419"));
    }

    [Fact]
    public void A_table_keeps_the_markup_drops_trailing_nuls_and_refuses_bytes_outside_the_blob()
    {
        var t = new TestBlob(8);
        var rows = t.Array(0, 4, L.TagStride);
        t.U32(rows, 7);
        t.String(rows + L.TagText, "Holzscheite");
        t.U32(rows + L.TagStride, 42);
        t.Raw(rows + L.TagStride + L.TagText, Encoding.UTF8.GetBytes("<b>Kohlenmeiler</b> \0\0"));
        t.U32(rows + 2 * L.TagStride, 0);                                     // tag 0: left out
        t.String(rows + 2 * L.TagStride + L.TagText, "ignored");
        t.U32(rows + 3 * L.TagStride, 43);                                    // an empty text stays (an answer is null for it)
        var table = GameTextData.Table(t.Done, L)!;
        Assert.Equal(3, table.Count);
        Assert.Equal("<b>Kohlenmeiler</b> ", table[42]);                     // markup kept, trailing NULs off
        Assert.Equal("", table[43]);
        Assert.Equal("Kohlenmeiler", GameTextData.Plain(table[42]));
        var bad = t.Done;
        BinaryPrimitives.WriteInt32LittleEndian(bad.AsSpan(rows + L.TagText), 100_000);
        Assert.Null(GameTextData.Table(bad, L));                              // never a table with a wrong text
        Assert.Null(GameTextData.Table(new byte[4], L));
    }

    [Fact]
    public void A_layout_whose_id_or_language_leaves_its_row_reads_nothing_and_never_throws()
    {
        // review 31.13: Valid checked the string and data fields against the stride, not the tag id or the language value
        Assert.False((L with { TagId = 22 }).Valid);
        Assert.False((L with { LanguageValue = 38 }).Valid);
        // the readers on their own: the last row's id read past the blob is no table, never an exception (Content.TextIn,
        // Content.TextLanguages and GameTextStore.Resolve read the collection on the caller's thread)
        var table = new byte[8 + 24];
        BinaryPrimitives.WriteInt32LittleEndian(table, 8);
        BinaryPrimitives.WriteUInt32LittleEndian(table.AsSpan(4), 1);
        Assert.Null(GameTextData.Table(table, L with { TagId = 22 }));
        var collection = new byte[0x20 + 8 + 40];
        BinaryPrimitives.WriteInt32LittleEndian(collection.AsSpan(0x20), 8);
        BinaryPrimitives.WriteUInt32LittleEndian(collection.AsSpan(0x24), 1);
        Assert.Null(GameTextData.Languages(collection, L with { LanguageValue = 38 }));
    }

    [Fact]
    public void A_text_longer_than_the_limit_is_left_out_and_the_others_stay()
    {
        // GameTextData.MaxTextBytes says "longer ones are left out, never cut": one long text in the blob must not take the
        // whole language away (review 31.13)
        var t = new TestBlob(8);
        var rows = t.Array(0, 2, L.TagStride);
        t.U32(rows, 7);
        t.String(rows + L.TagText, "Holzscheite");
        t.U32(rows + L.TagStride, 8);
        t.Raw(rows + L.TagStride + L.TagText, Enumerable.Repeat((byte)'a', GameTextData.MaxTextBytes + 1).ToArray());
        var table = GameTextData.Table(t.Done, L)!;
        Assert.NotNull(table);
        Assert.Equal("Holzscheite", table[7]);
        Assert.False(table.ContainsKey(8));
    }

    [Fact]
    public void The_lookup_rows_are_unavailable_with_the_reason_when_a_client_has_no_source()
    {
        // review 31.13: LookupCaps reported a source's "no texts here" (kiln.types or kiln.assets off on a client) as
        // ServiceCaps.Static(..., CapabilityReason.Unknown, ...), which ServiceCaps refuses ("Unknown is never reported")
        // whenever neither needed row is off with a reason of its own (kiln.types' row on while its types are not, no row
        // at all): the rows were left out, and Capabilities.Get answered "unknown, no row" instead of unavailable and why
        const string why = "kiln.types has no game types here";
        GameTextStore.UseSource(new ScriptedTexts { Why = why });
        VoxelMaterialData.UseSource(new ScriptedMaterials(() => null, why));
        try
        {
            LookupCaps.Rows.Ensure();
            ServiceCapRows.RefreshStarted();
            // the source's own reason, or (when another test left a needed row off in this process) that row's
            foreach (var name in new[] { Content.GameTextsCapability, Content.VoxelMaterialsCapability })
            {
                var row = Capabilities.Get(name);
                Assert.Equal(CapabilityState.Unavailable, row.State);
                Assert.NotEqual(CapabilityReason.Unknown, row.Reason);
                Assert.True(row.ReasonText == why && row.Reason == CapabilityReason.ServiceMissing || row.ReasonText.StartsWith("needs ", StringComparison.Ordinal),
                    $"{name}: {row.Reason} {row.ReasonText}");
            }
        }
        finally
        {
            VoxelMaterialData.ResetForTests();
        }
    }

    // ---- the store -------------------------------------------------------------------------------------------------

    [Fact]
    public void A_language_resolves_from_any_spelling_and_null_is_the_players()
    {
        Client();
        Assert.Equal("de-de", GameTextStore.Resolve(null));                  // GameLanguage.Current
        Assert.Equal("de-de", GameTextStore.Resolve(""));
        Assert.Equal("de-de", GameTextStore.Resolve("De_De"));
        Assert.Equal("de-de", GameTextStore.Resolve("de"));
        Assert.Equal("pt-br", GameTextStore.Resolve("pt"));                  // the first shipped language with that primary
        Assert.Equal("xx", GameTextStore.Resolve("xx"));
    }

    [Fact]
    public async Task A_table_is_read_once_in_the_background_and_the_least_used_goes_past_four()
    {
        var s = Client();
        s.Tables["fr-fr"] = new() { [7] = "Bûches" };
        s.Tables["ja-jp"] = new() { [7] = "丸太" };
        Assert.Null(GameTextStore.TryGet("de-de"));                          // not read yet: the read starts
        var de = EventuallyValue(() => GameTextStore.TryGet("de-de"));
        Assert.Equal("Holzscheite", de.Texts![7]);
        Assert.Equal(1, s.Reads);
        Assert.Equal("Holzscheite", GameTextStore.Text(7, "de-de"));
        Assert.Null(GameTextStore.Text(9, "en-us"));                         // not read yet
        Assert.Equal("<b>Chopped</b> wood.", (await GameTextStore.LoadAsync("en-us", default)).Texts![8]);
        Assert.Null(GameTextStore.Text(9, "en-us"));                         // an empty text is no text
        Assert.Equal(2, s.Reads);
        foreach (var l in new[] { "pt-br", "fr-fr", "ja-jp" }) await GameTextStore.LoadAsync(l, default);
        Assert.Equal(5, s.Reads);
        Assert.NotNull(GameTextStore.TryGet("ja-jp"));                        // kept
        Assert.Null(GameTextStore.TryGet("de-de"));                          // the least recently used of five: read again
        EventuallyValue(() => GameTextStore.TryGet("de-de"));
        Assert.Equal(6, s.Reads);
    }

    [Fact]
    public async Task A_failed_read_says_why_and_waits_before_it_is_tried_again()
    {
        var s = Client();
        Assert.Null(GameTextStore.TryGet("xx-yy"));
        var failed = EventuallyValue(() => GameTextStore.TryGet("xx-yy"));
        Assert.Null(failed.Texts);
        Assert.Equal("the game ships no texts in \"xx-yy\" (it has en-us, de-de, pt-br)", failed.Reason);
        Assert.NotNull(GameTextStore.TryGet("xx-yy"));                        // the back-off: answered, not read again
        Assert.Equal(1, s.Reads);
        var again = await GameTextStore.LoadAsync("xx-yy", default);          // an explicit load asks again
        Assert.Null(again.Texts);
        Assert.Equal(2, s.Reads);
        // a server: no read at all, the one reason
        GameTextStore.UseSource(new ScriptedTexts { Why = GameTextStore.ServerReason });
        var server = GameTextStore.TryGet("de-de")!.Value;
        Assert.Null(server.Texts);
        Assert.Equal(GameTextStore.ServerReason, server.Reason);
        Assert.Empty(GameTextStore.Languages);
    }

    [Fact]
    public async Task A_load_that_is_cancelled_throws_and_the_read_goes_on()
    {
        var s = Client();
        s.Gate = new ManualResetEventSlim(false);
        using var cts = new CancellationTokenSource();
        var load = GameTextStore.LoadAsync("de-de", cts.Token);
        cts.Cancel();
        await Assert.ThrowsAnyAsync<OperationCanceledException>(() => load);
        s.Gate.Set();
        Assert.Equal("Holzscheite", (await GameTextStore.LoadAsync("de-de", default)).Texts![7]);
        Assert.Equal(1, s.Reads);
    }

    // ---- Content ---------------------------------------------------------------------------------------------------

    [Fact]
    public async Task Content_reads_any_shipped_language_on_any_thread()
    {
        Client();
        Assert.Equal(["en-us", "de-de", "pt-br"], Content.TextLanguages);
        var de = await Content.LoadTextsAsync("de");
        Assert.True(de.Available);
        Assert.Equal(GameTextSource.Game, de.Source);
        Assert.Equal("de-de", de.Language);
        Assert.Equal(2, de.Count);
        Assert.Equal("<b>Gehacktes</b> Holz.", de.Get(8));
        Assert.Equal("Gehacktes Holz.", de.GetPlain(8));
        Assert.Null(de.Get(1234));
        Assert.Equal([7u, 8u], de.Keys.Order());
        Assert.Equal("Holzscheite", Content.TextIn(7, "de-de"));
        Assert.Equal("Holzscheite", Content.TextIn(7, null));                // the player's language
        Assert.Null(Content.TextIn(7, "en-us"));                              // not read yet: the read starts
        Assert.Equal("Wood Logs", Eventually(() => Content.TextIn(7, "en-us")));
        Assert.Equal("Wood Logs", await Task.Run(() => Content.TryGetTexts("EN_us")!.Get(7)));   // a worker thread
        var guid = Guid.Parse("4d8d0e3a-1b0f-4783-808c-1203bdd118ca");
        Assert.Null(Content.TextIn(guid, "de-de"));                          // a GUID is hashed; this one has no text
        Assert.Null(Content.TextIn(Guid.Empty, "de-de"));
        var none = await Content.LoadTextsAsync("xx-yy");
        Assert.False(none.Available);
        Assert.Equal(GameTextSource.None, none.Source);
        Assert.Empty(none);
        Assert.Contains("ships no texts in \"xx-yy\"", none.Reason);
    }

    [Fact]
    public async Task A_dedicated_server_answers_english_from_a_kiln_catalog_and_says_why_for_the_rest()
    {
        GameTextStore.UseSource(new ScriptedTexts { Why = GameTextStore.ServerReason });
        var dir = Path.Combine(Path.GetTempPath(), "kiln-gametexts-" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(dir);
        var saved = Environment.GetEnvironmentVariable("KILN_CATALOG");
        try
        {
            var file = Path.Combine(dir, "client.json");
            File.WriteAllText(file, """{"format":1,"game":{"steamBuild":"26430625","svn":"1287369","side":"client"},"sections":{"loca":[{"id":"0x00000007","text":"Wood Logs"}]}}""");
            Environment.SetEnvironmentVariable("KILN_CATALOG", file);
            Texts.Reset();
            var de = await Content.LoadTextsAsync("de-de");
            Assert.False(de.Available);
            Assert.Equal(GameTextStore.ServerReason, de.Reason);
            Assert.Null(Content.TextIn(7, "de-de"));
            var en = await Content.LoadTextsAsync("en-us");
            Assert.Equal(GameTextSource.KilnCatalog, en.Source);
            Assert.Equal("Wood Logs", en.Get(7));
            Assert.Equal("Wood Logs", Content.TextIn(7, "en"));
            Assert.Equal(["en-us"], Content.TextLanguages);
            // no catalog: English says why too
            Environment.SetEnvironmentVariable("KILN_CATALOG", Path.Combine(dir, "none.json"));
            Texts.Reset();
            var missing = await Content.LoadTextsAsync("en-us");
            Assert.False(missing.Available);
            Assert.StartsWith(GameTextStore.ServerReason + "; no Kiln catalog English either:", missing.Reason);
        }
        finally
        {
            Environment.SetEnvironmentVariable("KILN_CATALOG", saved);
            Texts.Reset();
            Directory.Delete(dir, true);
        }
    }

    // ---- Items -----------------------------------------------------------------------------------------------------

    [Fact]
    public void An_item_infos_text_tags_are_the_fnv_of_its_four_loca_guids()
    {
        Assert.Equal((0x48, 0x58, 0x68, 0x78), GameItemTexts.Measured);      // tools/kfc types, client 26430625
        var name = Guid.NewGuid();
        var description = Guid.NewGuid();
        var info = new byte[0x90];
        name.ToByteArray().CopyTo(info, 0x48);
        description.ToByteArray().CopyTo(info, 0x68);
        var tags = GameItemTexts.Parse(info, GameItemTexts.Measured);
        Assert.Equal(new ItemTextTags(Texts.TagOf(name), 0, Texts.TagOf(description), 0), tags);
        Assert.Equal(new ItemTextTags(Texts.TagOf(name), 0, 0, 0), GameItemTexts.Parse(info.AsSpan(0, 0x70), GameItemTexts.Measured));
    }

    [Fact]
    public async Task Items_answer_descriptions_and_names_in_a_chosen_language()
    {
        Client();
        var logs = new ItemId(722268629);                                     // "Wood Logs" in Kiln's shipped items.tsv
        GameNames.UseForTests(GameNameLayout.Measured, new() { [logs.Value] = Guid.NewGuid() }, new(), new(), "de-de", texts: null);
        GameItemTexts.Override = i => i == logs ? new ItemTextTags(7, 0, 8, 0) : null;
        Assert.Equal(new ItemTextTags(7, 0, 8, 0), Items.TextTags(logs));
        Assert.Null(Items.TextTags(new ItemId(5)));
        Assert.Null(Items.Description(logs));                                // de-de not read yet
        Assert.Null(Items.DisplayNameIn(logs, "pt-br"));                       // nor pt-br: no English stand-in for another language
        Assert.Equal("Wood Logs", Items.DisplayNameIn(logs, "en-us"));          // English: the shipped name meanwhile
        await Content.LoadTextsAsync("de-de");
        await Content.LoadTextsAsync("pt-br");
        Assert.Equal("Gehacktes Holz.", Items.Description(logs));            // plain: markup out
        Assert.Equal("Gehacktes Holz.", Items.DescriptionIn(logs, "de"));
        Assert.Equal("Holzscheite", Items.DisplayNameIn(logs, null));
        Assert.Equal("Toras", Items.DisplayNameIn(logs, "pt-br"));
        Assert.Null(Items.DescriptionIn(logs, "pt-br"));                        // pt-br has no text for the description tag
        Assert.Null(Items.Description(new ItemId(0)));
    }

    [Fact]
    public void A_new_items_description_is_its_definitions()
    {
        Client();
        GameNames.UseForTests(GameNameLayout.Measured, new(), new(), new(), "de-de", texts: null);
        var mine = new ItemId(0xdeadbeef);
        DefinedNames.DescriptionOverride = (id, lang) => id == mine.Value ? $"Mein Ding ({lang})" : null;
        Assert.Equal("Mein Ding (de-de)", Items.Description(mine));
        Assert.Equal("Mein Ding (fr-fr)", Items.DescriptionIn(mine, "fr-fr"));
        Assert.Null(Items.Description(new ItemId(1)));
        // a definition's descriptions: that language, its primary, "*", English; "" is no description
        var def = new ContentNames.Def(1, Guid.NewGuid(), null, null, "Glacier", new Dictionary<string, string>(), false)
        {
            Descriptions = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase) { ["de"] = "Gletscher", ["*"] = "Old ice", ["fr-fr"] = "" },
        };
        Assert.Equal("Gletscher", ContentNames.DescriptionFor(def, "de-de"));
        Assert.Equal("Old ice", ContentNames.DescriptionFor(def, "ja-jp"));
        Assert.Equal("", ContentNames.DescriptionFor(def, "fr-fr"));
        var parsed = ContentNames.Parse("""
            [{"kind":"item","guid":"7e57a11b-0000-4000-8000-000000000001","definition":{"displayName":"Glacier","description":"Old ice",
              "texts":{"de":{"name":"Gletscher","description":"Altes Eis"},"fr":{"name":"Glacier"}}}}]
            """).Values.Single();
        Assert.Equal("Altes Eis", ContentNames.DescriptionFor(parsed, "de-de"));
        Assert.Equal("Old ice", ContentNames.DescriptionFor(parsed, "fr-fr"));
        Assert.Equal("Gletscher", ContentNames.NameFor(parsed, "de-de"));
    }
}
