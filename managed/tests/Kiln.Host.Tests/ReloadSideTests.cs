// SPDX-License-Identifier: MIT
// `reload <id>` whose new kiln.mod.json changes "side" (registry-fixes item 1, review-registry-reload Q5). A mod's side holds for
// the whole process: the join compares the side-both mod lists, and the start's registry snapshot (bootSync) left out the fields
// a one-side mod writes. A reload from server to both made the lists match while the snapshot still left the mod's fields out,
// so Kiln clients got "registry boot values differ" (refused under modSync refuse) until a restart. The reload is now refused
// RestartRequired before anything unloads, like a new version that adds boot content, and the running version stays.
using Kiln.Host;
using Xunit;

namespace Kiln.Host.Tests;

public sealed class ReloadSideTests : ModsFixture
{
    private string Mod(string folder, string version, string side, string? dependsOn = null) => WriteManifest(folder,
        $$"""{ "id": "{{folder}}", "name": "{{folder}}", "version": "{{version}}", "apiVersion": "0.2", "side": "{{side}}"{{(dependsOn is null ? "" : $$""", "dependencies": {"{{dependsOn}}": ">=1.0.0"}""")}} }""");

    [Fact]
    public void A_reload_whose_new_version_changes_side_is_refused_before_the_running_version_unloads()
    {
        var host = new FakeHost();
        Mod("sided", "1.0.0", "server");
        Mod("sided-dep", "1.0.0", "both", "sided");
        using var mods = NewManager(host);
        mods.LoadAll();
        Assert.NotNull(mods.Find("sided"));
        Assert.NotNull(mods.Find("sided-dep"));

        Mod("sided", "1.1.0", "both");
        var r = mods.ReloadCore("sided");
        Assert.False(r.Ok);
        Assert.StartsWith("RestartRequired: mod sided's new version changes \"side\" in kiln.mod.json (server -> both)", r.Message);
        Assert.Contains("restart the game (or the server)", r.Message);
        Assert.EndsWith("The running version stays loaded", r.Message);
        Assert.Equal("1.0.0", mods.Find("sided")?.Manifest.Version);   // the running version stays loaded
        Assert.Equal(ManifestSide.Server, mods.Find("sided")?.Manifest.Side);
        Assert.NotNull(mods.Find("sided-dep"));                         // and so does its dependent
        Assert.True(host.Has("reload sided: RestartRequired: mod sided's new version changes \"side\""), host.Dump());

        // a dependent whose new version changes its side: the reload of what it depends on is refused the same way
        Mod("sided", "1.2.0", "server");
        Mod("sided-dep", "1.1.0", "server", "sided");
        var d = mods.ReloadCore("sided");
        Assert.False(d.Ok);
        Assert.StartsWith("RestartRequired: mod sided-dep's new version changes \"side\" in kiln.mod.json (both -> server)", d.Message);
        Assert.Equal("1.0.0", mods.Find("sided")?.Manifest.Version);
        Assert.Equal("1.0.0", mods.Find("sided-dep")?.Manifest.Version);

        // the same sides as at the start: the reload goes ahead as before
        Mod("sided-dep", "1.1.0", "both", "sided");
        var ok = mods.ReloadCore("sided");
        Assert.True(ok.Ok, ok.Message);
        Assert.Equal("1.2.0", mods.Find("sided")?.Manifest.Version);
        Assert.Equal("1.1.0", mods.Find("sided-dep")?.Manifest.Version);
    }

