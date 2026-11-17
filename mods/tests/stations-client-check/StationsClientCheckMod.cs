// SPDX-License-Identifier: MIT
using Kiln;

namespace StationsClientCheck;

/// <summary>
/// kiln.stations' real-client check (docs/kiln/services/stations.md, "Real-client checks"). Where the world runs (a
/// dedicated server or the hosting client): every station event in kiln.log as <c>stationcheck: event ...</c>, and
/// <c>/stationcheck</c> (admins) on the production station nearest to you (within 12 m):
/// <list type="bullet">
/// <item><c>base</c>: your base, its Magic chests, water sources and stations.</item>
/// <item><c>show</c>: the station (state, recipe, loaded crafts, inventory, outputs).</item>
/// <item><c>logs [n]</c>: n logs (default 100) into the base's first Magic chest, for a charcoal kiln.</item>
/// <item><c>plan [n]</c>, <c>fill [n] [add] [dry] [exact]</c>: <see cref="Stations.Plan"/> and <see cref="Stations.Fill"/>
/// up to n crafts (default 5).</item>
/// <item><c>stop [after]</c>, <c>resume</c>: <see cref="Stations.Stop"/> (now, or after the current craft) and
/// <see cref="Stations.Resume"/>.</item>
/// <item><c>recipes</c>, <c>select &lt;n&gt; [locked]</c>: the station's recipes, and <see cref="Stations.Select"/> of the
/// n-th (with <c>locked</c>: allowLocked).</item>
/// <item><c>collect</c>: <see cref="Stations.CollectToBase"/>.</item>
/// <item><c>cancel fill|select|stop|off</c>: the next such button a player presses at a station is canceled
/// (<see cref="StationPlayerActionEvent"/>).</item>
/// </list>
/// On a joined client it only says that kiln.stations is not there (a joined client has no server world).
/// </summary>
public sealed class StationsClientCheckMod : Mod
{
    private const string Logs = "Material_T1_Wood_Logs";
    private StationPlayerAction? _cancel;
    private readonly List<IDisposable> _subs = new();

    private void Say(string s) => Log.Info($"stationcheck: {s}");

    public override void OnLoad()
    {
        Say($"Stations.Available = {Stations.Available}, Bases.Available = {Bases.Available} (dedicated server {Client.IsDedicatedServer})");
        foreach (var row in new[] { Stations.ReadCapability, Stations.FillCapability, Stations.ControlCapability, Stations.EventsCapability })
            Say($"{row} = {(Capabilities.Is(row) ? "on" : "off")} ({Capabilities.Why(row)})");
        Try("StationStateChangedEvent", () => _subs.Add(Events.On<StationStateChangedEvent>(e => Say($"event: {e}"))));
        Try("StationCraftedEvent", () => _subs.Add(Events.On<StationCraftedEvent>(e => Say($"event: {e}"))));
        Try("StationPlacedEvent", () => _subs.Add(Events.On<StationPlacedEvent>(e => Say($"event: {e}"))));
        Try("StationRemovedEvent", () => _subs.Add(Events.On<StationRemovedEvent>(e => Say($"event: {e}"))));
        Try("BasesChangedEvent", () => _subs.Add(Events.On<BasesChangedEvent>(e => Say($"event: {e}"))));
        Try("StationChangedEvent", () => _subs.Add(Events.On<StationChangedEvent>(e => Say($"event: {e}"))));
        Try("StationPlayerActionEvent", () => _subs.Add(Events.On<StationPlayerActionEvent>(e =>
        {
            Say($"event: {e}");
            if (_cancel is { } a && e.Action == a && e.CanCancel)
            {
                _cancel = null;
                e.Cancel();
                Say($"canceled that {a} (the game does not run it)");
            }
        })));
        Try("commands", () => _subs.Add(Commands.Register("stationcheck", c => _ = Run(c), CommandPermission.Admin,
            usage: "base | show | logs [n] | plan [n] | fill [n] [add] [dry] [exact] | stop [after] | resume | recipes | select <n> [locked] | collect | cancel fill|select|stop|off",
            description: "kiln.stations' real-client check")));
    }

    public override void OnUnload()
    {
        foreach (var s in _subs) s.Dispose();
        _subs.Clear();
    }

    private void Try(string what, Action a)
    {
        try { a(); }
        catch (Exception e) { Say($"{what} not here: {e.Message}"); }
    }

    private static int Int(CommandContext c, int at, int fallback) => c.Count > at && int.TryParse(c.Args[at], out var x) ? x : fallback;
    private static bool Has(CommandContext c, string word) => c.Args.Skip(1).Any(a => string.Equals(a, word, StringComparison.OrdinalIgnoreCase));

