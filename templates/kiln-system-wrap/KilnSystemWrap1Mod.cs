using Kiln;

namespace KilnSystemWrap1;

// A system wrap runs your code in one of the game's own ECS programs: before its update, after it, instead of it, or
// around it per entity. This one wraps fuel_setup (the program that refills wells and water barrels) with AroundEach:
// before the game's update it notes each well's water, after it it sees what the game added and adds as much again,
// so wells refill twice as fast.
//
// Kiln.Game has a typed view of each of the game's programs (Ecs.Programs.FuelSetup): a property for each component
// the program reads or writes. A write through the view is marked for the save and sent to players like the game's own.
//
// The handlers run on the game's worker threads, several at once, once a player is in the world. So: nothing of
// Kiln's tick-thread API in them (they throw there), only your own data, shared through Interlocked.
public sealed class KilnSystemWrap1Mod : Mod
{
    private ProgramWrap<Ecs.Programs.FuelSetup>? _wrap;
    private bool _tried;
    private long _added;   // written by the game's worker threads

    public override void OnLoad()
    {
        // A wrap is made on a running world's program: wait for the server world's first step.
        Context.Tick.Post += OnPost;
    }

    private void OnPost(in TickInfo tick)
    {
        if (!_tried)
        {
            if (Systems.Find("fuel_setup") is null) return;  // no world yet
            _tried = true;
            // Refused (Ok = false) on a game build whose fuel_setup changed: the reason names the lists that did.
            var made = Systems.Wrap<Ecs.Programs.FuelSetup>();
            if (!made.Ok)
            {
                Log.Warn($"no wrap: {made.Reason}");
                return;
            }
            _wrap = made.Wrap!;
            _wrap.Disabled += e => Log.Warn($"the wrap stopped: {e.Reason}");
            _wrap.AroundEach(
                capture: (in Ecs.Programs.FuelSetup well) => well.FuelValues.Length > 0 ? well.FuelValues[0] : int.MinValue,
                apply: (ref Ecs.Programs.FuelSetup well, in int before) =>
                {
                    if (before == int.MinValue) return;
                    var added = well.FuelValues[0] - before;   // what the game's update just added (FuelValues: not marked)
                    if (added <= 0) return;
                    var fuel = well.Fuel;                      // the write slot: marked for the save and replication
                    fuel[0] += added;
                    fuel.Changed();                            // the game's formulas: never above the maximum
                    Interlocked.Add(ref _added, added);
                });
            return;
        }
        if (_wrap is not null && tick.Step % 2500 == 0 && Interlocked.Exchange(ref _added, 0) is var n and > 0)
            Log.Info($"wells got {n} units of water more than the game gave them");
    }
}
