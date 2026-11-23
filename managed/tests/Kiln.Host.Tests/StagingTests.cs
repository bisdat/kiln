// SPDX-License-Identifier: MIT
using System.Runtime.InteropServices;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json.Nodes;
using Kiln.Admin;
using Xunit;

namespace Kiln.Host.Tests;

/// <summary>Kiln.Admin's side of staged updates (mods/Kiln.Admin/Staging.cs, linked here as it is): update_stage of a mod and of
/// a Kiln bundle, update_cancel, update_rollback and update_list, in the formats native/core/src/staged.cpp applies at the
/// next start. Sources come from kiln/staged/incoming only and are copied; kiln/data is never read or written.</summary>
public sealed class StagingTests : IDisposable
{
    private readonly string _game = Path.Combine(Path.GetTempPath(), "kiln-staging-" + Guid.NewGuid().ToString("N"));
    private string Kiln => Path.Combine(_game, "kiln");
    private string Staged => Path.Combine(Kiln, "staged");
    private string Incoming => Path.Combine(Staged, "incoming");

    public StagingTests()
    {
        Directory.CreateDirectory(Path.Combine(Kiln, "mods"));
        // persistent state no staged change may touch (dev.31: kiln/data/registry)
        Put(Path.Combine(Kiln, "data", "kiln-network", "server-id"), "srv-4b8e2c\n");
        Put(Path.Combine(Kiln, "data", "registry", "entry-compat.json"), "{\"format\":1}");
    }

    public void Dispose()
    {
        try { Directory.Delete(_game, true); } catch (IOException) { }
    }

    private static void Put(string file, string text)
    {
        Directory.CreateDirectory(Path.GetDirectoryName(file)!);
        File.WriteAllText(file, text);
    }

    private static string Sha(string text) => Convert.ToHexStringLower(SHA256.HashData(Encoding.UTF8.GetBytes(text)));

    private Dictionary<string, string> Tree(string dir) =>
        Directory.Exists(dir)
            ? Directory.EnumerateFiles(dir, "*", SearchOption.AllDirectories).ToDictionary(f => Path.GetRelativePath(dir, f).Replace('\\', '/'), File.ReadAllText)
            : [];

    private Dictionary<string, string> DataBefore() => Tree(Path.Combine(Kiln, "data"));
    private void DataSame(Dictionary<string, string> before) => Assert.Equal(before, Tree(Path.Combine(Kiln, "data")));

    private static Dictionary<string, string> ModFiles(string id, string version) => new()
    {
        ["kiln.mod.json"] = $$"""{"id": "{{id}}", "version": "{{version}}", "apiVersion": "0.2", "entryAssembly": "Mod.dll"}""",
        ["Mod.dll"] = $"dll {id} {version}",
        ["data/a.json"] = $"{{\"v\":\"{version}\"}}",
    };

    private string Source(string name, Dictionary<string, string> files)
    {
        var dir = Path.Combine(Incoming, name);
        foreach (var (rel, text) in files) Put(Path.Combine(dir, rel), text);
        return dir;
    }

    private JsonObject Read(string file) => (JsonObject)JsonNode.Parse(File.ReadAllText(file))!;

    [Fact]
    public void Path_rules_answer_the_shared_fixture_as_the_native_side_does()
    {
        var f = Read(Path.Combine(AppContext.BaseDirectory, "fixtures", "staged-paths.json"));
        var n = 0;
        foreach (var kind in new[] { "rel", "kiln", "folder" })
            foreach (var verdict in new[] { "ok", "bad" })
                foreach (var c in (JsonArray)f[kind]![verdict]!)
                {
                    var path = c!.GetValue<string>();
                    var got = kind switch { "rel" => Staging.RelPathOk(path, out _), "kiln" => Staging.KilnPathOk(path, out _), _ => Staging.ModFolderOk(path) };
                    Assert.True(got == (verdict == "ok"), $"{kind} {verdict}: {path}");
                    n++;
                }
        Assert.True(n > 60);
    }

