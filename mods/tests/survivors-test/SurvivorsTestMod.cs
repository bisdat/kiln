// SPDX-License-Identifier: MIT
// Survivors lab test (dev.31 package 18; docs/kiln/services/survivors.md), for a lab server with no player.
// Run 1 (no state file): reads; RecruitWorld (written, then NoChange); a refused tool for a player who is not in game;
// a Flame Altar spawned for a base; Summon (a new NPC, then a move of the same one); beds: assign, NoChange, reassign,
// an actor who is not in game, a non-bed, a far bed, a second survivor displacing the first, unassign twice; a destroyed
// bed seen as a game-origin change; then the first survivor keeps a bed and the state file is written: stop the server
// (it saves) and start it again.
// Run 2 (state file there): the relation came back from the save (both ends, new entity ids), then everything is put
// back (unassign, despawn, revoke the unlocks) and the state file removed.
// Output: "survivors-test: PASS <what>" / "FAIL <what>: <why>" lines in kiln/logs/kiln.log, then "done: N PASS, M FAIL".
using System.Text.Json;
using Kiln;

namespace SurvivorsTest;

public sealed class SurvivorsTestMod : Mod
{
    private int _pass, _fail;
    private bool _running, _done;
    private ulong _start;
    private readonly List<SurvivorEvent> _events = [];

    private void Pass(string what) { _pass++; Log.Info($"survivors-test: PASS {what}"); }
    private void Fail(string what, string why) { _fail++; Log.Error($"survivors-test: FAIL {what}: {why}"); }
    private void Check(bool ok, string what, string why = "") { if (ok) Pass(what); else Fail(what, why); }
    private void Note(string s) => Log.Info($"survivors-test: {s}");
    private string StateFile => Path.Combine(Context.DataDirectory, "run1.json");

    private sealed record Saved(uint Survivor, uint Other, double BedX, double BedY, double BedZ, uint[] Unlocks);

    public override void OnLoad()
    {
        void Record(SurvivorEvent e)
        {
            lock (_events) _events.Add(e);
            Note($"event: {e}");
        }
        Events.On<SurvivorRecruitedEvent>(Record);
        Events.On<SurvivorToolGivenEvent>(Record);
        Events.On<SurvivorSummonedEvent>(Record);
        Events.On<SurvivorAssignmentChangedEvent>(Record);
        Context.Tick.Pre += OnPre;
    }

    private void OnPre(in TickInfo tick)
    {
        if (_start == 0) _start = tick.Step;
        var ready = Lifecycle.IsServerRunning && tick.Step >= Lifecycle.StartedAtStep + 50 && Knowledge.Status is { Applying: true, Npcs: > 0 };
        if (_done || _running || !(ready || tick.Step - _start > 12000)) return;
        _running = true;
        _ = Run().ContinueWith(t =>
        {
            if (t.Exception is { } e) Fail("run", e.GetBaseException().ToString());
            Note($"done: {_pass} PASS, {_fail} FAIL");
            _done = true;
        }, TaskScheduler.Current);
    }

    private async Task Steps(int n)
    {
        for (var i = 0; i < n; i++) await Context.Scheduler.NextTick();
    }

    private async Task<T?> WaitEvent<T>(Func<T, bool> match, int steps) where T : SurvivorEvent
    {
        for (var i = 0; i < steps; i++)
        {
            lock (_events)
                if (_events.OfType<T>().FirstOrDefault(match) is { } e) return e;
            await Context.Scheduler.NextTick();
        }
        return null;
    }

    private static string R(SurvivorResult r) =>
        $"{r.Completion} ok={r.Ok} effects={r.Effects} steps={r.Steps} reason={r.Reason ?? "-"} bed {r.Before.Bed}->{r.After.Bed} entity {r.After.Entity}";

    private Task Run() => File.Exists(StateFile) ? Run2() : Run1();

