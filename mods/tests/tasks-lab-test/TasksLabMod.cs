// SPDX-License-Identifier: MIT
// Kiln tasks and their world reads and writes on a dedicated server (the lab half of taking them out of experimental for
// 0.2.0-dev.30). Player-free but for a player character the mod spawns next to a saved chest (no client, no screen).
//
// First load in a process: the task basics (off the tick and back, OnTick, PostToTick, a fault, a cancel, Wait refused
// on the tick, a long-running task beside short ones, the 1024-task cap), six rounds of verified reads from a task
// against the tick's own reads, compare-exchange writes on a padding byte of the character (a no-op, one bit flipped
// and put back, a conflict, a write from the tick refused in frame, the committed-write events), four writers racing
// on that byte, and a load phase (resident readers and writers, bursts of short tasks) with the server step time
// measured before and during it. Then resident tasks stay running (one waiting for its cancel, one ignoring it for a
// while, one suspended, readers, pending hops) and the file ready-1 tells tasks-lab-driver to reload the mod.
// Second load: the basics again and the resident tasks again, for the server's exit (run.sh stops the lab).
// One line per check: "tasks-lab-test: PASS|FAIL ...", then "tasks-lab-test done: N PASS, M FAIL (load L)".
using System.Diagnostics;
using System.Runtime.InteropServices;
using Kiln;

namespace TasksLabTest;

public sealed class TasksLabMod : Mod
{
    private const string Me = "tasks-lab-test";
    private int _pass, _fail, _load;
    private EntityId _player, _chest;
    private int _exchanged;  // committed-write events of this mod on the character (tick thread)
    private readonly List<KilnTask> _resident = [];

    [StructLayout(LayoutKind.Sequential)]
    private struct Pos
    {
        public long X, Y, Z;
        public readonly WorldPosition World => new(X / 4294967296.0, Y / 4294967296.0, Z / 4294967296.0);
    }

    private void Info(string s) => Log.Info($"{Me}: INFO {s}");

    private bool Check(bool ok, string what)
    {
        if (ok) { Interlocked.Increment(ref _pass); Log.Info($"{Me}: PASS {what}"); }
        else { Interlocked.Increment(ref _fail); Log.Error($"{Me}: FAIL {what}"); }
        return ok;
    }

    private async Task Steps(int n)
    {
        for (var i = 0; i < n; i++) await Context.Scheduler.NextTick();
    }

    private string Data(string name) => Path.Combine(Context.DataDirectory, name);

    /// <summary>How many times the mod loaded in this process (1 on the first load): data file loads.txt holds "pid count".</summary>
    private int CountLoad()
    {
        Directory.CreateDirectory(Context.DataDirectory);
        var pid = Environment.ProcessId;
        var n = 1;
        if (File.Exists(Data("loads.txt")) && File.ReadAllText(Data("loads.txt")).Split(' ') is [var p, var c] && p == $"{pid}" && int.TryParse(c, out var k))
            n = k + 1;
        File.WriteAllText(Data("loads.txt"), $"{pid} {n}");
        return n;
    }

    private int Setting(string name, int fallback) =>
        File.Exists(Data(name)) && int.TryParse(File.ReadAllText(Data(name)).Trim(), out var v) ? v : fallback;

    public override void OnLoad()
    {
        if (!Client.IsDedicatedServer) return;
        _load = CountLoad();
        foreach (var f in Directory.GetFiles(Context.DataDirectory, $"ready-{_load}")) File.Delete(f);
        _ = Run().ContinueWith(t =>
        {
            if (t.Exception != null) Check(false, $"the test threw: {t.Exception.InnerException}");
            Log.Info($"{Me} done: {_pass} PASS, {_fail} FAIL (load {_load})");
        });
    }

    public override void OnUnload() => Info($"unloading (load {_load}) with {_resident.Count(t => t.State is KilnTaskState.Queued or KilnTaskState.Running)} resident task(s) not ended");