    [Fact]
    public void A_mod_is_staged_as_kiln_core_reads_it()
    {
        var data = DataBefore();
        foreach (var (rel, text) in ModFiles("m", "1.0.0")) Put(Path.Combine(Kiln, "mods", "mods-m-folder", rel), text);  // id m lives here
        var src = Source("m-2", ModFiles("m", "2.0.0"));
        var expect = ModFiles("m", "2.0.0").ToDictionary(kv => kv.Key, kv => Sha(kv.Value));
        var r = Staging.StageMod(Kiln, "m-2", "m", null, expect, "nexus 1 file 2", "kiln-webadmin: alice");
        Assert.Equal("mod.mods-m-folder", r["unit"]!.GetValue<string>());
        var dir = Path.Combine(Staged, "mods", "mods-m-folder");
        var doc = Read(Path.Combine(dir, "staged.json"));
        Assert.Equal(1, doc["format"]!.GetValue<int>());
        Assert.Equal("mod", doc["kind"]!.GetValue<string>());
        Assert.Equal("mods-m-folder", doc["folder"]!.GetValue<string>());
        Assert.Equal("m", doc["id"]!.GetValue<string>());
        Assert.Equal("2.0.0", doc["version"]!.GetValue<string>());
        Assert.Equal("1.0.0", doc["from"]!.GetValue<string>());
        Assert.Equal("kiln-webadmin: alice", doc["by"]!.GetValue<string>());
        var files = ((JsonArray)doc["files"]!).Select(x => (Path: x!["path"]!.GetValue<string>(), Sha: x["sha256"]!.GetValue<string>(), Size: x["size"]!.GetValue<long>())).ToList();
        Assert.Equal(ModFiles("m", "2.0.0").Keys.Order(StringComparer.Ordinal), files.Select(f => f.Path));
        foreach (var f in files)
        {
            Assert.Equal(Sha(ModFiles("m", "2.0.0")[f.Path]), f.Sha);
            Assert.Equal(Encoding.UTF8.GetByteCount(ModFiles("m", "2.0.0")[f.Path]), f.Size);
        }
        Assert.Equal(ModFiles("m", "2.0.0"), Tree(Path.Combine(dir, "files")));
        Assert.Equal(ModFiles("m", "2.0.0"), Tree(src));  // copied, not moved
        Assert.Empty(Directory.EnumerateDirectories(Staged, ".stage-*"));
        DataSame(data);

        // listed; a second staging of the same folder is refused until it is canceled
        var unit = ((JsonArray)Staging.List(Kiln)["units"]!).Single()!;
        Assert.Equal("mod.mods-m-folder", unit["unit"]!.GetValue<string>());
        Assert.Equal("1.0.0", unit["current"]!["version"]!.GetValue<string>());
        Assert.Equal("2.0.0", unit["staged"]!["version"]!.GetValue<string>());
        Assert.Equal(3, unit["staged"]!["files"]!.GetValue<int>());
        Assert.False(unit["open"]!.GetValue<bool>());
        var again = Assert.Throws<StagingError>(() => Staging.StageMod(Kiln, "m-2", null, null, null, null, "t"));
        Assert.Contains("is staged already; cancel it first", again.Message, StringComparison.Ordinal);
        Assert.Equal(new[] { "update" }, Staging.Cancel(Kiln, "mod.mods-m-folder")["canceled"]!.AsArray().Select(x => x!.GetValue<string>()));
        Assert.False(Directory.Exists(dir));
        Assert.Throws<StagingError>(() => Staging.Cancel(Kiln, "mod.mods-m-folder"));
        DataSame(data);
    }

