// SPDX-License-Identifier: MIT
// The helper of mods/tests/component-watch-test's RELEASE rows (07 S7): see kiln.mod.json. It keeps its watches live
// until it unloads and never disposes them: its unload (or reload) has to release them by its owner.
using Kiln;

namespace ComponentWatchHelper;

public sealed class ComponentWatchHelperMod : Mod
{
    private const string Id = "component-watch-helper";
    private readonly string _nonce = Guid.NewGuid().ToString("N")[..8];
    private readonly List<ComponentWatch> _watches = [];
    private IDisposable? _sub;

    private string Data(string name) => Path.Combine(Context.DataDirectory, name);

    public override void OnLoad()
    {
        var mode = Path.GetFullPath(Path.Combine(Context.DataDirectory, "..", "component-watch-test", "mode"));
        if (!File.Exists(mode) || File.ReadAllText(mode).Trim() != "release")
        {
            Log.Info($"{Id}: not the release run: idle");
            return;
        }
        Directory.CreateDirectory(Context.DataDirectory);
        Log.Info($"{Id}: loaded {_nonce}");
        _ = Run();
    }

    private async Task Run()
    {
        for (var n = 0; n < 3000 && !Spawn.WorldRunning; n++) await Context.Scheduler.NextTick();
        for (var i = 0; i < 5; i++) await Context.Scheduler.NextTick();
        var at = new WorldPosition(3845, 835, 1490);
        var a = await Spawn.Entity("1_Player", at, options: new SpawnOptions { Tag = Id, Flags = SpawnFlags.Transient });
        var b = await Spawn.Entity("1_Player", at with { X = at.X + 3 }, options: new SpawnOptions { Tag = Id, Flags = SpawnFlags.Transient });
        for (var i = 0; i < 30; i++) await Context.Scheduler.NextTick();
        _sub = Events.On<ComponentValueChangedEvent>(e => File.AppendAllText(Data("events.txt"), $"{_nonce} {e.Step} {e.Entity.Value} {e.Component} {e.Origin}\n"));
        foreach (var r in new[]
                 {
                     ComponentChanges.Watch(new(ComponentWatchScope.Players, [], ObservedComponents.Health)),
                     ComponentChanges.Watch(new(ComponentWatchScope.Entities, [a], ObservedComponents.All, ComponentWatchExpansion.PlayerInventoryAndItems)),
                     ComponentChanges.Watch(new(ComponentWatchScope.Entities, [b], ObservedComponents.Health)),
                 })
        {
            if (r.Watch is { } w) _watches.Add(w);
            else Log.Warn($"{Id}: a watch was refused: {r.Reason}");
        }
        Spawn.Destroy(b);  // its watch is suspended at the next window: the unload releases a suspended watch too
        for (var i = 0; i < 4; i++) await Context.Scheduler.NextTick();
        Log.Info($"{Id}: {_nonce}: {_watches.Count} live watch(es) ({string.Join(", ", _watches.Select(w => $"{w.Id} {w.State} {w.Pairs}"))}); root {a.Value}");
        File.AppendAllText(Data("ready.txt"), $"{_nonce} {a.Value} {string.Join(" ", _watches.Select(w => w.Id))}\n");
    }

    public override void OnUnload()
    {
        // on purpose: nothing disposed, the subscription is left to the bus (it goes with the mod)
        Log.Info($"{Id}: unloading {_nonce} with {_watches.Count} watch(es) not disposed");
        if (File.Exists(Data("ready.txt"))) File.AppendAllText(Data("unloaded.txt"), $"{_nonce}\n");
    }
}