    private async Task Run()
    {
        for (var i = 0; i < 240 && !(Spawn.WorldRunning && Items.All.Count > 0); i++) await Steps(25);
        await Steps(25);
        Check(WorldConcurrent.IsAvailable, $"world.ecs_read is on: {Capabilities.Why(WorldConcurrent.Capability)}");
        Check(WorldConcurrent.CanWrite, $"world.ecs_write is on: {Capabilities.Why(WorldConcurrent.WriteCapability)}");
        await Basics();
        _chest = await Chest();
        if (_chest.IsNone) return;
        _player = await Character();
        if (_player.IsNone) return;
        using var sub = Events.On<ComponentExchangedEvent>(e =>
        {
            if (e.Mod == Me && e.Entity == _player) _exchanged++;
        });
        if (_load == 1)
        {
            var quick = File.Exists(Data("quick"));  // only the load phase (for bisecting it)
            if (!quick) for (var round = 1; round <= 6; round++) await Reads(round);
            var target = PickTarget();
            if (target is { } t)
            {
                if (!quick)
                {
                    await Writes(t);
                    await Race(t);
                }
                await Load(t);
            }
        }
        // load 2 is the driver's control: a reload of the same mod without tasks in flight
        if (_load != 2)
        {
            StartResident();
            var seconds = Setting("resident-seconds", 60);
            var gaps = await StepTimes(seconds);
            Check(gaps.Count > 0 && gaps.Max() < 1000 && _resident.All(t => t.State == KilnTaskState.Running),
                $"{seconds} s with the resident tasks running ({string.Join(", ", _resident.Select(t => $"{t.Name} {t.State}"))}): the server step stays smooth: {Stats(gaps)}");
        }
        File.WriteAllText(Data($"ready-{_load}"), $"{Environment.ProcessId}");
        Info($"READY {(_load < 4 ? "reload" : "exit")}: {_resident.Count} resident task(s) running");
    }

    // -------------------------------------------------------------------------------------------- basics ------

    private async Task Basics()
    {
        var t = Context.Tasks.Run("basics", async ctx =>
        {
            var off = !Game.IsTickThread;
            await ctx.ToTick();
            return (off, on: Game.IsTickThread);
        });
        var r = await t;
        Check(r.off && r.on && t.State == KilnTaskState.Completed, $"a task runs off the tick and its ToTick hop on it: off {r.off}, on {r.on}, {t.State}");

        var posted = 0;
        var t2 = Context.Tasks.Run("on-tick", async ctx =>
        {
            ctx.PostToTick(() => posted++);
            return await ctx.OnTick(() => Game.IsTickThread ? 7 : -1);
        });
        var v = await t2;
        await Steps(1);
        Check(v == 7 && posted == 1, $"OnTick returns the tick's value ({v}) and PostToTick ran once ({posted})");

        var boom = Context.Tasks.Run("boom", (Action<KilnTaskContext>)(_ => throw new InvalidOperationException($"{Me}: deliberate task fault")));
        Exception? caught = null;
        try { await boom; } catch (Exception e) { caught = e; }
        Check(boom.State == KilnTaskState.Faulted && caught is InvalidOperationException && boom.Fault?.Message.Contains("deliberate") == true && Mods.IsLoaded(Me),
            $"a throwing task faults alone: {boom.State}, {caught?.GetType().Name}, the mod still loaded {Mods.IsLoaded(Me)}");

        var unwound = 0;
        var c = Context.Tasks.Run("cancel-me", async ctx =>
        {
            try { await Task.Delay(Timeout.Infinite, ctx.Cancellation); }
            finally { Interlocked.Exchange(ref unwound, 1); }
        });
        await Steps(2);
        c.Cancel();
        Exception? cancelled = null;
        try { await c; } catch (Exception e) { cancelled = e; }
        Check(c.State == KilnTaskState.Cancelled && cancelled is OperationCanceledException && Volatile.Read(ref unwound) == 1,
            $"Cancel ends a waiting task: {c.State}, {cancelled?.GetType().Name}, its finally ran {unwound == 1}");

        var refused = false;
        try { c.Wait(); } catch (InvalidOperationException) { refused = true; }
        Check(refused, "KilnTask.Wait() on the tick is refused (InvalidOperationException)");

        var sw = Stopwatch.StartNew();
        var longOne = Context.Tasks.Run("long", ctx =>
        {
            while (sw.ElapsedMilliseconds < 1500 && !ctx.Cancellation.IsCancellationRequested) Thread.SpinWait(2000);
        }, new KilnTaskOptions(LongRunning: true));
        var shorts = Enumerable.Range(0, 8).Select(i => Context.Tasks.Run($"short-{i}", ctx => Task.FromResult(i * i))).ToList();
        var sum = 0;
        foreach (var s in shorts) sum += await s;
        var shortsAt = sw.ElapsedMilliseconds;
        await longOne;
        Check(sum == 140 && shortsAt < 1400 && longOne.State == KilnTaskState.Completed,
            $"short tasks finish beside a long-running one: all 8 by {shortsAt} ms, the long one after {sw.ElapsedMilliseconds} ms");

        var gate = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var held = new List<KilnTask>();
        KilnTaskQueueFullException? full = null;
        for (var i = 0; i < 1100 && full == null; i++)
        {
            try { held.Add(Context.Tasks.Run("held", ctx => gate.Task.WaitAsync(ctx.Cancellation))); }
            catch (KilnTaskQueueFullException e) { full = e; }
        }
        Check(full != null && held.Count is >= 1000 and <= 1024, $"the 1024-task cap refuses the next task: {held.Count} accepted, then {full?.GetType().Name}");
        gate.SetResult();
        var ended = 0;
        foreach (var h in held) { try { await h; ended++; } catch { } }
        Check(ended == held.Count && held.All(h => h.State == KilnTaskState.Completed), $"the {held.Count} held tasks complete once released ({ended})");
        var after = await Context.Tasks.Run("after-cap", ctx => Task.FromResult(1));
        Check(after == 1, "a task starts again after the cap's tasks ended");
    }