    [Fact]
    public void Mod_refusals_leave_nothing_staged()
    {
        var data = DataBefore();
        void Refused(string what, Action stage)
        {
            var e = Assert.Throws<StagingError>(stage);
            Assert.Contains(what, e.Message, StringComparison.Ordinal);
            Assert.False(Directory.Exists(Path.Combine(Staged, "mods")) && Directory.EnumerateDirectories(Path.Combine(Staged, "mods")).Any(), what);
            Assert.Empty(Directory.Exists(Staged) ? Directory.EnumerateDirectories(Staged, ".stage-*") : []);
        }
        Source("ok", ModFiles("m", "2.0.0"));
        // sources only from kiln/staged/incoming: never kiln/data, never a path outside
        foreach (var (rel, text) in ModFiles("m", "2.0.0")) Put(Path.Combine(Kiln, "data", "kiln-webadmin", "dl", rel), text);
        data = DataBefore();
        Refused("must be a folder under", () => Staging.StageMod(Kiln, Path.Combine(Kiln, "data", "kiln-webadmin", "dl"), null, null, null, null, "t"));
        Refused("must be a folder under", () => Staging.StageMod(Kiln, "../../data/kiln-webadmin/dl", null, null, null, null, "t"));
        Refused("must be a folder under", () => Staging.StageMod(Kiln, Incoming, null, null, null, null, "t"));
        Refused("does not exist", () => Staging.StageMod(Kiln, "nope", null, null, null, null, "t"));
        Refused("source required", () => Staging.StageMod(Kiln, "", null, null, null, null, "t"));
        Refused("no readable kiln.mod.json", () => Staging.StageMod(Kiln, Source("nomanifest", new() { ["a.dll"] = "x" }), null, null, null, null, "t"));
        Refused("kiln.mod.json says id m, not other", () => Staging.StageMod(Kiln, "ok", "other", null, null, null, "t"));
        Refused("cannot be a folder under kiln/mods", () => Staging.StageMod(Kiln, "ok", null, "../escape", null, null, "t"));
        Refused("cannot be a folder under kiln/mods", () => Staging.StageMod(Kiln, "ok", null, ".stage-x", null, null, "t"));
        var expect = ModFiles("m", "2.0.0").ToDictionary(kv => kv.Key, kv => Sha(kv.Value));
        expect["Mod.dll"] = Sha("something else");
        Refused("files: Mod.dll is", () => Staging.StageMod(Kiln, "ok", null, null, expect, null, "t"));
        expect = ModFiles("m", "2.0.0").ToDictionary(kv => kv.Key, kv => Sha(kv.Value));
        expect["extra.dll"] = Sha("x");
        Refused("files: extra.dll is not in the source", () => Staging.StageMod(Kiln, "ok", null, null, expect, null, "t"));
        expect.Remove("extra.dll");
        expect.Remove("data/a.json");
        Refused("which the list does not name", () => Staging.StageMod(Kiln, "ok", null, null, expect, null, "t"));
        if (!OperatingSystem.IsWindows())
        {
            var bad = Source("badname", ModFiles("m", "2.0.0"));
            Put(Path.Combine(bad, "a:b.dll"), "x");
            Refused("the character :", () => Staging.StageMod(Kiln, "badname", null, null, null, null, "t"));
            var linked = Source("linked", ModFiles("m", "2.0.0"));
            File.CreateSymbolicLink(Path.Combine(linked, "secret"), Path.Combine(Kiln, "data", "kiln-network", "server-id"));
            Refused("the source holds a link: secret", () => Staging.StageMod(Kiln, "linked", null, null, null, null, "t"));
        }
        // part of Kiln (its receipt lists the folder): updated with Kiln, never on its own
        Put(Path.Combine(Kiln, ".install.json"), """{"format":1,"side":"server","runtime":{"version":"1"},"files":[{"path":"kiln/mods/kiln-admin/Kiln.Admin.dll","sha256":"00"}]}""");
        Source("admin", ModFiles("kiln-admin", "9.9.9"));
        Refused("is part of Kiln", () => Staging.StageMod(Kiln, "admin", null, null, null, null, "t"));
        // a rollback request waits: cancel it first
        Put(Path.Combine(Staged, "rollback", "mod.m.json"), "{}");
        Refused("a rollback of mod folder m is requested", () => Staging.StageMod(Kiln, "ok", null, null, null, null, "t"));
        DataSame(data);
    }

    [Fact]
    public void A_rollback_needs_a_kept_version_and_no_staged_update()
    {
        var data = DataBefore();
        Assert.Contains("no earlier version", Assert.Throws<StagingError>(() => Staging.Rollback(Kiln, "mod.m", "t")).Message, StringComparison.Ordinal);
        Assert.Throws<StagingError>(() => Staging.Rollback(Kiln, "mod.../x", "t"));
        Assert.Throws<StagingError>(() => Staging.Rollback(Kiln, "registry", "t"));
        Put(Path.Combine(Staged, "previous", "mods", "m", "kept.json"),
            """{"format":1,"unit":"mod.m","version":"1.0.0","replacedBy":"2.0.0","kept":"2026-11-23T10:00:00.000Z","reason":"update","entries":[{"path":"kiln/mods/m","before":"present"}]}""");
        var r = Staging.Rollback(Kiln, "mod.m", "kiln-webadmin: bob");
        Assert.Equal("1.0.0", r["to"]!.GetValue<string>());
        Assert.Equal("2.0.0", r["from"]!.GetValue<string>());
        var req = Read(Path.Combine(Staged, "rollback", "mod.m.json"));
        Assert.Equal("mod.m", req["unit"]!.GetValue<string>());
        Assert.Equal("kiln-webadmin: bob", req["by"]!.GetValue<string>());
        var unit = ((JsonArray)Staging.List(Kiln)["units"]!).Single()!;
        Assert.Equal("1.0.0", unit["previous"]!["version"]!.GetValue<string>());
        Assert.Equal("kiln-webadmin: bob", unit["rollback"]!["by"]!.GetValue<string>());
        Assert.Contains("is requested", Assert.Throws<StagingError>(() => Staging.Rollback(Kiln, "mod.m", "t")).Message, StringComparison.Ordinal);
        Assert.Equal(new[] { "rollback" }, Staging.Cancel(Kiln, "mod.m")["canceled"]!.AsArray().Select(x => x!.GetValue<string>()));
        // a staged update of the same folder: cancel it before a rollback
        Source("ok", ModFiles("m", "3.0.0"));
        Staging.StageMod(Kiln, "ok", null, "m", null, null, "t");
        Assert.Contains("is staged already", Assert.Throws<StagingError>(() => Staging.Rollback(Kiln, "mod.m", "t")).Message, StringComparison.Ordinal);
        DataSame(data);
    }

