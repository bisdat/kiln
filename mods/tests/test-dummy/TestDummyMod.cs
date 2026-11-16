// SPDX-License-Identifier: MIT
using Kiln;

namespace TestDummy;

/// <summary>
/// <c>/dummy</c> (admins): the game's Training Doll 3 m from you; <c>/dummy enemy [template]</c>: a creature (default
/// Enemy_Vukah_Melee: it fights back) for effects that need an enemy; <c>/dummy clear</c>: removes them. A dummy is
/// healed to full after every hit, so it never breaks or dies, and after 5 s without a hit it tells the attacker in
/// chat what they did. Dummies are transient (not saved).
/// </summary>
public sealed class TestDummyMod : Mod
{
    private const string Doll = "Prop_Castle_TargetDummy_TrainingDoll_01";
    private const string Enemy = "Enemy_Vukah_Melee";

    private sealed class Session
    {
        public long First, Last, Total, Biggest;
        public int Hits, Crits;
    }

    private readonly HashSet<EntityId> _dummies = new();
    private readonly HashSet<EntityId> _placed = new();  // dolls placed through the building system (saved with the world)
    private readonly Dictionary<EntityId, int> _shield = new();  // /dummy enemies: hits still healed back to full, then it can die
    private readonly Dictionary<(EntityId Player, EntityId Dummy), Session> _sessions = new();
    private EventSubscription? _damaged;
    private IDisposable? _command;

    public override void OnLoad()
    {
        _damaged = Events.On<EntityDamagedEvent>(OnDamaged);
        Context.Tick.Pre += OnTick;
        _command = Commands.Register("dummy", c => _ = Run(c), CommandPermission.Admin, usage: "[enemy [template] | enemies [count] [hits] [template] | clear]",
            description: "a target dummy that never breaks, with a damage line in chat");
        Log.Info("test-dummy: /dummy, /dummy enemy [template], /dummy clear");
    }

    public override void OnUnload()
    {
        _damaged?.Dispose();
        _command?.Dispose();
        Context.Tick.Pre -= OnTick;
        Remove();
    }

    private int Remove()
    {
        var n = _dummies.Count;
        foreach (var d in _dummies)
        {
            if (_placed.Contains(d)) Build.RemoveProp(d, new BuildOptions { Flags = BuildFlags.InBases });
            else Spawn.Destroy(d);
        }
        _dummies.Clear();
        _placed.Clear();
        _shield.Clear();
        return n;
    }