    private async Task Run1()
    {
        Note($"run 1: caps read={Capabilities.Is(Survivors.ReadCapability)} recruit={Capabilities.Is(Survivors.RecruitCapability)} " +
             $"summon={Capabilities.Is(Survivors.SummonCapability)} assign={Capabilities.Is(Survivors.AssignCapability)}");
        foreach (var c in new[] { Survivors.ReadCapability, Survivors.RecruitCapability, Survivors.SummonCapability, Survivors.AssignCapability })
            Check(Capabilities.Is(c), $"capability {c} on", Capabilities.Why(c));

        // ---- reads
        var all = Survivors.List();
        Check(all.Count > 0, $"roster lists {all.Count} survivors");
        var locked = all.Where(s => !s.Unlocked && s.SummonTool.Value != 0 && s.Entity.IsNone).ToList();
        Note($"locked survivors with a tool and no NPC: {string.Join(", ", locked.Take(8).Select(s => $"{s.DebugName} {s.Id}"))}");
        var a = locked.FirstOrDefault(s => s.Id.Value == 0x672da6c9) ?? locked.FirstOrDefault();
        if (a is null)
        {
            Fail("a locked lab survivor", "none in this world");
            return;
        }
        Pass($"roster lists the lab survivor locked: {a.DebugName} {a.Id}");
        Check(Survivors.Find(a.Id) is { Unlocked: false } f && f.DebugName == a.DebugName, "Find agrees with List");
        Check(Survivors.Find(new SurvivorId(0x12345678)) is null, "Find of a non-roster id is null");

        // ---- recruit
        var rec = await Survivors.RecruitWorld(a.Id);
        Note($"recruit: {R(rec)}");
        Check(rec.Ok && rec.Completion == SurvivorCompletion.Done &&
              rec.Effects.HasFlag(SurvivorEffects.KnowledgeWritten | SurvivorEffects.RegistryConfirmed), "recruit: knowledge written, registry confirmed", R(rec));
        Check(rec.After.Unlocked && !rec.Before.Unlocked, "recruit: before locked, after unlocked");
        Check(await WaitEvent<SurvivorRecruitedEvent>(e => e.Survivor == a.Id && e.ByKiln, 30) is not null, "recruit raised SurvivorRecruitedEvent");
        var again = await Survivors.RecruitWorld(a.Id);
        Check(again.Ok && again.Completion == SurvivorCompletion.NoChange && again.Effects == SurvivorEffects.None, "recruit again: NoChange, nothing written", R(again));
        var tool = await Survivors.RecruitWorld(a.Id, new SurvivorRecruitOptions(new Player { Entity = new EntityId(9), Name = "nobody" }));
        Check(!tool.Ok && tool.Completion == SurvivorCompletion.Refused && tool.Reason!.StartsWith("RecipientNotInGame"), "tool for a player not in game: Refused RecipientNotInGame", R(tool));

        // ---- a base
        var altarAt = new WorldPosition(3800, 835, 1500);
        var altar = await Spawn.Entity("Home_Base_FlameAltar", altarAt);
        Guid? baseId = null;
        for (var i = 0; i < 100 && baseId is null; i++)
        {
            await Steps(5);
            baseId = Survivors.BaseAt(new WorldPosition(3802, 835, 1500));
        }
        Check(baseId is not null, $"BaseAt finds the spawned altar's base ({baseId})");
        if (baseId is null) return;
        var zone = Build.Bases.FirstOrDefault(b => b.Altar == altar);
        Note($"altar {altar} zone {zone?.Center} half {zone?.Half}");
        Check(Survivors.BaseAt(new WorldPosition(9000, 835, 9000)) is null, "BaseAt outside every base is null");

        // ---- summon
        var outside = await Survivors.Summon(a.Id, baseId.Value, new WorldPosition(3800, 835, 1600));
        Check(!outside.Ok && outside.Reason!.StartsWith("PositionOutsideBase"), "summon outside the zone: Refused PositionOutsideBase", R(outside));
        var home1 = new WorldPosition(3802, 835, 1500);
        var sum = await Survivors.Summon(a.Id, baseId.Value, home1, 90);
        Note($"summon: {R(sum)}");
        Check(sum.Ok && sum.Effects.HasFlag(SurvivorEffects.EntityCreated) && !sum.After.Entity.IsNone, "summon: one entity created", R(sum));
        Check(sum.After.Home is { } h1 && Dist(h1, home1) <= 0.05, $"summon: home within 0.05 m ({sum.After.Home})");
        Check(sum.After.BaseId == baseId, "summon: the survivor's base is the altar's");
        Check(await WaitEvent<SurvivorSummonedEvent>(e => e.Survivor == a.Id, 30) is not null, "summon raised SurvivorSummonedEvent");
        Check(Survivors.Of(sum.After.Entity)?.Id == a.Id, "Of(entity) names the survivor");
        var home2 = new WorldPosition(3806, 835, 1504);
        var move = await Survivors.Summon(a.Id, baseId.Value, home2);
        Note($"move: {R(move)}");
        Check(move.Ok && move.Effects.HasFlag(SurvivorEffects.HomeMoved) && !move.Effects.HasFlag(SurvivorEffects.EntityCreated) &&
              move.After.Entity == sum.After.Entity, "summon again: the same entity moved (Reset), home moved", R(move));
        Check(move.After.Home is { } h2 && Dist(h2, home2) <= 0.05, $"move: home within 0.05 m ({move.After.Home})");

        // ---- beds
        var bed1 = await Spawn.Entity("Prop_Furniture_Comfort3_T1_Bed", new WorldPosition(3808, 835, 1504));
        var bed2 = await Spawn.Entity("Prop_Furniture_Comfort3_T1_Bed", new WorldPosition(3810, 835, 1504));
        await Steps(10);
        Check(Survivors.BedOccupant(bed1) is null && Survivors.BedOccupant(bed2) is null, "new beds are free");
        var as1 = await Survivors.AssignBed(a.Id, bed1, SurvivorAssignmentOptions.Trusted);
        Note($"assign: {R(as1)}");
        Check(as1.Ok && as1.Completion == SurvivorCompletion.Done && as1.Effects == SurvivorEffects.BedChanged && as1.After.Bed == bed1, "assign both ends", R(as1));
        Check(Survivors.BedOccupant(bed1) == a.Id, "bed side names the survivor");
        Check(await WaitEvent<SurvivorAssignmentChangedEvent>(e => e.Survivor == a.Id && e.ByKiln && e.NewBed == bed1, 30) is not null,
            "assign raised SurvivorAssignmentChangedEvent (by Kiln)");
        var same = await Survivors.AssignBed(a.Id, bed1, SurvivorAssignmentOptions.Trusted);
        Check(same.Ok && same.Completion == SurvivorCompletion.NoChange, "assign the same bed again: NoChange", R(same));
        var as2 = await Survivors.AssignBed(a.Id, bed2, SurvivorAssignmentOptions.Trusted);
        Check(as2.Ok && as2.After.Bed == bed2 && Survivors.BedOccupant(bed1) is null && Survivors.BedOccupant(bed2) == a.Id,
            "reassign clears the old bed and sets the new one", R(as2));
        var actor = await Survivors.AssignBed(a.Id, bed1, new SurvivorAssignmentOptions(new EntityId(9)));
        Check(!actor.Ok && actor.Completion == SurvivorCompletion.Refused && actor.Reason!.StartsWith("ActorNotInGame") &&
              Survivors.BedOccupant(bed2) == a.Id, "an actor not in game: Refused, nothing written", R(actor));
        try
        {
            await Survivors.AssignBed(a.Id, bed1, new SurvivorAssignmentOptions(EntityId.None));
            Fail("PlayerPermissions without an actor throws", "no exception");
        }
        catch (ArgumentException)
        {
            Pass("PlayerPermissions without an actor throws ArgumentException");
        }
        var notBed = await Survivors.AssignBed(a.Id, altar, SurvivorAssignmentOptions.Trusted);
        Check(!notBed.Ok && notBed.Reason!.StartsWith("NotABed"), "an altar is not a bed: Refused NotABed", R(notBed));
        var far = await Spawn.Entity("Prop_Furniture_Comfort3_T1_Bed", new WorldPosition(3806, 835, 1590));
        await Steps(5);
        var farR = await Survivors.AssignBed(a.Id, far, SurvivorAssignmentOptions.Trusted);
        Note($"far bed (86 m): {R(farR)}");
        Check(!farR.Ok && farR.Reason!.StartsWith("BedOutOfHomeRange") && Survivors.BedOccupant(bed2) == a.Id,
            "a bed beyond the bed radius: Refused BedOutOfHomeRange", R(farR));
        Spawn.Destroy(far);

        // ---- a second survivor displaces the first
        var b = Survivors.List().FirstOrDefault(s => !s.Unlocked && s.SummonTool.Value != 0 && s.Entity.IsNone && s.Id != a.Id);
        uint other = 0;
        if (b is null) Fail("a second locked survivor", "none");
        else
        {
            other = b.Id.Value;
            var sb = await Survivors.Summon(b.Id, baseId.Value, new WorldPosition(3812, 835, 1500));
            Note($"second summon ({b.DebugName}): {R(sb)}");
            Check(sb.Ok && sb.Effects.HasFlag(SurvivorEffects.KnowledgeWritten) && sb.Effects.HasFlag(SurvivorEffects.EntityCreated),
                "summon of a locked survivor recruits it first", R(sb));
            var disp = await Survivors.AssignBed(b.Id, bed2, SurvivorAssignmentOptions.Trusted);
            Note($"displace: {R(disp)}");
            Check(disp.Ok && disp.DisplacedNpc == move.After.Entity && Survivors.Find(a.Id)!.Bed.IsNone && Survivors.BedOccupant(bed2) == b.Id,
                "B takes A's bed: A displaced, both ends right", R(disp));
            var un = await Survivors.UnassignBed(b.Id, SurvivorAssignmentOptions.Trusted);
            Check(un.Ok && un.Completion == SurvivorCompletion.Done && un.After.Bed.IsNone && Survivors.BedOccupant(bed2) is null, "unassign clears both ends", R(un));
            var un2 = await Survivors.UnassignBed(b.Id, SurvivorAssignmentOptions.Trusted);
            Check(un2.Ok && un2.Completion == SurvivorCompletion.NoChange, "unassign again: NoChange", R(un2));
        }

        // ---- a game-origin change: the bed is destroyed
        var as3 = await Survivors.AssignBed(a.Id, bed1, SurvivorAssignmentOptions.Trusted);
        Check(as3.Ok && as3.After.Bed == bed1, "assign A to bed 1 again", R(as3));
        lock (_events) _events.Clear();
        Spawn.Destroy(bed1);
        var game = await WaitEvent<SurvivorAssignmentChangedEvent>(e => e.Survivor == a.Id && !e.ByKiln, 400);
        // the game drops the destroyed bed from the NPC; with a free bed in range (bed 2) its upkeep takes that one
        Check(game is not null && game.OldBed == bed1 && game.NewBed != bed1, "a destroyed bed: the game moves the NPC off it, seen as a game-origin change",
            game?.ToString() ?? "no event");

        // ---- keep a bed for the restart
        var keep = await Survivors.AssignBed(a.Id, bed2, SurvivorAssignmentOptions.Trusted);
        Check(keep.Ok && keep.After.Bed == bed2, "A keeps bed 2 for the restart", R(keep));
        var bpos = World.Position(bed2) ?? default;
        var unlocks = new List<uint>();
        foreach (var s in new[] { a.Id.Value, other })
            if (s != 0 && Knowledge.Npcs.FirstOrDefault(n => n.NpcId == s) is { } n) unlocks.Add(n.Unlock.Id);
        Directory.CreateDirectory(Context.DataDirectory);
        File.WriteAllText(StateFile, JsonSerializer.Serialize(new Saved(a.Id.Value, other, bpos.X, bpos.Y, bpos.Z, [.. unlocks])));
        var save = await Saves.Request();
        Note($"save requested: {save}");
        Note("run 1 over: stop the server and start it again for run 2");
    }

