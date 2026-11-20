// SPDX-License-Identifier: MIT
using Kiln;

namespace TkWatch;

/// <summary>Watches every entity with Health (and every player) from its first Pre, logs each change it is told of,
/// and never disposes its watches: its unload must release them (ComponentWatchUnloadTests).</summary>
public sealed class TkWatchMod : Mod
{
    private readonly List<ComponentWatch> _watches = [];
    private bool _made;

    public override void OnLoad()
    {
        Events.On<ComponentValueChangedEvent>(e => Log.Info($"watch: event {e.Component} of {e.Entity} ({e.Origin})"));
        Context.Tick.Pre += (in TickInfo t) =>
        {
            if (_made) return;
            _made = true;
            var health = World.Find("Health").Take(128).ToArray();
            foreach (var r in new[]
                     {
                         ComponentChanges.Watch(new(ComponentWatchScope.Entities, health, ObservedComponents.Health)),
                         ComponentChanges.Watch(new(ComponentWatchScope.Players, [], ObservedComponents.Health | ObservedComponents.Stamina)),
                     })
            {
                Log.Info($"watch: made {r.Ok} {r.Pairs} {r.Reason}");
                if (r.Watch is { } w) _watches.Add(w);
            }
        };
    }
}