    // --------------------------------------------------------------------------------------- the world ------

    private async Task<EntityId> Chest()
    {
        for (var i = 0; i < 40; i++)
        {
            var found = Items.FindInventories(InventoryKinds.Containers)
                .Where(e => Items.Read(e) is { Initialized: true } && World.Position(e) != null)
                .OrderBy(e => e.Value).ToList();
            if (found.Count > 0)
            {
                Check(true, $"a saved container of the world: {found[0]} at {World.Position(found[0])}");
                return found[0];
            }
            await Steps(25);
        }
        Check(false, "a saved container in this world (make the lab with --save from a world that has a base)");
        return default;
    }

    private async Task<EntityId> Character()
    {
        if (File.Exists(Data("character.txt")) && long.TryParse(File.ReadAllText(Data("character.txt")), out var old) &&
            new EntityId((uint)old) is var known && World.Exists(known) && World.Has(known, "PlayerInput"))
        {
            Info($"the character of the last load is still there: {known}");
            return known;
        }
        var q = World.Position(_chest)!.Value;
        var at = new WorldPosition(q.X + 45, q.Y + 3, q.Z + 45);
        var p = await Spawn.Entity("1_Player", at, 0, new SpawnOptions { Tag = Me, Flags = SpawnFlags.Transient });
        for (var i = 0; i < 120 && !p.IsNone && Items.Read(p) is not { Initialized: true }; i++) await Steps(1);
        await Steps(50);  // let it land
        Check(!p.IsNone && World.Has(p, "CurrentTransform") && World.Has(p, "Health") && Items.Read(p) is { Initialized: true },
            $"a player character {p} spawned at {at}: CurrentTransform {World.Has(p, "CurrentTransform")}, Health {World.Has(p, "Health")}, an initialized inventory {Items.Read(p) is { Initialized: true }}");
        File.WriteAllText(Data("character.txt"), $"{p.Value}");
        return p;
    }

    // ------------------------------------------------------------------------------------------- reads ------