    private async Task Run2()
    {
        var s = JsonSerializer.Deserialize<Saved>(File.ReadAllText(StateFile))!;
        var id = new SurvivorId(s.Survivor);
        SurvivorInfo? a = null;
        for (var i = 0; i < 200 && a is not { Entity.IsNone: false, Bed.IsNone: false }; i++)
        {
            a = Survivors.Find(id);
            await Steps(5);
        }
        Note($"run 2: {a}");
        Check(a is { Unlocked: true, Entity.IsNone: false }, "restart: the survivor is recruited and in the world");
        Check(a is { Bed.IsNone: false } && World.Position(a.Bed) is { } p && Dist(p, new WorldPosition(s.BedX, s.BedY, s.BedZ)) < 0.5,
            "restart: its bed is the same bed (by position; new entity ids)", $"{a?.Bed}");
        Check(a is { Bed.IsNone: false } && Survivors.BedOccupant(a.Bed) == id, "restart: the bed side names it (both ends)");
        // put back
        if (a is not null)
        {
            var un = await Survivors.UnassignBed(id, SurvivorAssignmentOptions.Trusted);
            Check(un.Ok, "cleanup: unassign", R(un));
            if (!a.Bed.IsNone) Spawn.Destroy(a.Bed);
        }
        foreach (var n in new[] { s.Survivor, s.Other })
            if (n != 0 && Survivors.Find(new SurvivorId(n)) is { Entity.IsNone: false } x) Spawn.Destroy(x.Entity);
        foreach (var u in s.Unlocks) await Knowledge.World.Revoke(u);
        foreach (var e in Spawn.FindByName("Home_Base_FlameAltar", new WorldPosition(3800, 835, 1500), 5)) Spawn.Destroy(e);
        foreach (var e in Spawn.FindByName("Prop_Furniture_Comfort3_T1_Bed", new WorldPosition(3808, 835, 1504), 20)) Spawn.Destroy(e);
        File.Delete(StateFile);
        Note("run 2 over: the lab world is put back");
    }

    private static double Dist(WorldPosition a, WorldPosition b)
    {
        double dx = a.X - b.X, dy = a.Y - b.Y, dz = a.Z - b.Z;
        return Math.Sqrt(dx * dx + dy * dy + dz * dz);
    }
}
