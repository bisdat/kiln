// SPDX-License-Identifier: MIT
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using Xunit;

namespace Kiln.Contract.Tests;

/// <summary>Live server, Kiln 0.2.0-dev.31.10: a data-only mod hot-loaded with <c>load</c> (so the last loaded mod), its
/// kiln.mod.json changed, then <c>reload &lt;id&gt;</c>: its data packs were not re-applied. Kiln.Content told kiln.content
/// only the list of loaded mod ids, which a reload of the last mod leaves as it was. It now tells it each mod that was
/// loaded again under the same id (a new load: a new owner), with the reason, over a fake kiln.content table.</summary>
public sealed unsafe class ContentPackReloadTests : FakeTableTest
{
    private static readonly List<string> Calls = [];
    private readonly IModsBackend? _mods = ModsBackend.Current;
    private readonly Mods _list = new();
    private ulong _owner = 500;

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int FakePackMods(NativeStr json)
    {
        Calls.Add("mods " + json);
        return 0;
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int FakeReloaded(NativeStr mod, NativeStr reason)
    {
        Calls.Add($"reloaded {mod}: {reason}");
        return 0;
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int FakeReloadPacks()
    {
        Calls.Add("reload all");
        return 0;
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int FakeLabel(ulong owner, NativeStr label) => 0;

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static uint FakeRevert(ulong owner) => 0;

    private sealed class Mods : IModsBackend
    {
        public ModEntry[] Entries = [];
        public IReadOnlyList<ModEntry> All => Entries;
        public ModSide Side => ModSide.Server;
        public ModEntry? Get(string id) => Array.Find(Entries, e => e.Id == id);
        public ModStats Stats(string id) => default;
        public bool Send(ModContext? sender, string targetId, string method, object? payload) => false;
        public Task<ModOperationResult> Request(ModOperation op, string arg) => throw new NotSupportedException();
        public void Request(ModOperation op, string arg, Action<ModOperationResult> done, ModContext? caller) => throw new NotSupportedException();
    }

    private readonly Dictionary<string, ModContext> _live = new(StringComparer.Ordinal);

    public ContentPackReloadTests()
    {
        Calls.Clear();
        ModsBackend.Current = _list;
    }

    public override void Dispose()
    {
        ModsBackend.Current = _mods;
        base.Dispose();
    }

    private void Table(bool withReloaded)
    {
        var t = Alloc<ContentNative.Table>();
        var size = withReloaded ? (uint)sizeof(ContentNative.Table) : (uint)Marshal.OffsetOf<ContentNative.Table>(nameof(ContentNative.Table.PackModReloaded));
        t->Header = new ServiceHeader { Size = size, Version = 4 };
        t->PackMods = &FakePackMods;
        t->LabelOwner = &FakeLabel;
        t->RevertOwner = &FakeRevert;
        t->ReloadPacks = &FakeReloadPacks;
        if (withReloaded) t->PackModReloaded = &FakeReloaded;
        Swap(typeof(Content), "_table", Box(t));
        Swap(typeof(Content), "_lastModList", null);
        Swap(typeof(Content), "Loads", new PackModLoads());
    }

    /// <summary>Kiln.Host's load of <paramref name="id"/>: a new context (its own owner) under an Info of its own.</summary>
    private void Load(string id, string version)
    {
        if (_live.Remove(id, out var old)) old.Retire();
        var info = new ModInfo(id, id, version, "/mods/" + id);
        _live[id] = HostAccess.CreateContext(info, _owner++, ".");
    }

    private void Publish(params string[] order) =>
        _list.Entries = order.Select((id, i) => new ModEntry(id, id, _live[id].Info.Version, _live[id].Info.Directory, ModSide.Server,
            ModState.Loaded, null, [], i, _live[id].Info)).ToArray();

    [Fact]
    public void A_reload_of_the_last_loaded_mod_re_applies_its_packs_with_the_reason()
    {
        Table(withReloaded: true);
        Load("other", "1.0.0");
        Load("skilltest", "1.0.0");
        Publish("other", "skilltest");
        Content.SyncMods();
        Assert.Equal(["mods [\"other\",\"skilltest\"]"], Calls);
        Calls.Clear();
        Content.SyncMods();
        Assert.Empty(Calls);  // nothing changed: nothing sent

        // control command `reload skilltest`: the same id in the same place, a new load, a new version on disk
        Load("skilltest", "1.0.1");
        Publish("other", "skilltest");
        Content.OnModReloaded(Reloaded("skilltest", "1.0.1"));
        Content.SyncMods();
        Assert.Equal(["reloaded skilltest: was reloaded (1.0.0 -> 1.0.1)"], Calls);  // the list is the same: no pack_mods
        Calls.Clear();
        Content.SyncMods();
        Assert.Empty(Calls);  // once
    }

    [Fact]
    public void An_unload_and_a_load_between_two_syncs_re_apply_too()
    {
        Table(withReloaded: true);
        Load("skilltest", "1.0.0");
        Publish("skilltest");
        Content.SyncMods();
        Calls.Clear();
        Load("skilltest", "2.0.0");  // `unload skilltest`, then `load skilltest` with a new version, within 25 steps
        Publish("skilltest");
        Content.SyncMods();
        Assert.Equal(["reloaded skilltest: was unloaded and loaded again (1.0.0 -> 2.0.0)"], Calls);
    }

    [Fact]
    public void A_reload_names_the_dependents_it_reloaded_with()
    {
        Table(withReloaded: true);
        Load("lib", "1.0.0");
        Load("pack", "1.0.0");
        Publish("lib", "pack");
        Content.SyncMods();
        Calls.Clear();
        Load("lib", "1.0.0");
        Load("pack", "1.0.0");
        Publish("lib", "pack");
        Content.OnModReloaded(Reloaded("lib", "1.0.0", "pack"));
        Content.SyncMods();
        Assert.Equal(["reloaded lib: was reloaded (1.0.0)", "reloaded pack: was reloaded with lib (1.0.0)"], Calls);
    }

    [Fact]
    public void A_mod_that_moved_in_the_order_gets_the_new_list_and_its_reload()
    {
        Table(withReloaded: true);
        Load("a", "1.0.0");
        Load("b", "1.0.0");
        Publish("a", "b");
        Content.SyncMods();
        Calls.Clear();
        Load("a", "1.0.0");  // a reload appends the mod behind the others
        Publish("b", "a");
        Content.OnModReloaded(Reloaded("a", "1.0.0"));
        Content.SyncMods();
        Assert.Equal(["mods [\"b\",\"a\"]", "reloaded a: was reloaded (1.0.0)"], Calls);
    }

    [Fact]
    public void An_older_kiln_content_without_the_slot_re_applies_every_pack()
    {
        Table(withReloaded: false);
        Load("skilltest", "1.0.0");
        Publish("skilltest");
        Content.SyncMods();
        Calls.Clear();
        Load("skilltest", "1.0.1");
        Publish("skilltest");
        Content.SyncMods();
        Assert.Equal(["reload all"], Calls);
    }

    private static ModReloadedEvent Reloaded(string id, string version, params string[] cascade) => new(id, version, true, cascade);
}
