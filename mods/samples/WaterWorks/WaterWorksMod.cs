// SPDX-License-Identifier: MIT
// WaterWorks: wells and water barrels refill faster. The game's own program fuel_setup tops a well's water up every
// so often (its whole units, once the fraction it keeps reaches one); a typed wrap of it with AroundEach notes each
// well's water before the game's update and, after it, adds (WellRefill - 1) times what the game just added. The write
// goes through the program's own write slot, so it is saved and sent to players like the game's own; the game's
// formulas (Attribute32.Changed) keep it at the well's maximum. A player sees the water bar of a well fill faster.
//
// What it shows (docs/kiln/services/systems.md): Systems.Wrap<T> with a view Kiln.Game generates for the program
// (Ecs.Programs.FuelSetup), refused with the lists that changed when a game update changes fuel_setup; AroundEach; a
// read that is not marked (FuelValues) before a write that is (Fuel); handlers that run on the game's worker threads
// once a player is in the world (no tick-thread API inside them).
using Kiln;

namespace WaterWorks;

public sealed class WaterWorksConfig
{
    [Comment("Times the game's own refill of wells and water barrels (1: as the game does it).")]
    [Range(1, 20)]
    public double WellRefill { get; set; } = 2;
}

public sealed class WaterWorksMod : Mod
{
    private ModConfig<WaterWorksConfig>? _config;
    private ProgramWrap<Ecs.Programs.FuelSetup>? _wrap;
    private bool _tried;
    private double _refill = 2;  // read by the handlers on the game's workers: a plain copy, set on the tick thread
    private long _extra;         // units added since the last report (any thread)

    public override void OnLoad()
    {
        _config = Config.Register<WaterWorksConfig>();
        _refill = _config.Value.WellRefill;
        // A wrap is made on a running world's program: wait for the server world's first step.
        Context.Tick.Post += OnPost;
    }

    [Subscribe]
    private void OnConfigChanged(ConfigChangedEvent e)
    {
        if (e.ModId == Context.Info.Id) Volatile.Write(ref _refill, _config!.Value.WellRefill);
    }

    private void OnPost(in TickInfo tick)
    {
        if (!_tried)
        {
            if (Systems.Find("fuel_setup") is null) return;  // no world yet
            _tried = true;
            Wrap();
        }
        else if (_wrap is not null && tick.Step % 2500 == 0 && Interlocked.Exchange(ref _extra, 0) is var extra and > 0)
            Log.Info($"WaterWorks: wells got {extra} units of water more than the game gave them lately");
    }

    private void Wrap()
    {
        var made = Systems.Wrap<Ecs.Programs.FuelSetup>();
        if (!made.Ok)
        {
            // another game build whose fuel_setup changed, or native SET is loaded: the wells keep the game's speed
            Log.Warn($"WaterWorks is off: {made.Reason}");
            return;
        }
        _wrap = made.Wrap!;
        _wrap.Disabled += e => Log.Warn($"WaterWorks stopped: {e.Reason}");
        _wrap.AroundEach(
            // before the game's update: the water now (FuelValues reads without marking anything)
            capture: (in Ecs.Programs.FuelSetup well) => well.FuelValues.Length > 0 ? well.FuelValues[0] : int.MinValue,
            // after it: what the game added, times (WellRefill - 1) on top
            apply: (ref Ecs.Programs.FuelSetup well, in int before) =>
            {
                if (before == int.MinValue) return;
                var added = well.FuelValues[0] - before;
                if (added <= 0) return;
                var more = (int)Math.Round(added * (Volatile.Read(ref _refill) - 1));
                if (more <= 0) return;
                var fuel = well.Fuel;   // the write slot: marked for the save and replication
                fuel[0] += more;
                fuel.Changed();         // the game's formulas: never above the well's maximum
                Interlocked.Add(ref _extra, more);
            });
        Log.Info($"WaterWorks: wells and water barrels refill {_refill:0.##} times as fast");
    }
}
