// SPDX-License-Identifier: MIT
// The boot content fingerprint (BootFingerprint.cs, docs/kiln/network.md "Boot content at the join"): what a boot mod's generation
// committed, hashed so a server and a player compare it at the join. The same content gives the same hash whatever the key order, the
// file order, the line endings, the number spelling or the mod's folder; a changed patch value gives another; a mod that is not a boot
// mod, not side both, or left out of the generation has none. The publication runs through BootPhase on a fake kiln.boot table, with a
// mod that reads its setting in OnBoot and stages a resource patch with it (More Flame Altars' case).
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Text.Json.Nodes;
using Kiln.Host;
using Xunit;

namespace Kiln.Host.Tests;

public sealed class PatchBootSettings
{
    [Range(1, 16)]
    public int Altars { get; set; } = 3;
}

/// <summary>A boot mod whose resource patch is a setting.</summary>
public sealed class PatchBootMod : Kiln.Mod
{
    public override void OnBoot(BootContext boot)
    {
        var altars = Config.Register<PatchBootSettings>().Value.Altars;
        var staged = BootDefinitions.For(boot).Define("altars", new ResourcePatchBlock
        {
            Resource = Guid.Parse("82706b40-61b1-4b8f-8b23-dcec6971bda1"),
            Type = "keen::BalancingTable",
            Set = { ["altarsPerFlameLevel[*]"] = ResourceValue.AtLeast(altars) },
        });
        if (!staged.Ok) throw new InvalidOperationException(staged.Reason);
    }
}

/// <summary>A boot mod whose item has a PNG icon of its folder: the boot lowering commits it as the file's full path, as a run-time define names it.</summary>
public sealed class IconBootMod : Kiln.Mod
{
    public override void OnBoot(BootContext boot)
    {
        var staged = BootDefinitions.For(boot).Define("token", new ItemBlock { Name = "Token", Visual = new VisualBlock { Icon = "icons/token.png" } });
        if (!staged.Ok) throw new InvalidOperationException(staged.Reason);
    }
}

[Collection("boot generation")]  // BootGeneration, Capabilities and Config are process-wide
public sealed class BootFingerprintTests : ModsFixture
{
    private const string Patch = """
        {
          "format": 1,
          // the altars
          "resourcePatches": {
            "more-altars": { "format": 1, "resource": "82706b40-61b1-4b8f-8b23-dcec6971bda1", "type": "keen::BalancingTable",
                             "set": { "altarsPerFlameLevel[*]": { "min": 16 }, "experienceNeedPerLevel[0]": 1500 } },
          },
        }
        """;

    // the same values: other key order, other spacing, CRLF, other number spelling, no comment, no trailing comma
    private const string PatchAgain =
        "{\r\n\"resourcePatches\": {\"more-altars\": {\"set\": {\"experienceNeedPerLevel[0]\": 1.5e3, \"altarsPerFlameLevel[*]\": {\"min\": 16.0}},\r\n" +
        "\"type\": \"keen::BalancingTable\", \"resource\": \"82706b40-61b1-4b8f-8b23-dcec6971bda1\", \"format\": 1}},\r\n\"format\": 1\r\n}\r\n";

    private static Dictionary<string, BootModState> _states = new(StringComparer.Ordinal);

    // The fake kiln.boot table lives as long as the test: the recorded generation reads its owner states through it (ReadState) after
    // the boot phase returned, until Dispose records generation 0 again.
    private readonly unsafe KilnBootApi* _table = (KilnBootApi*)NativeMemory.AllocZeroed((nuint)sizeof(KilnBootApi));

    public BootFingerprintTests() => BootGeneration.Record(0, [], new Dictionary<string, IReadOnlyList<BootStaged>>());

    public override unsafe void Dispose()
    {
        BootGeneration.Record(0, [], new Dictionary<string, IReadOnlyList<BootStaged>>());
        BootFingerprint.Publish(0, new Dictionary<string, string?>());
        NativeMemory.Free(_table);
        base.Dispose();
    }