    private async Task Reads(int round)
    {
        var before = World.Position(_player);
        var chestAt = World.Position(_chest);
        var invLen = World.ReadBytes(_player, "Health")?.Length ?? -1;
        if (before is null || chestAt is null || invLen <= 0) { Check(false, $"round {round}: the tick's reads: {before} {chestAt} {invLen}"); return; }
        var player = _player;
        var chest = _chest;
        var r = await Context.Tasks.Run("reads", async ctx =>
        {
            var why = new List<string>();
            var open = WorldConcurrent.Open(player);
            if (!open.IsOk) return ($"open {open.Status}", 0, default(WorldPosition));
            var e = open.Value;
            var pos = WorldConcurrent.Read<Pos>(e, "CurrentTransform");
            var seen = pos.IsOk ? pos.Value.World : default;
            if (!pos.IsOk) why.Add($"CurrentTransform {pos.Status}");
            var inv = new byte[invLen];
            var copy = WorldConcurrent.ReadBytes(e, "Health", inv);
            if (copy.Status != ReadStatus.Ok) why.Add($"Health {copy.Status} (required {copy.Required})");
            var refs = new ConcurrentEntity[16];
            var query = new EntityQuery(WorldKind.Server, "CurrentTransform") { Center = chestAt.Value, Radius = 80 };
            ConcurrentQueryResult found = default;
            for (var attempt = 0; attempt < 8; attempt++)
            {
                found = WorldConcurrent.Query(query, refs);
                if (found.Status == ReadStatus.TooSmall) { refs = new ConcurrentEntity[found.Required + 16]; continue; }
                if (found.Status == ReadStatus.Changed) continue;
                break;
            }
            var ids = found.Status == ReadStatus.Ok ? refs.Take(found.Count).Select(x => x.Id).ToHashSet() : [];
            if (!ids.Contains(player) || !ids.Contains(chest)) why.Add($"query {found.Status}: {found.Count} entities, player in {ids.Contains(player)}, container in {ids.Contains(chest)}");
            var busy = 0;
            var quiet = WorldConcurrent.Read<Pos>(e, "CurrentTransform", ReadMode.FrameQuiet);
            while (quiet.Status == ReadStatus.Busy && busy < 50)
            {
                busy++;
                await ctx.NextGateWindow();
                quiet = WorldConcurrent.Read<Pos>(e, "CurrentTransform", ReadMode.FrameQuiet);
            }
            if (!quiet.IsOk) why.Add($"frame-quiet {quiet.Status} after {busy} busy");
            var many = WorldConcurrent.ReadMany(e, new ComponentSet("CurrentTransform", "Health"));
            if (many.Status != ReadStatus.Ok || many.StatusOf(0) != ReadStatus.Ok || many.StatusOf(1) != ReadStatus.Ok)
                why.Add($"ReadMany {many.Status} ({many.StatusOf(0)}, {many.StatusOf(1)})");
            if (WorldConcurrent.Validate(e) != ReadStatus.Ok) why.Add($"Validate {WorldConcurrent.Validate(e)}");
            if (!WorldConcurrent.Template(e).IsOk) why.Add($"Template {WorldConcurrent.Template(e).Status}");
            var absent = WorldConcurrent.Open(new EntityId(0x7ffffff0));
            if (absent.Status != ReadStatus.Absent) why.Add($"an id that does not exist: {absent.Status}");
            await ctx.ToTick();
            var inFrame = WorldConcurrent.Read<Pos>(e, "CurrentTransform", ReadMode.FrameQuiet).Status;
            if (inFrame != ReadStatus.Busy) why.Add($"frame-quiet on the tick: {inFrame} (Busy expected)");
            return (string.Join("; ", why), busy, seen);
        });
        var after = World.Position(_player);
        var d = Math.Min(r.Item3.DistanceTo(before.Value), after is { } a ? r.Item3.DistanceTo(a) : double.MaxValue);
        Check(r.Item1.Length == 0 && d <= 0.5,
            $"round {round}: the task's reads agree with the tick's: position within {d:0.###} m, Health {invLen} bytes, the bounded query, frame-quiet after {r.Item2} busy look(s), ReadMany, Validate, Template, an absent id, Busy on the tick{(r.Item1.Length > 0 ? ": " + r.Item1 : "")}");
        await Steps(25 * 3);
    }

    // ------------------------------------------------------------------------------------------ writes ------

    private sealed record Target(string Component, int Size, int Pad);

    private static List<int> Padding(GameType t)
    {
        var covered = new bool[t.Size];
        foreach (var f in t.AllFields)
            for (var i = Math.Max(0, f.Offset); i < Math.Min(t.Size, f.Offset + f.Type.Size); i++) covered[i] = true;
        return [.. Enumerable.Range(0, t.Size).Where(i => !covered[i])];
    }

    private Target? PickTarget()
    {
        var writable = new List<string>();
        Target? pick = null;
        foreach (var t in GameTypes.Components.OrderBy(t => t.Name, StringComparer.Ordinal))
        {
            if (!World.Has(_player, t.Name)) continue;
            var policy = WorldConcurrent.WritePolicy(t.Name);
            if (!policy.IsWritable) continue;
            writable.Add(t.Name);
            if (pick == null && policy.Size == t.Size && t.FieldCount > 0 && Padding(t) is [var pad, ..]) pick = new(t.Name, t.Size, pad);
        }
        Info($"writable from a task on the character: {writable.Count}: {string.Join(", ", writable)}");
        Check(pick != null, $"a writable, untracked component of the character with a padding byte: {pick}");
        return pick;
    }

