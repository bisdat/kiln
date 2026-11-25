// SPDX-License-Identifier: MIT
using Xunit;

namespace Kiln.Contract.Tests;

/// <summary>Kiln.Quests' quests/*.json follow a mod's load, not only its id (the pattern of the live server's data-pack
/// report, 0.2.0-dev.31.10): an unload and a load between two checks (no ModReloadedEvent) read the new load's files.</summary>
public sealed class QuestsReloadTests : ContractTest
{
    private readonly IModsBackend? _mods = ModsBackend.Current;
    private readonly string _dir = Path.Combine(Path.GetTempPath(), "kiln-questsreload-" + Guid.NewGuid().ToString("N"));
    private readonly List<ModContext> _contexts = [];

    private sealed class Listed(Func<ModEntry[]> entries) : IModsBackend
    {
        public IReadOnlyList<ModEntry> All => entries();
        public ModSide Side => ModSide.Server;
        public ModEntry? Get(string id) => Array.Find(entries(), e => e.Id == id);
        public ModStats Stats(string id) => default;
        public bool Send(ModContext? sender, string targetId, string method, object? payload) => false;
        public Task<ModOperationResult> Request(ModOperation op, string arg) => throw new NotSupportedException();
        public void Request(ModOperation op, string arg, Action<ModOperationResult> done, ModContext? caller) => throw new NotSupportedException();
    }

    public override void Dispose()
    {
        ModsBackend.Current = _mods;
        foreach (var c in _contexts) c.Retire();
        try { Directory.Delete(_dir, true); } catch (IOException) { }
        base.Dispose();
    }

    private static string Quest(string id) =>
        $$"""{ "format": 1, "quests": { "{{id}}": { "name": { "en": "{{id}}" }, "steps": [ { "id": "s", "title": { "en": "S" }, "objective": { "kill": { "tag": "t", "count": 1 } } } ] } } }""";

    [Fact]
    public void An_unload_and_a_load_between_two_checks_read_the_new_files()
    {
        Directory.CreateDirectory(Path.Combine(_dir, "quests"));
        File.WriteAllText(Path.Combine(_dir, "quests", "q.json"), Quest("first_errand"));
        ModEntry entry = null!;
        void Load(string version)
        {
            var info = new ModInfo("questreload", "questreload", version, _dir);
            _contexts.Add(HostAccess.CreateContext(info, (ulong)(900 + _contexts.Count), "."));
            entry = new ModEntry("questreload", "questreload", version, _dir, ModSide.Server, ModState.Loaded, null, [], 0, info);
        }
        Load("1.0.0");
        ModsBackend.Current = new Listed(() => [entry]);
        Quests.CheckMods();
        Assert.Contains(Quests.Definitions, q => q.Id == "first_errand");

        File.WriteAllText(Path.Combine(_dir, "quests", "q.json"), Quest("second_errand"));
        Load("1.0.1");  // `unload questreload` and `load questreload` since the last check
        Quests.CheckMods();
        Assert.Contains(Quests.Definitions, q => q.Id == "second_errand");
        Assert.DoesNotContain(Quests.Definitions, q => q.Id == "first_errand");

        Quests.CheckMods();  // the same load: nothing read again
        ModsBackend.Current = new Listed(() => []);
        Quests.CheckMods();
        Assert.DoesNotContain(Quests.Definitions, q => q.Id == "second_errand");
    }
}