    private string Mod(string folder, params (string File, string Text)[] files)
    {
        var dir = Directory.CreateDirectory(Path.Combine(ModsDir, folder)).FullName;
        foreach (var (file, text) in files)
        {
            Directory.CreateDirectory(Path.GetDirectoryName(Path.Combine(dir, file))!);
            File.WriteAllText(Path.Combine(dir, file), text);
        }
        return dir;
    }

    private static JsonObject Envelope(params (string Collector, string Owner, string Key, string Document)[] docs)
    {
        var c = new JsonObject();
        foreach (var (collector, owner, key, document) in docs)
            ((c[collector] ??= new JsonArray()).AsArray()).Add(new JsonObject { ["owner"] = owner, ["key"] = key, ["document"] = JsonNode.Parse(document) });
        return c;
    }

    // ------------------------------------------------------------------------------------------------ the hash

    [Theory]
    [InlineData("9007199254740992", "9007199254740992.0", true)]
    [InlineData("9007199254740992.0", "9007199254740993.0", false)]
    [InlineData("18446744073709551614.0", "18446744073709551615.0", false)]
    [InlineData("1e400", "10e399", true)]
    [InlineData("1e-400", "2e-400", false)]
    [InlineData("-0", "0.0", true)]
    public void Numeric_canonicalization_is_exact(string a, string b, bool equal)
    {
        var dir = Mod("numbers");
        string Hash(string n) => BootFingerprint.Of("m", dir, Envelope(("patches", "m", "p", "{\"set\":{\"value\":" + n + "}}")));
        Assert.Equal(equal, Hash(a) == Hash(b));
    }

    [Fact]
    public void Line_breaks_inside_committed_text_remain_significant()
    {
        var dir = Mod("text");
        string Hash(string text) => BootFingerprint.Of("m", dir, Envelope(("content", "m", "p",
            new JsonObject { ["description"] = text }.ToJsonString())));
        Assert.NotEqual(Hash("one\r\ntwo"), Hash("one\ntwo"));
    }

    [Fact]
    public void Duplicate_JSON_keys_count_the_last_value_as_the_native_reader_does()
    {
        var duplicate = Mod("duplicate", ("blocks/p.json", Patch.Replace("\"min\": 16", "\"min\": 12, \"min\": 16")));
        var plain = Mod("plain", ("blocks/p.json", Patch));
        Assert.Single(BootFingerprint.FilePatches(duplicate));
        Assert.Equal(BootFingerprint.Of("m", plain, new JsonObject()), BootFingerprint.Of("m", duplicate, new JsonObject()));
    }

    [Fact]
    public void Patches_keep_native_file_application_order()
    {
        const string add = """{"resourcePatches":{"z":{"resource":"82706b40-61b1-4b8f-8b23-dcec6971bda1","type":"keen::BalancingTable","set":{"experienceNeedPerLevel[0]":{"add":2}}}}}""";
        const string mul = """{"resourcePatches":{"a":{"resource":"82706b40-61b1-4b8f-8b23-dcec6971bda1","type":"keen::BalancingTable","set":{"experienceNeedPerLevel[0]":{"mul":3}}}}}""";
        var first = Mod("add-first", ("blocks/1.json", add), ("blocks/2.json", mul));
        var second = Mod("mul-first", ("blocks/1.json", mul), ("blocks/2.json", add));
        Assert.NotEqual(BootFingerprint.Of("m", first, new JsonObject()), BootFingerprint.Of("m", second, new JsonObject()));
    }

    [Fact]
    public void The_same_patches_give_the_same_hash_whatever_the_order_spacing_line_endings_and_number_spelling()
    {
        var server = Mod("server/more-altars", ("blocks/a-other.json", """{ "statuses": {} }"""), ("blocks/patches.json", Patch));
        var client = Mod("client/more-altars", ("blocks/patches.json", PatchAgain), ("blocks/a-other.json", "{\"statuses\":{}}"));
        var a = BootFingerprint.Of("more-altars", server, new JsonObject());
        Assert.Matches("^[0-9a-f]{64}$", a);
        Assert.Equal(a, BootFingerprint.Of("more-altars", client, new JsonObject()));
    }