    /// <summary>One masked compare-exchange of the padding byte from <paramref name="from"/> to <paramref name="to"/>,
    /// retried on Conflict (another byte of the component moved), Busy and Unreadable. In a task.</summary>
    private static async Task<(ExchangeBytesResult Result, int Tries)> SetPad(KilnTaskContext ctx, ConcurrentEntity e, Target t, Func<byte, byte> change)
    {
        var cur = new byte[t.Size];
        var desired = new byte[t.Size];
        var mask = new byte[t.Size];
        mask[t.Pad] = 0xff;
        for (var tries = 1; tries <= 2000; tries++)
        {
            ctx.Cancellation.ThrowIfCancellationRequested();
            var read = WorldConcurrent.ReadBytes(e, t.Component, cur);
            if (read.Status != ReadStatus.Ok) { await ctx.NextGateWindow(); continue; }
            cur.CopyTo(desired, 0);
            desired[t.Pad] = change(cur[t.Pad]);
            var r = WorldConcurrent.TryCompareExchangeBytes(e, t.Component, cur, desired, mask);
            switch (r.Status)
            {
                case ExchangeStatus.Conflict:
                    continue;
                case ExchangeStatus.Busy or ExchangeStatus.Unreadable:
                    await ctx.NextGateWindow();
                    continue;
                default:
                    return (r, tries);
            }
        }
        return (default, 2000);
    }

    private static async Task<byte> ReadPad(KilnTaskContext ctx, ConcurrentEntity e, Target t)
    {
        var cur = new byte[t.Size];
        for (var i = 0; i < 50; i++)
        {
            if (WorldConcurrent.ReadBytes(e, t.Component, cur).Status == ReadStatus.Ok) return cur[t.Pad];
            await ctx.NextGateWindow();
        }
        throw new InvalidOperationException($"{t.Component} could not be read in 50 steps");
    }

    private async Task Writes(Target t)
    {
        var player = _player;
        var events0 = _exchanged;
        var r = await Context.Tasks.Run("writes", async ctx =>
        {
            var why = new List<string>();
            var e = WorldConcurrent.Open(player).Value;
            var orig = await ReadPad(ctx, e, t);
            var noop = await SetPad(ctx, e, t, b => b);
            if (noop.Result.Status != ExchangeStatus.Ok || (noop.Result.Flags & ExchangeFlags.NoOp) == 0) why.Add($"no-op: {noop.Result}");
            var flip = await SetPad(ctx, e, t, b => (byte)(b ^ 1));
            if (flip.Result.Status != ExchangeStatus.Ok || (flip.Result.Flags & ExchangeFlags.Written) == 0) why.Add($"flip: {flip.Result}");
            var flipped = await ReadPad(ctx, e, t);
            if (flipped != (byte)(orig ^ 1)) why.Add($"read back after the flip: {flipped}, {orig ^ 1} expected");

            // a stale expectation: the padding byte as it was before the flip
            var cur = new byte[t.Size];
            var seen = new byte[t.Size];
            ExchangeBytesResult conflict = default;
            for (var i = 0; i < 50; i++)
            {
                if (WorldConcurrent.ReadBytes(e, t.Component, cur).Status != ReadStatus.Ok) { await ctx.NextGateWindow(); continue; }
                var expected = (byte[])cur.Clone();
                expected[t.Pad] = orig;
                conflict = WorldConcurrent.TryCompareExchangeBytes(e, t.Component, expected, expected, default, seen);
                if (conflict.Status is ExchangeStatus.Busy or ExchangeStatus.Unreadable) { await ctx.NextGateWindow(); continue; }
                break;
            }
            if (conflict.Status != ExchangeStatus.Conflict || seen[t.Pad] != (byte)(orig ^ 1)) why.Add($"a stale expectation: {conflict}, saw {seen[t.Pad]}");

            // from the tick: in frame
            var buf = (byte[])cur.Clone();
            var onTick = await ctx.OnTick(() =>
            {
                var x = WorldConcurrent.TryCompareExchangeBytes(e, t.Component, buf, buf);
                return (x.Status, x.Reason);
            });
            if (onTick.Status != ExchangeStatus.Busy || onTick.Reason != WriteReason.InFrame) why.Add($"from the tick: {onTick.Status} ({onTick.Reason})");

            var back = await SetPad(ctx, e, t, _ => orig);
            var final = await ReadPad(ctx, e, t);
            if (back.Result.Status != ExchangeStatus.Ok || final != orig) why.Add($"put back: {back.Result}, {final} ({orig} expected)");
            return (string.Join("; ", why), orig, flip.Tries + back.Tries);
        });
        await Steps(3);
        var events = _exchanged - events0;
        Check(r.Item1.Length == 0 && events == 2,
            $"compare-exchange on {t.Component} byte {t.Pad} (padding, was {r.orig}): a no-op, a bit flipped and read back, a stale expectation answered Conflict, the tick answered Busy (InFrame), the bit put back ({r.Item3} tries); {events} committed-write events (2 expected){(r.Item1.Length > 0 ? ": " + r.Item1 : "")}");
    }