    // ---- Kiln ----
    private static readonly JsonObject Components = (JsonObject)JsonNode.Parse("""
        {"core": {"sides": ["server", "client"], "default": true, "requires": []},
         "admin": {"sides": ["server"], "default": true, "requires": ["core"]},
         "samples": {"sides": ["server"], "default": false, "requires": []},
         "ui": {"sides": ["client"], "default": true, "requires": []}}
        """)!;

    private void Installed(Dictionary<string, (string Text, string Component, bool Config)> files, string version = "0.2.0-dev.30.9",
        string? components = null, string? deselected = null)
    {
        foreach (var (rel, f) in files) Put(Path.Combine(_game, rel), f.Text);
        var receipt = new JsonObject
        {
            ["format"] = 1, ["installId"] = "0b8a6c1e-1111-4111-8111-111111111111", ["installed"] = "2026-11-01T00:00:00Z", ["side"] = "server",
            ["platform"] = "proton",
            ["runtime"] = JsonNode.Parse($$"""{"version": "{{version}}", "components": {{components ?? "[\"admin\",\"core\"]"}}{{(deselected is null ? "" : ", \"deselected\": " + deselected)}}}"""),
            ["files"] = new JsonArray([.. files.Select(kv => (JsonNode)new JsonObject
            {
                ["path"] = kv.Key, ["sha256"] = Sha(kv.Value.Text), ["size"] = Encoding.UTF8.GetByteCount(kv.Value.Text),
                ["component"] = kv.Value.Component, ["config"] = kv.Value.Config,
            })]),
            ["replaced"] = new JsonArray(),
        };
        Put(Path.Combine(Kiln, ".install.json"), receipt.ToJsonString());
    }

    private string Bundle(string name, Dictionary<string, (string Text, string Component, bool Config)> files, string version = "0.2.0-dev.30.10")
    {
        var dir = Path.Combine(Incoming, name);
        foreach (var (rel, f) in files) Put(Path.Combine(dir, rel), f.Text);
        var manifest = new JsonObject
        {
            ["format"] = 1, ["version"] = version, ["kiln"] = version, ["components"] = Components.DeepClone(),
            ["files"] = new JsonArray([.. files.Select(kv => (JsonNode)new JsonObject
            {
                ["path"] = kv.Key, ["size"] = Encoding.UTF8.GetByteCount(kv.Value.Text), ["sha256"] = Sha(kv.Value.Text),
                ["component"] = kv.Value.Component, ["config"] = kv.Value.Config,
            })]),
        };
        Put(Path.Combine(dir, "kiln-runtime.json"), manifest.ToJsonString());
        return dir;
    }

    private static Dictionary<string, (string, string, bool)> Old() => new()
    {
        ["dinput8.dll"] = ("proxy 1", "core", false),
        ["kiln/kiln-core.dll"] = ("core 1", "core", false),
        ["kiln/host/Kiln.Host.dll"] = ("host same", "core", false),
        ["kiln/kiln.json"] = ("{\"mine\": true}", "core", true),
        ["kiln/services/gone.dll"] = ("gone 1", "core", false),
        ["kiln/mods/kiln-admin/Kiln.Admin.dll"] = ("admin 1", "admin", false),
    };

    private static Dictionary<string, (string, string, bool)> New() => new()
    {
        ["dinput8.dll"] = ("proxy 2", "core", false),
        ["kiln/kiln-core.dll"] = ("core 2", "core", false),
        ["kiln/host/Kiln.Host.dll"] = ("host same", "core", false),        // unchanged: not copied
        ["kiln/kiln.json"] = ("{\"default\": true}", "core", true),       // the player's config: kept
        ["kiln/host/Kiln.New.dll"] = ("new 2", "core", false),
        ["kiln/mods/kiln-admin/Kiln.Admin.dll"] = ("admin 2", "admin", false),
        ["kiln/mods/hello/Hello.dll"] = ("hello", "samples", false),     // not a default: not installed
        ["kiln/ui/Kiln.Ui.dll"] = ("ui", "ui", false),                     // client only
    };