    [Fact]
    public void A_changed_patch_value_gives_another_hash()
    {
        var a = BootFingerprint.Of("more-altars", Mod("a", ("blocks/patches.json", Patch)), new JsonObject());
        var b = BootFingerprint.Of("more-altars", Mod("b", ("blocks/patches.json", Patch.Replace("\"min\": 16", "\"min\": 12"))), new JsonObject());
        Assert.NotEqual(a, b);
        var staged = Envelope(("patches", "m", "altars", """{"resource":"82706b40-61b1-4b8f-8b23-dcec6971bda1","type":"keen::BalancingTable","set":{"x":{"min":4}}}"""));
        var other = Envelope(("patches", "m", "altars", """{"resource":"82706b40-61b1-4b8f-8b23-dcec6971bda1","type":"keen::BalancingTable","set":{"x":{"min":5}}}"""));
        var dir = Mod("c");
        Assert.NotEqual(BootFingerprint.Of("m", dir, staged), BootFingerprint.Of("m", dir, other));
    }

    [Fact]
    public void Envelope_documents_count_by_collector_and_key_not_by_their_order_and_other_owners_do_not_count()
    {
        var dir = Mod("m");
        var one = Envelope(("content", "m", "token#1", """{"op":"define","kind":"items","definition":{"name":"Token","guid":"x"}}"""),
            ("assets", "m", "chill#1", """{"entries":[{"kind":"resource","data":"AAEC"}]}"""));
        var two = Envelope(("assets", "m", "chill#1", """{"entries":[{"data":"AAEC","kind":"resource"}]}"""),
            ("content", "other-mod", "x#1", """{"op":"define"}"""),
            ("content", "m", "token#1", """{"definition":{"guid":"x","name":"Token"},"kind":"items","op":"define"}"""));
        Assert.Equal(BootFingerprint.Of("m", dir, one), BootFingerprint.Of("m", dir, two));
        Assert.NotEqual(BootFingerprint.Of("m", dir, one), BootFingerprint.Of("m", dir, new JsonObject()));
    }

    [Fact]
    public void A_key_the_blocks_file_and_OnBoot_both_have_counts_as_the_files()
    {
        var dir = Mod("m", ("blocks/patches.json", Patch));
        var staged = Envelope(("patches", "m", "more-altars", """{"resource":"82706b40-61b1-4b8f-8b23-dcec6971bda1","type":"t","set":{"x":1}}"""));
        Assert.Equal(BootFingerprint.Of("m", dir, new JsonObject()), BootFingerprint.Of("m", dir, staged));  // kiln.content keeps the file's
    }

    [Fact]
    public void A_path_inside_the_mods_own_folder_counts_by_its_place_in_the_folder()
    {
        var server = Mod("server-root/frost");
        var client = Mod("client-root/somewhere/frost");
        string Icon(string dir) => JsonValue.Create(Path.Combine(dir, "icons", "token.png"))!.ToJsonString();
        var a = BootFingerprint.Of("frost", server, Envelope(("content", "frost", "token#1", "{\"definition\":{\"icon\":" + Icon(server) + "}}")));
        var b = BootFingerprint.Of("frost", client, Envelope(("content", "frost", "token#1", "{\"definition\":{\"icon\":" + Icon(client) + "}}")));
        Assert.Equal(a, b);
        var c = BootFingerprint.Of("frost", client, Envelope(("content", "frost", "token#1", "{\"definition\":{\"icon\":" + Icon(Path.Combine(client, "other")) + "}}")));
        Assert.NotEqual(a, c);
    }