    private async Task Run(CommandContext c)
    {
        var who = c.Source.Player;
        // from kiln/control/console (no player): the world's only player, a hosting client's own character
        if (who.IsNone && Players.All is [var only]) who = only.Entity;
        void Out(string s)
        {
            c.Reply(s);
            Say(s);
        }
        if (who.IsNone)
        {
            Out("run it as a player (or from the console with exactly one player in the world)");
            return;
        }
        try
        {
            var verb = c.Count > 0 ? c.Args[0].ToLowerInvariant() : "";
            if (verb == "cancel")
            {
                _cancel = (c.Count > 1 ? c.Args[1].ToLowerInvariant() : "") switch
                {
                    "fill" => StationPlayerAction.Fill,
                    "select" => StationPlayerAction.Select,
                    "stop" => StationPlayerAction.Stop,
                    _ => null,
                };
                Out(_cancel is null ? "no button is canceled" : $"the next {_cancel} a player presses at a station is canceled");
                return;
            }
            var home = Bases.Of(who);
            if (verb == "base")
            {
                if (home is null) { Out("you stand in no base"); return; }
                Out($"{home} \"{home.Name}\"; altars {string.Join(", ", home.Altars)}");
                Out($"  Magic chests: {string.Join(", ", Bases.Storage(home.Id))} (take deposits: {string.Join(", ", Bases.Storage(home.Id, deposit: true))})");
                Out($"  water: {string.Join(", ", Bases.WaterSources(home.Id))}");
                foreach (var s in Stations.InBase(home.Id)) Out($"  {Stations.Read(s)}");
                return;
            }
            if (verb == "logs")
            {
                if (home is null) { Out("you stand in no base"); return; }
                var chest = Bases.Storage(home.Id).FirstOrDefault();
                if (chest.IsNone) { Out("the base has no Magic chest"); return; }
                Out($"logs into {chest}: {await Items.Give(chest, Logs, Math.Clamp(Int(c, 1, 100), 1, 5000))}");
                return;
            }
            var station = Nearest(who);
            if (station.IsNone) { Out("no production station within 12 m"); return; }
            switch (verb)
            {
                case "show":
                {
                    var s = Stations.Read(station)!;
                    Out($"{s}");
                    Out($"  {s.Template}; stop pending {s.StopPending}; slots: {string.Join(", ", s.Slots.Where(x => !x.IsEmpty).Select(x => $"{x.Item} x{x.Count}"))}; outputs {s.Outputs.Count}, leftovers {s.Leftovers.Count}");
                    break;
                }
                case "plan":
                    Out($"Plan: {Stations.Plan(station, new FillOptions { Crafts = Math.Clamp(Int(c, 1, 5), 1, 1000) })}");
                    break;
                case "fill":
                {
                    var o = new FillOptions
                    {
                        Crafts = Math.Clamp(Int(c, 1, 5), 1, 1000), Mode = Has(c, "add") ? FillMode.Add : FillMode.UpTo, DryRun = Has(c, "dry"),
                        Exact = Has(c, "exact"),
                    };
                    Out($"Fill {o.Crafts} ({o.Mode}{(o.DryRun ? ", dry run" : "")}{(o.Exact ? ", exact" : "")}): {await Stations.Fill(station, o)}");
                    break;
                }
                case "stop":
                    Out($"Stop: {await Stations.Stop(station, Has(c, "after") ? StopWhen.AfterCurrentCraft : StopWhen.Now)}");
                    break;
                case "resume":
                    Out($"Resume: {await Stations.Resume(station)}");
                    break;
                case "recipes":
                {
                    var ids = Stations.RecipesOf(station);
                    for (var i = 0; i < ids.Count; i++)
                        if (Stations.Recipe(ids[i]) is { } r)
                            Out($"  {i}: 0x{r.Id:x8} -> {string.Join(", ", r.Outputs)} ({r.Duration.TotalSeconds} s)");
                    break;
                }
                case "select":
                {
                    var ids = Stations.RecipesOf(station);
                    var n = Int(c, 1, -1);
                    if (n < 0 || n >= ids.Count) { Out($"select 0..{ids.Count - 1} (see recipes)"); break; }
                    Out($"Select 0x{ids[n]:x8}: {await Stations.Select(station, ids[n], allowLocked: Has(c, "locked"))}");
                    break;
                }
                case "collect":
                    Out($"CollectToBase: {await Stations.CollectToBase(station)}");
                    break;
                default:
                    Out("/stationcheck base | show | logs [n] | plan [n] | fill [n] [add] [dry] [exact] | stop [after] | resume | recipes | select <n> [locked] | collect | cancel fill|select|stop|off");
                    break;
            }
        }
        catch (Exception e)
        {
            Out($"failed: {e.GetType().Name}: {e.Message}");
        }
    }

    private static EntityId Nearest(EntityId player) => Stations.Nearest(player, null, 12) ?? EntityId.None;
}