    private async Task Run(CommandContext c)
    {
        var who = c.Source.Player;
        if (who.IsNone && Players.All is [var only]) who = only.Entity;
        if (who.IsNone) { c.Reply("run it as a player"); return; }
        var sub = c.Count > 0 ? c.Args[0].ToLowerInvariant() : "";
        if (sub == "clear")
        {
            var n = Remove();
            c.Reply($"removed {n} dumm{(n == 1 ? "y" : "ies")}");
            return;
        }
        if (sub == "upgrade")
        {
            // a code Upgrade at the nearest forge of a gem the forge lists: an item in a Generic page (backpack, action
            // bar) with a gem in its socket; worn gear is not listed by the forge
            var inv = Items.Read(who);
            var carrier = inv?.Slots.FirstOrDefault(x => !x.IsEmpty && x.Category == InventoryCategory.Generic && Gems.Socket(x.Ref) is not null);
            if (carrier is null) { c.Reply("nothing with a gem in your backpack or action bar"); return; }
            var forge = NearestForge(who);
            if (forge.IsNone) { c.Reply("no Shroud Forge within 30 m"); return; }
            var r = await Gems.Upgrade(who, carrier.Ref, forge);
            c.Reply($"code Upgrade of {carrier.Item}'s gem at forge {forge}: {r}");
            Log.Info($"test-dummy: code Upgrade of {carrier.Item}'s gem at forge {forge}: {r}");
            return;
        }
        if (sub == "socket")
        {
            // a code Socket at the nearest forge: a loose gem from the backpack into an empty socket of an item there
            var slots = Items.Read(who)?.Slots.Where(x => !x.IsEmpty && x.Category == InventoryCategory.Generic).ToList() ?? [];
            var carrier = slots.FirstOrDefault(x => !x.ItemEntity.IsNone && World.Has(x.ItemEntity, "WeaponGemSlot") && Gems.Socket(x.Ref) is null);
            var gem = slots.FirstOrDefault(x => Gems.Get(x.Item) is not null);
            if (carrier is null || gem is null) { c.Reply($"need an empty socket ({carrier?.Item}) and a loose gem ({gem?.Item}) in your backpack"); return; }
            var forge = NearestForge(who);
            if (forge.IsNone) { c.Reply("no Shroud Forge within 30 m"); return; }
            var r = await Gems.Socket(who, carrier.Ref, gem.Ref, forge);
            c.Reply($"code Socket of {gem.Item} into {carrier.Item} at forge {forge}: {r}");
            Log.Info($"test-dummy: code Socket of {gem.Item} into {carrier.Item} at forge {forge}: {r}");
            return;
        }
        if (sub == "inv")
        {
            // diagnostics for the gem return's room check: the player's inventory links, their category and free slots
            var inv = Items.Read(who);
            if (inv is null) { c.Reply("no inventory"); return; }
            foreach (var g in inv.Slots.GroupBy(x => (x.Link, x.Category, x.Slot.Inventory)).OrderBy(g => g.Key.Link))
            {
                var line = $"link {g.Key.Link} {g.Key.Category} ({(int)g.Key.Category}) inventory {g.Key.Inventory}: {g.Count()} slots, {g.Count(x => x.IsEmpty)} empty";
                c.Reply(line);
                Log.Info($"test-dummy: inv {line}");
            }
            return;
        }
        if (sub == "enemies")
        {
            var count = c.Count > 1 && int.TryParse(c.Args[1], out var n) ? Math.Clamp(n, 1, 12) : 4;
            var hits = c.Count > 2 && int.TryParse(c.Args[2], out var h) ? Math.Clamp(h, 0, 50) : 3;
            var kind = c.Count > 3 ? c.Args[3] : Enemy;
            if (Players.PositionOf(who) is not { } p0) { c.Reply("your position is not known"); return; }
            var made = 0;
            for (var i = 0; i < count; i++)
            {
                var a = 2 * Math.PI * i / count;
                var en = await Spawn.Entity(kind, new WorldPosition(p0.X + 6 * Math.Cos(a), p0.Y + 0.5, p0.Z + 6 * Math.Sin(a)), 0,
                    new SpawnOptions { Tag = "test-dummy", Flags = SpawnFlags.Transient });
                if (en.IsNone) continue;
                _dummies.Add(en);
                _shield[en] = hits;
                made++;
            }
            c.Reply($"{made} x {kind} in a ring 6 m around you; each is healed back to full for its first {hits} hits, then it can die");
            Log.Info($"test-dummy: {made} x {kind} around {who}, {hits} shielded hits each");
            return;
        }
        var template = sub == "enemy" ? (c.Count > 1 ? c.Args[1] : Enemy) : Doll;
        if (Players.PositionOf(who) is not { } at) { c.Reply("your position is not known"); return; }
        var where = new WorldPosition(at.X + 3, at.Y + 0.5, at.Z);
        // a prop spawned outside the building system is removed by the game after a few seconds: the doll is placed
        var e = template == Doll
            ? Build.PlaceProp(Doll, where, options: new BuildOptions { Flags = BuildFlags.InBases })
            : await Spawn.Entity(template, where, 0, new SpawnOptions { Tag = "test-dummy", Flags = SpawnFlags.Transient });
        if (template == Doll && !e.IsNone) _placed.Add(e);
        await Context.Scheduler.NextTick();
        await Context.Scheduler.NextTick();
        if (e.IsNone) { c.Reply($"could not spawn {template}"); return; }
        _dummies.Add(e);
        var hp = Combat.GetHealth(e);
        c.Reply($"{template} is 3 m east of you ({e}, health {hp?.Current}/{hp?.Max}); it heals to full after every hit");
        Log.Info($"test-dummy: {template} {e} for {who}, health {hp?.Current}/{hp?.Max}");
    }

    private static EntityId NearestForge(EntityId player)
    {
        if (Players.PositionOf(player) is not { } at) return EntityId.None;
        var best = EntityId.None;
        var bestD = 30.0 * 30.0;
        foreach (var id in Spawn.FindByName("Gem_Forge", max: 64))
        {
            if (World.Position(id) is not { } p) continue;
            var d = (p.X - at.X) * (p.X - at.X) + (p.Y - at.Y) * (p.Y - at.Y) + (p.Z - at.Z) * (p.Z - at.Z);
            if (d < bestD) { bestD = d; best = id; }
        }
        return best;
    }

    private void OnDamaged(EntityDamagedEvent e)
    {
        if (e.Amount <= 0 || !_dummies.Contains(e.Target)) return;
        if (_shield.TryGetValue(e.Target, out var left))
        {
            if (left > 0)
            {
                _shield[e.Target] = left - 1;
                Combat.SetHealth(e.Target, e.HealthMax);
            }
        }
        else Combat.SetHealth(e.Target, e.HealthMax);
        if (e.Attacker.IsNone) return;
        var key = (e.Attacker, e.Target);
        var now = Environment.TickCount64;
        if (!_sessions.TryGetValue(key, out var s)) _sessions[key] = s = new Session { First = now };
        s.Last = now;
        s.Total += e.Amount;
        s.Biggest = Math.Max(s.Biggest, e.Amount);
        s.Hits++;
        if (e.Flags.HasFlag(HitFlags.Crit)) s.Crits++;
    }

    private void OnTick(in TickInfo t)
    {
        if (_sessions.Count == 0 || t.Step % 8 != 0) return;
        var now = Environment.TickCount64;
        foreach (var (key, s) in _sessions.Where(kv => now - kv.Value.Last >= 5000).ToList())
        {
            _sessions.Remove(key);
            var secs = Math.Max(0.1, (s.Last - s.First) / 1000.0);
            var line = $"Dummy: {s.Total:N0} damage in {secs:0.0} s ({s.Total / secs:N0} DPS), {s.Hits} hits, biggest {s.Biggest:N0}, {s.Crits} crits";
            Chat.Tell(key.Player, line);
            Log.Info($"test-dummy: {line}");
        }
    }
}