    [Fact]
    public void A_mod_folder_reached_through_a_link_counts_its_resolved_paths_as_its_own()
    {
        // Kiln.Blocks names a mod's file by its link-resolved path (BlockFiles.InMod): a Steam library behind a link gives another
        // spelling of the same folder
        var real = Mod("library/frost");
        var link = Path.Combine(ModsDir, "frost-link");
        Directory.CreateSymbolicLink(link, real);
        var viaLink = BootFingerprint.Of("frost", link, Envelope(("content", "frost", "token#1",
            "{\"definition\":{\"icon\":" + JsonValue.Create(Path.Combine(real, "icon.png"))!.ToJsonString() + "}}")));
        var plain = Mod("elsewhere/frost");
        var direct = BootFingerprint.Of("frost", plain, Envelope(("content", "frost", "token#1",
            "{\"definition\":{\"icon\":" + JsonValue.Create(Path.Combine(plain, "icon.png"))!.ToJsonString() + "}}")));
        Assert.Equal(direct, viaLink);
    }

    [Fact]
    public void A_link_in_the_mod_folders_parent_and_a_trailing_separator_do_not_change_the_fingerprint()
    {
        var real = Mod("library-parent/frost");
        var parentLink = Path.Combine(ModsDir, "linked-parent");
        Directory.CreateSymbolicLink(parentLink, Path.GetDirectoryName(real)!);
        var via = Path.Combine(parentLink, "frost") + Path.DirectorySeparatorChar;
        JsonObject Docs(string icon) => Envelope(("skills", "frost", "talent#1",
            new JsonObject { ["iconFile"] = icon }.ToJsonString()));
        Assert.Equal(BootFingerprint.Of("frost", real, Docs(Path.Combine(real, "icons", "token.png"))),
            BootFingerprint.Of("frost", via, Docs(Path.Combine(real, "icons", "token.png"))));
    }