    [Fact]
    public void A_Kiln_bundle_is_staged_as_a_plan_for_this_install()
    {
        Installed(Old());
        var data = DataBefore();
        var receiptBefore = File.ReadAllBytes(Path.Combine(Kiln, ".install.json"));
        Bundle("kiln-30.10", New());
        var r = Staging.StageKiln(Kiln, "kiln-30.10", null, "kiln 0.2.0-dev.30.10", new string('a', 64), "kiln-webadmin: alice");
        Assert.Equal("kiln", r["unit"]!.GetValue<string>());
        var dir = Path.Combine(Staged, "kiln");
        var doc = Read(Path.Combine(dir, "staged.json"));
        Assert.Equal("kiln", doc["kind"]!.GetValue<string>());
        Assert.Equal("0.2.0-dev.30.10", doc["version"]!.GetValue<string>());
        Assert.Equal("0.2.0-dev.30.9", doc["from"]!.GetValue<string>());
        Assert.Equal(Convert.ToHexStringLower(SHA256.HashData(receiptBefore)), doc["base"]!["receipt"]!.GetValue<string>());
        var files = ((JsonArray)doc["files"]!).Select(x => x!["path"]!.GetValue<string>()).ToList();
        Assert.Equal(new[] { "dinput8.dll", "kiln/kiln-core.dll", "kiln/host/Kiln.New.dll", "kiln/mods/kiln-admin/Kiln.Admin.dll", "kiln/.install.json" }.Order(StringComparer.Ordinal),
            files.Order(StringComparer.Ordinal));
        var removes = ((JsonArray)doc["remove"]!).Select(x => (x!["path"]!.GetValue<string>(), x["sha256"]!.GetValue<string>())).ToList();
        Assert.Equal(new[] { ("kiln/services/gone.dll", Sha("gone 1")) }, removes);
        // every staged file is what staged.json says
        foreach (var f in (JsonArray)doc["files"]!)
            Assert.Equal(f!["sha256"]!.GetValue<string>(), Staging.Sha256Of(Path.Combine(dir, "files", f["path"]!.GetValue<string>())));
        // the new receipt: this version, the same components, the bundle's files for them, the install's identity kept
        var next = Read(Path.Combine(dir, "files", "kiln", ".install.json"));
        Assert.Equal("0.2.0-dev.30.10", next["runtime"]!["version"]!.GetValue<string>());
        Assert.Equal(new string('a', 64), next["runtime"]!["sha256"]!.GetValue<string>());
        Assert.Equal(new[] { "admin", "core" }, next["runtime"]!["components"]!.AsArray().Select(x => x!.GetValue<string>()));
        Assert.Equal("0b8a6c1e-1111-4111-8111-111111111111", next["installId"]!.GetValue<string>());
        Assert.Equal(6, next["files"]!.AsArray().Count);  // core + admin files of the bundle
        Assert.Contains("kiln-admin", next["installer"]!.GetValue<string>(), StringComparison.Ordinal);
        // nothing installed changed yet
        Assert.Equal("proxy 1", File.ReadAllText(Path.Combine(_game, "dinput8.dll")));
        Assert.Equal(receiptBefore, File.ReadAllBytes(Path.Combine(Kiln, ".install.json")));
        DataSame(data);
        var unit = ((JsonArray)Staging.List(Kiln)["units"]!).Single()!;
        Assert.Equal("0.2.0-dev.30.9", unit["current"]!["version"]!.GetValue<string>());
        Assert.Equal(1, unit["staged"]!["remove"]!.GetValue<int>());
    }

    [Fact]
    public void Kiln_components_follow_kilnctls_upgrade_rule()
    {
        // an opt-out stays out; a default the player never turned off comes in; requirements are added
        var receipt = (JsonObject)JsonNode.Parse("""{"runtime": {"components": ["core"], "deselected": ["admin"]}}""")!;
        var manifest = new JsonObject { ["components"] = Components.DeepClone() };
        Assert.Equal(new[] { "core" }, Staging.ResolveComponents(manifest, receipt, out var off).Order());
        Assert.Equal(new[] { "admin" }, off);
        receipt = (JsonObject)JsonNode.Parse("""{"runtime": {"components": ["samples"]}}""")!;
        Assert.Equal(new[] { "admin", "core", "samples" }, Staging.ResolveComponents(manifest, receipt, out _).Order());
    }