    private async Task Race(Target t)
    {
        const int writers = 4, each = 250;
        var player = _player;
        var events0 = _exchanged;
        var orig = await Context.Tasks.Run("race-orig", ctx => ReadPad(ctx, WorldConcurrent.Open(player).Value, t));
        var sw = Stopwatch.StartNew();
        var tasks = Enumerable.Range(0, writers).Select(i => Context.Tasks.Run($"race-{i}", async ctx =>
        {
            var e = WorldConcurrent.Open(player).Value;
            int ok = 0, tries = 0, other = 0;
            while (ok < each && other == 0)
            {
                var r = await SetPad(ctx, e, t, b => (byte)(b + 1));
                tries += r.Tries;
                if (r.Result.Status == ExchangeStatus.Ok && (r.Result.Flags & ExchangeFlags.Written) != 0) ok++;
                else other++;
            }
            return (ok, tries, other);
        })).ToList();
        var results = new List<(int ok, int tries, int other)>();
        foreach (var x in tasks) results.Add(await x);
        var elapsed = sw.ElapsedMilliseconds;
        var final = await Context.Tasks.Run("race-final", ctx => ReadPad(ctx, WorldConcurrent.Open(player).Value, t));
        var total = results.Sum(x => x.ok);
        await Steps(3);
        var events = _exchanged - events0;
        Check(total == writers * each && results.All(x => x.other == 0) && (byte)(orig + total) == final && events == total,
            $"{writers} tasks racing on one byte: {total} increments in {elapsed} ms ({results.Sum(x => x.tries)} tries), the byte went {orig} -> {final} " +
            $"({(byte)(orig + total)} expected), {events} committed-write events");
        var restored = await Context.Tasks.Run("race-restore", async ctx =>
        {
            var e = WorldConcurrent.Open(player).Value;
            await SetPad(ctx, e, t, _ => orig);
            return await ReadPad(ctx, e, t);
        });
        Check(restored == orig, $"the padding byte put back after the race: {restored}");
    }

    // -------------------------------------------------------------------------------------------- load ------

    private async Task<List<double>> StepTimes(int seconds, Func<int, Task>? perStep = null)
    {
        var gaps = new List<double>();
        var sw = Stopwatch.StartNew();
        var last = sw.Elapsed.TotalMilliseconds;
        for (var step = 0; sw.Elapsed.TotalSeconds < seconds; step++)
        {
            await Context.Scheduler.NextTick();
            var now = sw.Elapsed.TotalMilliseconds;
            gaps.Add(now - last);
            last = now;
            if (perStep != null) await perStep(step);
        }
        return gaps;
    }

    private static string Stats(List<double> g)
    {
        if (g.Count == 0) return "no steps";
        var s = g.OrderBy(x => x).ToList();
        double P(double q) => s[Math.Min(s.Count - 1, (int)(q * s.Count))];
        return $"{g.Count} steps, p50 {P(0.5):0.0} ms, p99 {P(0.99):0.0} ms, max {s[^1]:0.0} ms";
    }