    // `unload <id>` then `load <folder>` does what a reload does (architecture.md "What reload <id> covers"): a load of a mod this
    // start loaded is refused the same way when its side is not the start's, and so is a reload of a mod that is not loaded
    // (its start snapshot has the start's side all the same).
    [Fact]
    public void An_unload_and_a_load_with_another_side_is_refused_and_so_is_a_reload_of_a_mod_that_is_not_loaded()
    {
        var host = new FakeHost();
        var dir = Mod("sided", "1.0.0", "both");
        using var mods = NewManager(host);
        mods.LoadAll();
        Assert.True(mods.Unload("sided", out _));
        Mod("sided", "1.1.0", "server");
        Assert.False(mods.TryLoad(dir, out var error));
        Assert.StartsWith("RestartRequired: mod sided's new version changes \"side\" in kiln.mod.json (both -> server)", error);
        Assert.Null(mods.Find("sided"));

        var r = mods.ReloadCore("sided");
        Assert.False(r.Ok);
        Assert.StartsWith("RestartRequired: mod sided's new version changes \"side\" in kiln.mod.json (both -> server)", r.Message);
        Assert.Null(mods.Find("sided"));

        Mod("sided", "1.1.0", "both");
        Assert.True(mods.TryLoad(dir, out var again), again);
        Assert.Equal("1.1.0", mods.Find("sided")?.Manifest.Version);
    }

    // review-registry-fixes: a client that may host a world hands its side "server" mods to the registry stage too (RegistryOrder:
    // they are in the start's registry snapshot with side server), but loads them only when it hosts, so they were never in
    // LoadAll's load order. A new version with another side then loaded with the next hosted world (or after an unload and a
    // load while hosting): the side-both lists named it while the start's registry values still left its fields out.
    [Fact]
    public void A_hosting_clients_server_mod_keeps_the_side_the_start_ordered_it_with()
    {
        var host = new FakeHost();
        var dir = Mod("hosted", "1.0.0", "server");
        using var mods = NewManager(host, new ModManagerOptions
        {
            Side = ManifestSide.Client, HostedServerMods = true, KilnVersion = host.Version, HasFeature = host.HasFeature,
            StateFile = Path.Combine(LogsDir, "mods.txt"), TestResultsDirectory = Path.Combine(LogsDir, "tests"),
        });
        mods.LoadAll();
        Assert.Null(mods.Find("hosted"));                       // a server mod waits for a hosted world
        Assert.Equal(["hosted"], mods.StartHosting());
        mods.StopHosting("test");

        // the next hosted world: the server mod's new version with side both does not load
        Mod("hosted", "1.1.0", "both");
        Assert.Empty(mods.StartHosting());
        Assert.Null(mods.Find("hosted"));
        Assert.StartsWith("RestartRequired: mod hosted's new version changes \"side\" in kiln.mod.json (server -> both)",
            Kiln.Mods.Get("hosted")?.Error ?? "");

        // while hosting: the start's side loads as before; an unload and a load with another side is refused
        Mod("hosted", "1.2.0", "server");
        Assert.True(mods.TryLoad(dir, out var again), again);
        Assert.Equal("1.2.0", mods.Find("hosted")?.Manifest.Version);
        Assert.True(mods.Unload("hosted", out _));
        Mod("hosted", "1.3.0", "both");
        Assert.False(mods.TryLoad(dir, out var error));
        Assert.StartsWith("RestartRequired: mod hosted's new version changes \"side\" in kiln.mod.json (server -> both)", error);
        Assert.Null(mods.Find("hosted"));
        mods.StopHosting("test");
    }

    // A mod this start did not load (hot-loaded later with `load`) keeps the side its running version loaded with.
    [Fact]
    public void A_mod_loaded_after_the_start_keeps_the_side_it_loaded_with()
    {
        var host = new FakeHost();
        using var mods = NewManager(host);
        mods.LoadAll();
        var dir = Mod("late", "1.0.0", "server");
        Assert.True(mods.TryLoad(dir, out var error), error);
        Mod("late", "1.0.1", "both");
        var r = mods.ReloadCore("late");
        Assert.False(r.Ok);
        Assert.StartsWith("RestartRequired: mod late's new version changes \"side\" in kiln.mod.json (server -> both)", r.Message);
        Assert.Equal("1.0.0", mods.Find("late")?.Manifest.Version);
    }
}