    [Fact]
    public void Kiln_refusals()
    {
        var data = DataBefore();
        void Refused(string what, Action stage)
        {
            Assert.Contains(what, Assert.Throws<StagingError>(stage).Message, StringComparison.Ordinal);
            Assert.False(Directory.Exists(Path.Combine(Staged, "kiln")), what);
            Assert.Empty(Directory.EnumerateDirectories(Staged, ".stage-*"));
        }
        Bundle("b", New());
        Installed(Old());
        data = DataBefore();
        // a bundle file under kiln/data (or anywhere a Kiln update may not write) is refused before anything is copied
        var withData = New();
        withData["kiln/data/registry/entry-compat.json"] = ("{\"hijack\":1}", "core", false);
        Bundle("data", withData);
        Refused("under kiln/data", () => Staging.StageKiln(Kiln, "data", null, null, null, "t"));
        var outside = New();
        outside["enshrouded_server.exe"] = ("exe", "core", false);
        Bundle("outside", outside);
        Refused("outside kiln/", () => Staging.StageKiln(Kiln, "outside", null, null, null, "t"));
        // the bundle's bytes are not what its manifest says
        Bundle("tampered", New());
        File.WriteAllText(Path.Combine(Incoming, "tampered", "kiln", "kiln-core.dll"), "core X");
        Refused("is not what kiln-runtime.json says", () => Staging.StageKiln(Kiln, "tampered", null, null, null, "t"));
        // dinput8.dll is not the one Kiln installed
        File.WriteAllText(Path.Combine(_game, "dinput8.dll"), "another loader");
        Refused("dinput8.dll is not the one Kiln installed", () => Staging.StageKiln(Kiln, "b", null, null, null, "t"));
        File.WriteAllText(Path.Combine(_game, "dinput8.dll"), "proxy 1");
        // already that version, nothing to change
        Installed(New().Where(kv => kv.Value.Item2 is "core" or "admin").ToDictionary(), "0.2.0-dev.30.10");
        Bundle("same", New());
        Refused("is installed already", () => Staging.StageKiln(Kiln, "same", null, null, null, "t"));
        DataSame(data);
    }

    [Fact]
    public void A_server_pack_install_without_a_receipt_gets_the_manual_update_and_a_receipt()
    {
        // unzipped by hand: Kiln's files, no kiln/.install.json; a stale service the new version no longer has
        foreach (var (rel, f) in Old()) Put(Path.Combine(_game, rel), f.Item1);
        Put(Path.Combine(_game, "dinput8.dll"), "MZ... KilnProxy/2 0.2.0-dev.30.9 ...");
        Put(Path.Combine(Kiln, "services", "kiln-old.dll"), "stale");
        Put(Path.Combine(Kiln, "plugins", "someone-elses.dll"), "a plugin");  // outside Kiln's own folders: kept
        var data = DataBefore();
        Bundle("pack", New());
        var r = Staging.StageKiln(Kiln, "pack", null, null, null, "t", running: "0.2.0-dev.30.9");
        Assert.Contains("had no receipt", r["note"]!.GetValue<string>(), StringComparison.Ordinal);
        var doc = Read(Path.Combine(Staged, "kiln", "staged.json"));
        Assert.Null(doc["base"]!["receipt"]);  // kiln-core checks there is still no receipt when it applies
        Assert.Equal("0.2.0-dev.30.9", doc["from"]!.GetValue<string>());  // no receipt: the version this server runs
        var removes = ((JsonArray)doc["remove"]!).Select(x => x!["path"]!.GetValue<string>()).Order(StringComparer.Ordinal).ToList();
        Assert.Equal(new[] { "kiln/services/gone.dll", "kiln/services/kiln-old.dll" }, removes);
        var next = Read(Path.Combine(Staged, "kiln", "files", "kiln", ".install.json"));
        Assert.Equal("server", next["side"]!.GetValue<string>());
        Assert.Equal(new[] { "admin", "core" }, next["runtime"]!["components"]!.AsArray().Select(x => x!.GetValue<string>()));
        Assert.True(Guid.TryParse(next["installId"]!.GetValue<string>(), out _));
        DataSame(data);
        // another loader's dinput8.dll (no Kiln marker) is not replaced
        Staging.Cancel(Kiln, "kiln");
        Put(Path.Combine(_game, "dinput8.dll"), "MZ another loader");
        Assert.Contains("dinput8.dll is not the one Kiln installed",
            Assert.Throws<StagingError>(() => Staging.StageKiln(Kiln, "pack", null, null, null, "t")).Message, StringComparison.Ordinal);
    }