    private async Task Load(Target t)
    {
        var seconds = Setting("load-seconds", 90);
        // data file "load-parts": which of readers, writers, bursts run (one per line); all three without it
        var parts = File.Exists(Data("load-parts")) ? File.ReadAllLines(Data("load-parts")).Select(l => l.Trim()).ToHashSet() : ["readers", "writers", "bursts"];
        Info($"load parts: {string.Join(", ", parts.Order())}");
        var baseline = await StepTimes(Math.Max(10, seconds / 4));
        Info($"step time without tasks: {Stats(baseline)}");

        using var stop = new CancellationTokenSource();
        var player = _player;
        var chestAt = World.Position(_chest)!.Value;
        long reads = 0, readOther = 0, writes = 0, writeOther = 0;
        var odd = new System.Collections.Concurrent.ConcurrentDictionary<string, int>();
        var readers = Enumerable.Range(0, parts.Contains("readers") ? 2 : 0).Select(i => Context.Tasks.Run($"load-reader-{i}", async ctx =>
        {
            var e = WorldConcurrent.Open(player).Value;
            var refs = new ConcurrentEntity[256];
            var set = new ComponentSet("CurrentTransform", "Health");
            var query = new EntityQuery(WorldKind.Server, "CurrentTransform") { Center = chestAt, Radius = 80 };
            while (!stop.IsCancellationRequested)
            {
                var q = WorldConcurrent.Query(query, refs);
                var m = WorldConcurrent.ReadMany(e, set);
                var f = WorldConcurrent.Read<Pos>(e, "CurrentTransform", ReadMode.FrameQuiet);
                foreach (var s in new[] { q.Status, m.Status, f.Status })
                {
                    if (s is ReadStatus.Ok) Interlocked.Increment(ref reads);
                    else if (s is ReadStatus.Busy or ReadStatus.Changed or ReadStatus.TooSmall or ReadStatus.Unreadable) Interlocked.Increment(ref readOther);
                    else odd.AddOrUpdate($"read {s}", 1, (_, n) => n + 1);
                }
                if (f.Status == ReadStatus.Busy) await ctx.NextGateWindow();
            }
        }, new KilnTaskOptions(LongRunning: true))).ToList();
        var writersT = Enumerable.Range(0, parts.Contains("writers") ? 2 : 0).Select(i => Context.Tasks.Run($"load-writer-{i}", async ctx =>
        {
            var e = WorldConcurrent.Open(player).Value;
            var n = 0;
            while (!stop.IsCancellationRequested)
            {
                // flip and put back: the byte is the same after every pair, whichever writer's pair interleaves
                var a = await SetPad(ctx, e, t, b => (byte)(b ^ (1 << (i + 1))));
                var b2 = await SetPad(ctx, e, t, b => (byte)(b ^ (1 << (i + 1))));
                foreach (var r in new[] { a.Result, b2.Result })
                    if (r.Status == ExchangeStatus.Ok) Interlocked.Increment(ref writes);
                    else { Interlocked.Increment(ref writeOther); odd.AddOrUpdate($"write {r.Status}", 1, (_, k) => k + 1); }
                if (++n % 10 == 0) await ctx.NextGateWindow();
            }
        })).ToList();

        int started = 0, completed = 0, appliedOnTick = 0, faulted = 0, refusedFull = 0;
        var bursts = new List<KilnTask<long>>();
        var startMs = new List<double>();
        // bursts: a spawner task starts 200 short tasks about once a second (Run is any thread), and the tick starts 10
        // every 25 steps, timed: what starting a task costs the tick
        var burstLock = new object();
        var spawner = parts.Contains("bursts") ? Context.Tasks.Run("load-spawner", async ctx =>
        {
            while (!stop.IsCancellationRequested)
            {
                StartBurst(200);
                try { await Task.Delay(1000, stop.Token); } catch (OperationCanceledException) { }
            }
        }) : null;
        var loaded = await StepTimes(seconds, step =>
        {
            if (step % 25 != 0 || !parts.Contains("bursts")) return Task.CompletedTask;
            var started0 = Stopwatch.GetTimestamp();
            StartBurst(10);
            startMs.Add(Stopwatch.GetElapsedTime(started0).TotalMilliseconds);
            return Task.CompletedTask;
        });
        void StartBurst(int count)
        {
            for (var k = 0; k < count; k++)
            {
                try
                {
                    var task = Context.Tasks.Run("burst", async ctx =>
                    {
                        long s = 0;
                        for (var j = 0; j < 20_000; j++) s += j ^ k;
                        ctx.PostToTick(() => appliedOnTick++);
                        await ctx.ToTick();
                        return s;
                    });
                    lock (burstLock) { bursts.Add(task); started++; }
                }
                catch (KilnTaskQueueFullException) { Interlocked.Increment(ref refusedFull); }
            }
        }
        stop.Cancel();
        if (spawner != null) { try { await spawner; } catch { } }
        if (startMs.Count > 0)
            Check(startMs.Average() / 10 < 1.0,
                $"starting a task on the tick costs {startMs.Average() / 10 * 1000:0} us on average (10 at a time: p50 {startMs.Order().ElementAt(startMs.Count / 2):0.0} ms, max {startMs.Max():0.0} ms)");
        foreach (var b in bursts.ToList()) { try { await b; completed++; } catch { faulted++; } }
        foreach (var x in readers.Concat(writersT)) { try { await x; } catch (Exception e) { odd.AddOrUpdate($"task {e.GetType().Name}: {e.Message}", 1, (_, k) => k + 1); } }
        await Steps(3);
        var pad = await Context.Tasks.Run("load-pad", ctx => ReadPad(ctx, WorldConcurrent.Open(player).Value, t));
        var orig = await Context.Tasks.Run("load-orig", ctx => Task.FromResult(pad));
        Info($"step time with tasks: {Stats(loaded)}");
        Check(faulted == 0 && completed == started && appliedOnTick == started,
            $"load: {started} burst tasks in {seconds} s, all completed ({completed}) and every PostToTick applied ({appliedOnTick}); {refusedFull} refused at the cap");
        Check(odd.IsEmpty && (reads > 0) == parts.Contains("readers") && (writes > 0) == parts.Contains("writers"),
            $"load: {reads} reads Ok, {readOther} busy/changed/unreadable, {writes} writes Ok, {writeOther} not; nothing else{(odd.IsEmpty ? "" : ": " + string.Join(", ", odd.Select(kv => $"{kv.Key} x{kv.Value}")))}");
        // the readers here never yield unless a frame-quiet read is busy, the writers take the engine's gate many times a
        // step, and a spawner starts 200 tasks a second: far more than a mod should. The bound is that the server keeps
        // stepping (no step over a second) at no worse than half again its median step without tasks.
        double P50(List<double> g) => g.OrderBy(x => x).ElementAt(g.Count / 2);
        Check(loaded.Count > 0 && loaded.Max() < 1000 && P50(loaded) <= P50(baseline) * 1.5,
            $"load: the server keeps stepping: without tasks {Stats(baseline)}; with them {Stats(loaded)}");
        _ = orig;
    }