    [Fact]
    public void Native_windows_and_Wine_drive_folders_count_the_same_icon()
    {
        (string, string, JsonNode?)[] Entry(string path) => [("content", "token#1", new JsonObject { ["icon"] = path })];
        Assert.Equal(BootFingerprint.Hash(Entry(@"C:\Games\frost\icons\token.png"), [BootFingerprint.NormalizeRoot(@"c:\games\frost\")]),
            BootFingerprint.Hash(Entry(@"Z:\home\player\mods\frost/icons/token.png"), [BootFingerprint.NormalizeRoot(@"Z:\home\player\mods\frost")]));
    }

    [Theory]
    [InlineData(@"C:\Games\Enshrouded\kiln\mods\frost\icons\token.png", @"C:\Games\Enshrouded\kiln\mods\frost", "$mod/icons/token.png")]
    [InlineData(@"\\?\C:\Games\Enshrouded\kiln\mods\frost\icons\token.png", @"C:\Games\Enshrouded\kiln\mods\frost\", "$mod/icons/token.png")]
    [InlineData(@"c:\games\enshrouded\KILN\mods\frost/icons/token.png", @"\\?\C:\Games\Enshrouded\kiln\mods\frost", "$mod/icons/token.png")]
    [InlineData(@"Z:\home\player\.steam\Enshrouded\kiln\mods\frost\a.png", @"Z:\home\player\.steam\Enshrouded\kiln\mods\frost", "$mod/a.png")]
    [InlineData(@"C:\Games\Enshrouded\kiln\mods\frosty\a.png", @"C:\Games\Enshrouded\kiln\mods\frost", @"C:\Games\Enshrouded\kiln\mods\frosty\a.png")]
    [InlineData("line one\r\nline two", @"C:\x", "line one\r\nline two")]
    [InlineData("item:Torch", @"C:\x", "item:Torch")]
    public void Windows_paths_and_line_endings_are_counted_the_same_on_every_machine(string value, string modFolder, string counted) =>
        Assert.Equal(counted, BootFingerprint.Text(value, BootFingerprint.NormalizeRoot(modFolder)));

    [Theory]
    [InlineData(@"\\?\UNC\server\library\frost\icon.png", @"\\server\library\frost")]
    [InlineData(@"\\server\library\frost\icon.png", @"\\?\UNC\server\library\frost")]
    public void Extended_UNC_paths_match_the_mod_root(string value, string root) =>
        Assert.Equal("$mod/icon.png", BootFingerprint.Text(value, BootFingerprint.NormalizeRoot(root)));

    [Fact]
    public void Culture_and_array_order_have_their_expected_effects()
    {
        var dir = Mod("culture");
        var original = System.Globalization.CultureInfo.CurrentCulture;
        try
        {
            string Hash(string values) => BootFingerprint.Of("m", dir, Envelope(("assets", "m", "I", values)));
            System.Globalization.CultureInfo.CurrentCulture = System.Globalization.CultureInfo.GetCultureInfo("en-US");
            var first = Hash("[1.25,1500,\"I\",\"ı\"]");
            System.Globalization.CultureInfo.CurrentCulture = System.Globalization.CultureInfo.GetCultureInfo("tr-TR");
            Assert.Equal(first, Hash("[1.25,1.5e3,\"I\",\"ı\"]"));
            Assert.NotEqual(first, Hash("[1500,1.25,\"I\",\"ı\"]"));
        }
        finally { System.Globalization.CultureInfo.CurrentCulture = original; }
    }

    [Theory]
    [InlineData("1500", "1500")]
    [InlineData("1500.0", "1500")]
    [InlineData("1.5e3", "1500")]
    [InlineData("1.5E+3", "1500")]
    [InlineData("0.25", "0.25")]
    [InlineData("-2.50", "-2.5")]
    [InlineData("12345678901234567890", "12345678901234567890")]
    public void Numbers_count_by_their_value(string raw, string counted) => Assert.Equal(counted, BootFingerprint.Number(raw));

    [Fact]
    public void A_file_that_is_not_json_or_names_no_patches_adds_nothing()
    {
        var dir = Mod("m", ("blocks/broken.json", "{ \"resourcePatches\": { "), ("blocks/plain.json", """{ "statuses": { "chill": {} } }"""),
            ("blocks/notes.txt", Patch), ("blocks/upper.JSON", Patch));
        Assert.Empty(BootFingerprint.FilePatches(dir));
        Assert.Equal(BootFingerprint.Of("m", Mod("empty"), new JsonObject()), BootFingerprint.Of("m", dir, new JsonObject()));
    }

    [Fact]
    public void A_blocks_files_baseRules_are_boot_content_keyed_as_a_staged_rule()
    {
        // kiln.content reads a blocks file's "baseRules" in prepare (gamedata_bootpatch.cpp file_docs), keyed "baseRules@<name>" as a staged
        // BaseRulesBlock is: the file's wins a name both have, and a changed value changes the hash
        const string rules = """{ "format": 1, "baseRules": { "roomy": { "minimumAltars": 6, }, }, }""";
        var dir = Mod("bases", ("blocks/bases.json", rules));
        Assert.Equal("baseRules@roomy", Assert.Single(BootFingerprint.FilePatches(dir)).Key);
        var changed = Mod("bases-changed", ("blocks/bases.json", rules.Replace("6", "7")));
        Assert.NotEqual(BootFingerprint.Of("m", dir, new JsonObject()), BootFingerprint.Of("m", changed, new JsonObject()));
        var staged = new JsonObject
        {
            ["patches"] = new JsonArray(new JsonObject { ["owner"] = "m", ["key"] = "baseRules@roomy", ["document"] = new JsonObject { ["minimumAltars"] = 2 } }),
        };
        Assert.Equal(BootFingerprint.Of("m", dir, new JsonObject()), BootFingerprint.Of("m", dir, staged));
    }

    [Fact]
    public void A_data_packs_gearRules_are_boot_content_and_change_the_hash()
    {
        // kiln.content's prepare lowers content/*.json's "gearRules" to a BalancingTable patch (gamedata_bootpatch.cpp gear_rule_docs):
        // files by name, any case of .json, comments allowed, trailing commas not (nlohmann::json)
        const string rules = """
            { // boot values for gear
              "format": 1, "gear": { "Weapon_*": { "power": { "mul": 2 } } }, "gearRules": { "rarityDurability": [1, 1, 1, 1.25, 1.5] } }
            """;
        var dir = Mod("gear", ("content/Gear.JSON", rules), ("content/items.json", """{ "items": { "Material_T1_Wood_Logs": { "maxStackSize": 1000 } } }"""),
            ("content/trailing.json", """{ "gearRules": { "socketChance": [0, 0, 0, 0, 1], }, }"""));
        var patches = BootFingerprint.FilePatches(dir);
        Assert.Equal("gearRules@Gear.JSON", Assert.Single(patches).Key);
        var changed = Mod("gear-changed", ("content/Gear.JSON", rules.Replace("1.5", "2")));
        Assert.NotEqual(BootFingerprint.Of("m", dir, new JsonObject()), BootFingerprint.Of("m", changed, new JsonObject()));
        var gearOnly = Mod("gear-only", ("content/Gear.JSON", rules.Replace("\"power\": { \"mul\": 2 }", "\"power\": 40")));
        Assert.Equal(BootFingerprint.Of("m", dir, new JsonObject()), BootFingerprint.Of("m", gearOnly, new JsonObject()));  // "gear" is world content, not boot
        Assert.Empty(BootFingerprint.FilePatches(Mod("no-rules", ("content/items.json", """{ "items": {} }"""))));
    }

    [Fact]
    public void A_lab_switch_writes_the_hashed_text_and_without_it_nothing_is_written()
    {
        var root = KilnPaths.Root;
        KilnPaths.Root = Root;
        try
        {
            var dir = Mod("m", ("blocks/patches.json", Patch));
            var dump = Path.Combine(Root, "state", "boot-fingerprints", "more-altars.txt");
            BootFingerprint.Of("more-altars", dir, new JsonObject());
            Assert.False(File.Exists(dump));  // off unless a lab asks
            Directory.CreateDirectory(Path.Combine(Root, "control"));
            File.WriteAllText(Path.Combine(Root, "control", BootFingerprint.DumpSwitch), "");
            var hash = BootFingerprint.Of("more-altars", dir, new JsonObject());
            Assert.True(File.Exists(dump));
            Assert.Equal(hash, Convert.ToHexStringLower(System.Security.Cryptography.SHA256.HashData(File.ReadAllBytes(dump))));
            Assert.Contains("experienceNeedPerLevel[0]", File.ReadAllText(dump));
        }
        finally { KilnPaths.Root = root; }
    }

    // ------------------------------------------------------------------------------------------------ the publication

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static unsafe int Collecting(KilnBootCommitResultV1* result)
    {
        result->Status = BootPhase.StatusCollecting;
        result->Generation = 1;
        return KilnAbi.Ok;
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static unsafe int Begin(ulong owner, KilnStr id, uint roles, ulong* generation) { *generation = 1; return KilnAbi.Ok; }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int Finish(ulong owner, ulong generation, uint accept) => KilnAbi.Ok;

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static unsafe int Accept(ulong owner, ulong gen, KilnStr kind, KilnStr key, KilnStr json, KilnBootStageResultV1* result)
    { result->Ok = 1; return KilnAbi.Ok; }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static unsafe int Publish(ulong owner, ulong gen, KilnBootCommitResultV1* result)
    { result->Status = BootPhase.StatusPublished; result->Generation = 1; return KilnAbi.Ok; }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static unsafe int OwnerState(KilnStr id, KilnBootOwnerStateV1* result)
    { result->State = (uint)_states.GetValueOrDefault(id.ToString(), BootModState.Committed); return KilnAbi.Ok; }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static unsafe int NoIssues(ulong gen, KilnBootIssueV1* issues, uint capacity, uint stride, uint* required)
    { *required = 0; return KilnAbi.Ok; }

    /// <summary>One start's boot phase over the mods in kiln/mods on the fake table; then Kiln.Network's view.</summary>
    private unsafe IReadOnlyDictionary<string, string> Start(FakeHost host)
    {
        using var mods = NewManager(host);
        *_table = new KilnBootApi { Size = (uint)sizeof(KilnBootApi), Version = 1, Status = &Collecting, BeginOwner = &Begin, FinishOwner = &Finish,
            Stage = &Accept, Commit = &Publish, OwnerState = &OwnerState, Issues = &NoIssues };
        var address = (nint)_table;
        BootPhase.Run((_, _) => address, 1, mods, host.Log);
        return BootFingerprint.Current();
    }

    private const string PatchMod = """
        { "id": "more-altars", "name": "More Altars", "version": "1.0.0", "apiVersion": "0.2", "side": "both", "boot": true,
          "entryAssembly": "Kiln.Host.Tests.dll", "entryType": "Kiln.Host.Tests.PatchBootMod" }
        """;

    [Fact]
    public void A_setting_read_in_OnBoot_gives_another_fingerprint_and_the_same_setting_the_same()
    {
        var root = KilnPaths.Root;
        KilnPaths.Root = Root;
        KilnPaths.Config = null!;
        _states = new(StringComparer.Ordinal);
        Capabilities.Report("mods.boot", CapabilityState.Available, CapabilityReason.None);
        try
        {
            var dir = WriteManifest("more-altars", PatchMod);
            File.Copy(Path.Combine(AppContext.BaseDirectory, "Kiln.Host.Tests.dll"), Path.Combine(dir, "Kiln.Host.Tests.dll"));
            var config = Path.Combine(Root, "config", "more-altars.json");
            Directory.CreateDirectory(Path.GetDirectoryName(config)!);
            string Run(int altars)
            {
                Config.Reset();
                File.WriteAllText(config, $"{{ \"altars\": {altars} }}\n");
                var host = new FakeHost();
                var current = Start(host);
                Assert.True(current.TryGetValue("more-altars", out var fp), host.Dump());
                Assert.True(host.Has("boot: more-altars: boot content " + fp![..16]), host.Dump());
                return fp;
            }
            var seven = Run(7);
            Assert.Matches("^[0-9a-f]{64}$", seven);
            Assert.Equal(seven, Run(7));
            Assert.NotEqual(seven, Run(9));
        }
        finally
        {
            Config.Reset();
            Capabilities.Reset();
            KilnPaths.Root = root;
            KilnPaths.Config = null!;
        }
    }

    private const string IconMod = """
        { "id": "icon-boot", "name": "Icon Boot", "version": "1.0.0", "apiVersion": "0.2", "side": "both", "clients": "mod", "boot": true,
          "entryAssembly": "Kiln.Host.Tests.dll", "entryType": "Kiln.Host.Tests.IconBootMod" }
        """;

    [Fact]
    public void A_committed_icons_full_path_counts_as_its_place_in_the_mods_folder_in_another_folder_and_through_a_link()
    {
        // two machines: the mod in kiln/mods, and the mod in a library elsewhere that kiln/mods reaches through a link. The boot lowering
        // names the icon by its full, link-resolved path (BlockFiles.InMod), which differs; what is hashed does not (the lab switch's dump)
        var root = KilnPaths.Root;
        KilnPaths.Root = Root;
        KilnPaths.Config = null!;
        _states = new(StringComparer.Ordinal);
        Capabilities.Report("mods.boot", CapabilityState.Available, CapabilityReason.None);
        Directory.CreateDirectory(KilnPaths.Control);
        File.WriteAllText(Path.Combine(KilnPaths.Control, BootFingerprint.DumpSwitch), "");
        string Install(string dir)
        {
            Directory.CreateDirectory(Path.Combine(dir, "icons"));
            File.WriteAllText(Path.Combine(dir, Manifest.FileName), IconMod);
            File.Copy(Path.Combine(AppContext.BaseDirectory, "Kiln.Host.Tests.dll"), Path.Combine(dir, "Kiln.Host.Tests.dll"));
            File.WriteAllBytes(Path.Combine(dir, "icons", "token.png"), [0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A]);
            return dir;
        }
        (string Fingerprint, string Hashed) Run()
        {
            var host = new FakeHost();
            var current = Start(host);
            Assert.True(current.TryGetValue("icon-boot", out var fp), host.Dump());
            Assert.Matches("^[0-9a-f]{64}$", fp);
            return (fp!, File.ReadAllText(Path.Combine(KilnPaths.State, "boot-fingerprints", "icon-boot.txt")));
        }
        try
        {
            var plain = Install(Path.Combine(ModsDir, "icon-boot"));
            var (a, hashedA) = Run();
            Directory.Delete(plain, recursive: true);
            var library = Install(Path.Combine(Root, "library", "icon-boot"));
            Directory.CreateSymbolicLink(Path.Combine(ModsDir, "icon-boot"), library);
            var (b, hashedB) = Run();
            Assert.Contains("\"icon\":\"$mod/icons/token.png\"", hashedA);
            Assert.DoesNotContain(BootFingerprint.NormalizeRoot(Root), hashedA, StringComparison.OrdinalIgnoreCase);
            Assert.Equal(hashedA, hashedB);
            Assert.Equal(a, b);
        }
        finally
        {
            Config.Reset();
            Capabilities.Reset();
            KilnPaths.Root = root;
            KilnPaths.Config = null!;
        }
    }

    [Fact]
    public void Only_side_both_boot_mods_of_the_generation_have_one_and_a_mod_left_out_is_out()
    {
        WriteManifest("plain", """{ "id": "plain", "name": "p", "version": "1.0.0", "apiVersion": "0.2", "side": "both" }""");
        WriteManifest("server-boot", """{ "id": "server-boot", "name": "s", "version": "1.0.0", "apiVersion": "0.2", "side": "server", "boot": true }""");
        WriteManifest("data-boot", """{ "id": "data-boot", "name": "d", "version": "1.0.0", "apiVersion": "0.2", "side": "both", "boot": true }""");
        WriteManifest("lonely", """{ "id": "lonely", "name": "l", "version": "1.0.0", "apiVersion": "0.2", "side": "both", "boot": true, "dependencies": { "helper": ">=1.0.0" } }""");
        Mod("data-boot", ("blocks/patches.json", Patch));
        _states = new(StringComparer.Ordinal) { ["lonely"] = BootModState.ExcludedDependency };
        Capabilities.Report("mods.boot", CapabilityState.Available, CapabilityReason.None);
        try
        {
            var host = new FakeHost();
            var current = Start(host);
            Assert.Equal(["data-boot", "lonely"], current.Keys.Order(StringComparer.Ordinal));
            Assert.Equal(BootFingerprint.Of("data-boot", Path.Combine(ModsDir, "data-boot"), new JsonObject()), current["data-boot"]);
            Assert.Equal(BootFingerprint.Out, current["lonely"]);

            _states["data-boot"] = BootModState.ActivationFault;  // its content faulted after the publication: no longer compared as committed
            Assert.Equal(BootFingerprint.Out, BootFingerprint.Current()["data-boot"]);

            BootGeneration.Record(0, [], new Dictionary<string, IReadOnlyList<BootStaged>>());  // another generation: these are not its
            Assert.Empty(BootFingerprint.Current());
        }
        finally { Capabilities.Reset(); }
    }

    [Fact]
    public void Without_a_boot_phase_nothing_is_published()
    {
        WriteManifest("data-boot", """{ "id": "data-boot", "name": "d", "version": "1.0.0", "apiVersion": "0.2", "side": "both", "boot": true }""");
        using var mods = NewManager(new FakeHost());
        BootPhase.Run((_, _) => 0, 1, mods, (_, _) => { });  // no kiln.boot: not a boot profile
        Assert.Empty(BootFingerprint.Current());
    }
}