    [Fact]
    public void A_Kiln_rollback_is_refused_once_kilnctl_or_the_launcher_changed_Kiln()
    {
        // the kept version holds only the files its change changed: over another install it would make a mix
        Installed(Old(), version: "0.2.0-dev.30.11");
        var receipt = File.ReadAllBytes(Path.Combine(Kiln, ".install.json"));
        void Kept(string? receiptSha) => Put(Path.Combine(Staged, "previous", "kiln", "kept.json"),
            $$"""{"format":1,"unit":"kiln","version":"0.2.0-dev.30.9","replacedBy":"0.2.0-dev.30.10","kept":"2026-11-23T10:00:00.000Z","reason":"update","entries":[{"path":"kiln/kiln-core.dll","before":"present"}]{{(receiptSha is null ? "" : $",\"receipt\":\"{receiptSha}\"")}}}""");
        Kept(Sha("{\"the receipt\": \"the staged change left\"}"));
        Assert.Contains("Kiln changed since", Assert.Throws<StagingError>(() => Staging.Rollback(Kiln, "kiln", "t")).Message, StringComparison.Ordinal);
        Assert.False(File.Exists(Path.Combine(Staged, "rollback", "kiln.json")));
        Kept(Convert.ToHexStringLower(SHA256.HashData(receipt)));  // the receipt is still the one the change left
        Assert.Equal("0.2.0-dev.30.9", Staging.Rollback(Kiln, "kiln", "t")["to"]!.GetValue<string>());
    }

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, EntryPoint = "GetShortPathNameW")]
    private static extern uint GetShortPathName(string longPath, [Out] char[] shortPath, uint length);

    /// <summary>The Windows 8.3 short name of an existing folder, or null (not Windows, or the volume makes none).</summary>
    private static string? ShortName(string dir)
    {
        if (!OperatingSystem.IsWindows()) return null;
        var buf = new char[4096];
        var n = GetShortPathName(dir, buf, (uint)buf.Length);
        if (n == 0 || n >= buf.Length) return null;
        var name = Path.GetFileName(new string(buf, 0, (int)n));
        return string.Equals(name, Path.GetFileName(dir), StringComparison.OrdinalIgnoreCase) ? null : name;
    }

    [Fact]
    public void A_mod_folder_is_named_by_its_own_name_not_a_short_name()
    {
        // kiln/mods/KILN-A~1 is kiln/mods/kiln-admin-console on a volume with 8.3 names: a unit by that name would replace
        // another folder (and pass the receipt's "updated with Kiln" rule)
        foreach (var (rel, text) in ModFiles("kiln-admin", "1.0.0")) Put(Path.Combine(Kiln, "mods", "kiln-admin-console", rel), text);
        if (ShortName(Path.Combine(Kiln, "mods", "kiln-admin-console")) is not { } alias) return;  // no other names here
        Source("adm", ModFiles("kiln-admin", "2.0.0"));
        Assert.Contains("own name", Assert.Throws<StagingError>(() => Staging.StageMod(Kiln, "adm", null, alias, null, null, "t")).Message,
            StringComparison.Ordinal);
        Assert.False(Directory.Exists(Path.Combine(Staged, "mods")) && Directory.EnumerateDirectories(Path.Combine(Staged, "mods")).Any());
        Assert.Equal("unit", Staging.StageMod(Kiln, "adm", null, "kiln-admin-console", null, null, "t").First().Key);
    }

    [Fact]
    public void The_files_list_holds_for_the_very_bytes_that_are_staged()
    {
        // kiln/staged/incoming changes while it is staged: the manifest read first, the files copied last must both be
        // the ones the caller's list (from the archive whose signature it checked) names
        Installed(Old());
        var src = Bundle("kiln-30.10", New());
        var expect = Directory.EnumerateFiles(src, "*", SearchOption.AllDirectories)
            .ToDictionary(f => Path.GetRelativePath(src, f).Replace('\\', '/'), f => Staging.Sha256Of(f)!, StringComparer.Ordinal);
        var good = File.ReadAllText(Path.Combine(src, "kiln-runtime.json"));
        var evil = (JsonObject)JsonNode.Parse(good)!;
        foreach (var f in (JsonArray)evil["files"]!)
            if (f!["path"]!.GetValue<string>() == "kiln/kiln-core.dll") { f["sha256"] = Sha("evil core"); f["size"] = 9; }
        File.WriteAllText(Path.Combine(src, "kiln-runtime.json"), evil.ToJsonString());
        File.WriteAllText(Path.Combine(src, "kiln", "kiln-core.dll"), "evil core");
        try
        {
            Staging.TestStep = step =>
            {
                if (step == "manifest")  // read: the listed bytes are back for the check of the list
                {
                    File.WriteAllText(Path.Combine(src, "kiln-runtime.json"), good);
                    File.WriteAllText(Path.Combine(src, "kiln", "kiln-core.dll"), "core 2");
                }
                if (step == "copy") File.WriteAllText(Path.Combine(src, "kiln", "kiln-core.dll"), "evil core");  // and gone again
            };
            Assert.Throws<StagingError>(() => Staging.StageKiln(Kiln, "kiln-30.10", expect, null, null, "t"));
        }
        finally
        {
            Staging.TestStep = null;
        }
        Assert.False(Directory.Exists(Path.Combine(Staged, "kiln")));
        // a file changed between the check of the list and its copy alone is refused too
        File.WriteAllText(Path.Combine(src, "kiln", "kiln-core.dll"), "core 2");
        try
        {
            Staging.TestStep = step => { if (step == "copy") File.WriteAllText(Path.Combine(src, "kiln", "host", "Kiln.New.dll"), "new X"); };
            Assert.Throws<StagingError>(() => Staging.StageKiln(Kiln, "kiln-30.10", expect, null, null, "t"));
        }
        finally
        {
            Staging.TestStep = null;
        }
        Assert.False(Directory.Exists(Path.Combine(Staged, "kiln")));
    }

    [Fact]
    public void Staged_updates_are_for_dedicated_servers_only()
    {
        Assert.Contains("dedicated server", Assert.Throws<StagingError>(() => Staging.ServerOnly(false)).Message, StringComparison.Ordinal);
        Staging.ServerOnly(true);
    }

    [Fact]
    public void The_kiln_folder_is_the_servers_wherever_the_data_directory_is()
    {
        // a profile with its own data directory (kiln/profiles/<id>/data, or the launcher's library) must not move kiln/staged
        var game = Path.Combine(Path.GetTempPath(), "srv");
        Assert.Equal(Path.Combine(game, "kiln"),
            Staging.KilnFolderOf(Path.Combine(game, "enshrouded_server.exe"), Path.Combine(game, "kiln", "profiles", "p1", "data", "kiln-admin")));
        Assert.Equal(Path.Combine(game, "kiln"), Staging.KilnFolderOf(null, Path.Combine(game, "kiln", "data", "kiln-admin")));
    }

    [Fact]
    public void Cancel_and_rollback_never_wait_for_a_staging_in_progress()
    {
        Put(Path.Combine(Staged, "rollback", "mod.m.json"), "{}");
        using var copying = new ManualResetEventSlim();
        using var release = new ManualResetEventSlim();
        // a staging that holds the gate (a long copy on a pool thread)
        var holder = Task.Run(() =>
        {
            var gate = typeof(Staging).GetField("Gate", System.Reflection.BindingFlags.NonPublic | System.Reflection.BindingFlags.Static)!.GetValue(null)!;
            lock (gate)
            {
                copying.Set();
                release.Wait(TimeSpan.FromSeconds(10));
            }
        });
        Assert.True(copying.Wait(TimeSpan.FromSeconds(10)));
        var watch = System.Diagnostics.Stopwatch.StartNew();
        Assert.Contains("being staged right now", Assert.Throws<StagingError>(() => Staging.Cancel(Kiln, "mod.m")).Message, StringComparison.Ordinal);
        Assert.Contains("being staged right now", Assert.Throws<StagingError>(() => Staging.Rollback(Kiln, "mod.m", "t")).Message, StringComparison.Ordinal);
        Assert.True(watch.ElapsedMilliseconds < 1000, $"{watch.ElapsedMilliseconds} ms");
        Assert.NotNull(Staging.List(Kiln)["units"]);  // the list never takes the gate
        release.Set();
        holder.Wait();
        Assert.Equal(new[] { "rollback" }, Staging.Cancel(Kiln, "mod.m")["canceled"]!.AsArray().Select(x => x!.GetValue<string>()));
    }

    [Fact]
    public void List_shows_every_unit_and_the_latest_results()
    {
        Put(Path.Combine(Staged, "probation", "mod.a.json"), """{"format":1,"unit":"mod.a","state":"failed","version":"2","error":"boom"}""");
        Put(Path.Combine(Staged, "probation", "kiln.json"), """{"format":1,"unit":"kiln","state":"booting","version":"0.2.0-dev.30.10"}""");
        Put(Path.Combine(Staged, "tx", "mod.b", "journal.json"), "{}");
        Put(Path.Combine(Staged, "probation", "mod.a.json.tmp"), "{}");
        Directory.CreateDirectory(Path.Combine(Staged, "mods", ".hidden"));
        for (var i = 0; i < 25; i++)
            Put(Path.Combine(Staged, "results", $"20261123T1000{i:00}000Z-0000-mod.a.json"), $$"""{"format":1,"id":"{{i}}","ok":true}""");
        var l = Staging.List(Kiln);
        var units = ((JsonArray)l["units"]!).Select(u => u!["unit"]!.GetValue<string>()).ToList();
        Assert.Equal(new[] { "kiln", "mod.a", "mod.b" }, units);
        var a = ((JsonArray)l["units"]!)[1]!;
        Assert.Equal("failed", a["probation"]!["state"]!.GetValue<string>());
        Assert.Equal("boom", a["probation"]!["error"]!.GetValue<string>());
        Assert.True(((JsonArray)l["units"]!)[2]!["open"]!.GetValue<bool>());
        var results = (JsonArray)l["results"]!;
        Assert.Equal(20, results.Count);
        Assert.Equal("24", results[0]!["id"]!.GetValue<string>());  // newest first
        Assert.Equal(Path.Combine(Path.GetFullPath(Kiln), "staged", "incoming"), l["incoming"]!.GetValue<string>());
    }
}