    // ------------------------------------------------------------------------------------------ resident ------

    private void StartResident()
    {
        // data file "resident": the names to start (one per line), for bisecting; all of them without it
        var only = File.Exists(Data("resident")) ? File.ReadAllLines(Data("resident")).Select(l => l.Trim()).Where(l => l.Length > 0).ToHashSet() : null;
        StartAll(name => only == null || only.Contains(name));
    }

    private void StartAll(Func<string, bool> want)
    {
        var player = _player;
        var dir = Context.DataDirectory;
        var load = _load;
        if (want("res-waits")) _resident.Add(Context.Tasks.Run("res-waits", async ctx =>
        {
            try { await Task.Delay(Timeout.Infinite, ctx.Cancellation); }
            finally { File.WriteAllText(Path.Combine(dir, $"unwound-{load}"), $"{Environment.ProcessId}"); }
        }));
        if (want("res-ignores")) _resident.Add(Context.Tasks.Run("res-ignores", ctx =>
        {
            // ignores its cancel for 3 s after it comes: the unload's join says so once, and the load context waits
            var since = Stopwatch.StartNew();
            var cancelledAt = -1L;
            while (cancelledAt < 0 || since.ElapsedMilliseconds - cancelledAt < 3000)
            {
                if (cancelledAt < 0 && ctx.Cancellation.IsCancellationRequested) cancelledAt = since.ElapsedMilliseconds;
                Thread.Sleep(20);
            }
            File.WriteAllText(Path.Combine(dir, $"ignored-{load}"), $"{since.ElapsedMilliseconds - cancelledAt}");
        }, new KilnTaskOptions(LongRunning: true)));
        var never = new TaskCompletionSource();
        if (want("res-suspended")) _resident.Add(Context.Tasks.Run("res-suspended", ctx => never.Task.WaitAsync(ctx.Cancellation)));
        if (want("res-reader")) _resident.Add(Context.Tasks.Run("res-reader", async ctx =>
        {
            var e = WorldConcurrent.Open(player).Value;
            while (!ctx.Cancellation.IsCancellationRequested)
            {
                WorldConcurrent.Read<Pos>(e, "CurrentTransform");
                await ctx.NextGateWindow();
            }
        }));
        if (want("res-hops")) _resident.Add(Context.Tasks.Run("res-hops", async ctx =>
        {
            while (!ctx.Cancellation.IsCancellationRequested)
            {
                ctx.PostToTick(() => { });
                await ctx.ToTick();
                await Task.Delay(5, ctx.Cancellation);
            }
        }));
    }
}
