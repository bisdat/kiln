// SPDX-License-Identifier: MIT
// [ImpactNative] lab test (package 06c S4), player-free. The natives below are the mod's whole glue: no RegisterSync,
// no DeclareTyped, no hand-written thunk. Rows:
//  - before the generated natives registered (OnLoad), a program passing float32 to Twice's int parameter is refused:
//    the typed declaration came from the impact/natives.json the build wrote, read by kiln.abilities at start;
//  - the generated natives registered at the mod's first tick, typed and synchronous;
//  - the cooked program (assets/twice.bin, kiln.assets) calling Twice answers in its start step;
//  - a loop over 5 with Sum5 finishes in 1 step; a round trip of sint32/float32/bool/Time/EntityId/float3.
using System.Diagnostics;
using System.Numerics;
using System.Text;
using Kiln;

namespace NativesGenTest;

public static class GenNatives
{
    internal static int TwiceCalls, SumCalls, RoundCalls;
    internal static bool RoundRead, CheckRead;

    [ImpactNative("Twice")]
    public static bool Twice(out int result, int value)
    {
        TwiceCalls++;
        result = value * 2;
        return true;
    }

    // Sum += I; I += 1; answers true while I < 5 (the program loops on it)
    [ImpactNative("Sum5")]
    public static bool Sum5(ref int sum, ref int i)
    {
        SumCalls++;
        sum += i;
        i++;
        return i < 5;
    }

    [ImpactNative("Round")]
    public static byte Round(in NativeContext ctx, ref int s, ref float f, ref bool b, ref TimeSpan t, ref EntityId e, ref Vector3 v)
    {
        RoundCalls++;
        RoundRead = s == 7 && f == 1.5f && b && t == TimeSpan.FromTicks(10) && e.Value == 42 && v == new Vector3(1, 2, 3) && !ctx.Self.IsNone;
        s = -7;
        f = 2.5f;
        b = false;
        t = TimeSpan.FromSeconds(2);
        e = new EntityId(43);
        v = new Vector3(4, 5, 6);
        return 1;
    }

    // review round 2: two writable TimeSpan arguments naming one variable share one copy (2 s + 1 s + 1 s = 4 s)
    [ImpactNative("Alias")]
    public static void Alias(ref TimeSpan a, ref TimeSpan b)
    {
        a += TimeSpan.FromSeconds(1);
        b += TimeSpan.FromSeconds(1);
    }

    // review round 2: a generated method that throws: its changed TimeSpan is written back, the call answers 0x11
    internal static int ThrowCalls;
    [ImpactNative("Throws")]
    public static bool Throws(ref TimeSpan t)
    {
        ThrowCalls++;
        t = TimeSpan.FromSeconds(5);
        throw new InvalidOperationException("natives-gen-test: Throws throws on purpose");
    }

    // S5: the ECS bridge. Hurt(target): Health of target through impact_simulation's own accessors, -5 then -7 in one
    // call (one ComponentWrittenEvent), the instance's Root, and a type the program does not declare (no access)
    internal static int HurtCalls, HurtBefore = -1;
    internal static bool HurtRead, HurtWrite1, HurtWrite2, UndeclaredRead = true;
    internal static EntityId HurtRoot;
    internal static int HurtThread;
    [ImpactNative("Hurt")]
    public static bool Hurt(in NativeContext ctx, EntityId target)
    {
        HurtCalls++;
        HurtThread = Environment.CurrentManagedThreadId;
        HurtRoot = ctx.Root;
        UndeclaredRead = ctx.TryRead<Ecs.ScaleComponent>(target, out _);
        HurtRead = ctx.TryRead<Ecs.Health>(target, out var h);
        if (!HurtRead) return false;
        HurtBefore = h.Values[0];
        h.Values[0] = HurtBefore - 5;
        HurtWrite1 = ctx.TryWrite(target, in h);
        h.Values[0] = HurtBefore - 7;
        HurtWrite2 = ctx.TryWrite(target, in h);
        return true;
    }

    // S7: a saved prop's CurrentTransform (a wall torch of the base; the save tracks the component) 1/16 m up through the
    // bridge: the save keeps it across a restart. Position y is fixed point 32.32 at bytes 8..16.
    internal static long NudgeAfter = long.MinValue;
    [ImpactNative("Nudge")]
    public static bool Nudge(in NativeContext ctx, EntityId target)
    {
        if (!ctx.TryRead<Ecs.CurrentTransform>(target, out var c)) return false;
        var bytes = System.Runtime.InteropServices.MemoryMarshal.AsBytes(new Span<Ecs.CurrentTransform>(ref c));
        System.Buffers.Binary.BinaryPrimitives.WriteInt64LittleEndian(bytes[8..], System.Buffers.Binary.BinaryPrimitives.ReadInt64LittleEndian(bytes[8..]) + (1L << 28));
        if (!ctx.TryWrite(target, in c) || !ctx.TryRead<Ecs.CurrentTransform>(target, out var a)) return false;
        NudgeAfter = YOf(a);
        return true;
    }

    internal static long YOf(in Ecs.CurrentTransform t) =>
        System.Buffers.Binary.BinaryPrimitives.ReadInt64LittleEndian(System.Runtime.InteropServices.MemoryMarshal.AsBytes(
            new ReadOnlySpan<Ecs.CurrentTransform>(in t))[8..]);

    [ImpactNative("Check")]
    public static bool Check(int s, float f, bool b, TimeSpan t, EntityId e, Vector3 v)
    {
        CheckRead = s == -7 && f == 2.5f && !b && t == TimeSpan.FromSeconds(2) && e.Value == 43 && v == new Vector3(4, 5, 6);
        return CheckRead;
    }
}

public sealed class NativesGenTestMod : Mod
{
    private static readonly Guid TwiceProgram = new("06c0a5e0-0000-4000-8000-000000000001");  // assets/twice.bin
    private const uint Sint32 = 0x9859f31e, Float32 = 0xe89f7410, Bool = 0xc894953d, Time = 0xdfe4e404, EntityIdType = 0x737c8f86, Float3 = 0x3d1f8582;
    private int _pass, _fail;
    private string? _onLoadRefusal;
    private bool _registeredAtOnLoad;
    private readonly List<ImpactSignalEvent> _signals = new();
    private readonly List<ImpactStartedEvent> _started = new();
    private readonly List<EntityId> _spawned = new();
    private static readonly Guid LateProgram = new("06c0a5e0-0000-4000-8000-000000000004");  // registered after the world
    private NativeAttachment? _onCooked, _onLate;
    private NativeAttachmentState _cookedAtLoad, _lateAtLoad;
    private long _cookedCalls, _lateCalls;

    public override void OnLoad()
    {
        // review fix 4 and P1 item 3: attachments made before the world, to the cooked program (kiln.assets puts it in when
        // the world's programs are) and to one this mod registers later; their state says where each stands
        var a = Abilities.Natives.Attach(Abilities.Programs.IdOf(TwiceProgram), "getSelf", NativeStage.Before, (ref NativeCall c) => Interlocked.Increment(ref _cookedCalls));
        var b = Abilities.Natives.Attach(Abilities.Programs.IdOf(LateProgram), "getSelf", NativeStage.Before, (ref NativeCall c) => Interlocked.Increment(ref _lateCalls));
        _onCooked = a.Handle;
        _onLate = b.Handle;
        _cookedAtLoad = _onCooked?.State ?? NativeAttachmentState.Gone;
        _lateAtLoad = _onLate?.State ?? NativeAttachmentState.Gone;
        // OnLoad runs before the mod's first tick, where Kiln registers its generated natives: only impact/natives.json
        // (read when kiln.abilities started) knows Twice's types now
        _registeredAtOnLoad = Natives.Twice.Registration is not null;
        try
        {
            Abilities.Programs.Register(Program(new Guid("06c0a5e0-0000-4000-8000-0000000000f1"),
                [11, 0xFFFF0001, 11, 0xFFFF0000, 19, 0x06c0a5f1, Natives.Twice.HashFor(Context.Info.Id), 27, 30],
                [("Out::value", Sint32, 4, new byte[4]), ("F::value", Float32, 4, BitConverter.GetBytes(1.5f))]));
            _onLoadRefusal = "accepted";
        }
        catch (ArgumentException e) { _onLoadRefusal = e.Message; }
        if (Client.IsDedicatedServer) _ = Run();
    }

    [Subscribe] private void OnSignal(ImpactSignalEvent e) => _signals.Add(e);
    [Subscribe] private void OnStarted(ImpactStartedEvent e) => _started.Add(e);

    private async Task Steps(int n) { for (var i = 0; i < n; i++) await Context.Scheduler.NextTick(); }

    private async Task Run()
    {
        try
        {
            Check(!_registeredAtOnLoad && _onLoadRefusal?.Contains("E_ARGTYPE") == true,
                $"before its natives registered, a program passing float32 to Twice's int parameter is refused from the build's impact/natives.json: {_onLoadRefusal}");
            var file = Path.Combine(Context.Info.Directory, "impact", "natives.json");
            var json = File.Exists(file) ? File.ReadAllText(file) : "";
            Check(new[] { "Twice", "Sum5", "Round", "Check" }.All(n => json.Contains($"\"natives-gen-test.{n}\"")) && json.Contains("\"sync\": true"),
                $"the build wrote impact/natives.json with the four generated typed entries ({json.Length} bytes)");
            var sw = Stopwatch.StartNew();
            while (!Spawn.WorldRunning && sw.Elapsed.TotalSeconds < 240) await Steps(1);
            await Steps(30);
            while ((Abilities.Status?.ProgramsTotal ?? 0) < 571 && sw.Elapsed.TotalSeconds < 300) await Steps(10);
            var refs = new[] { Natives.Twice, Natives.Sum5, Natives.Round, Natives.Check };
            var kinds = refs.Select(r => Abilities.Natives.All.FirstOrDefault(n => n.Name == r.FullName)?.Kind).ToList();
            Check(refs.All(r => r.Registration is { Ok: true }) && kinds.All(k => k == ImpactNativeKind.Sync),
                $"the generated natives registered at the mod's first tick, synchronous: {string.Join(", ", refs.Select(r => $"{r.FullName} {r.Registration}"))}");
            var anchor = Spawn.FindByName("FlameAltar", max: 1).FirstOrDefault();
            var at = !anchor.IsNone && Spawn.Inspect(anchor) is { } inf ? inf.Position : new WorldPosition(0, 0, 0);
            WorldPosition P(double dx) => new(at.X + 50 + dx, at.Y + 0.5, at.Z + 30);

            // the cooked program (kiln.assets): Twice($out, $in = 21); kiln.Signal($out, 0x6E)
            var cooked = Abilities.Programs.Info(Abilities.Programs.IdOf(TwiceProgram));
            _signals.Clear();
            _started.Clear();
            var t0 = GenNatives.TwiceCalls;
            var e1 = await Abilities.RunProgram(TwiceProgram, P(0));
            _spawned.Add(e1);
            await Steps(6);
            var st = _started.FirstOrDefault(s => s.Impact.Self == e1);
            var sig = _signals.FirstOrDefault(s => s.Impact.Self == e1);
            Check(cooked is { FromKiln: true } && sig is { Tag: 0x6E, AsInt: 42 } && st is not null && sig.Step == st.Step && GenNatives.TwiceCalls - t0 == 1,
                $"the cooked program calling the generated Twice answers in its start step: signal {sig?.AsInt} tag 0x{sig?.Tag:x} at step {sig?.Step}, started {st?.Step} ({cooked})");
            Check(_cookedAtLoad == NativeAttachmentState.Pending && _onCooked?.State == NativeAttachmentState.Active && Interlocked.Read(ref _cookedCalls) == 1,
                $"an attachment made before the world to the cooked program: {_cookedAtLoad} at load, {_onCooked?.State} once the world's programs are in, " +
                $"{Interlocked.Read(ref _cookedCalls)} call when it ran");

            // an attachment to a program that is registered only now: off at the readiness check, active after it went in
            var lateBefore = _onLate?.State;
            var statusBefore = Abilities.Status;
            Abilities.Programs.Register(Program(LateProgram, [11, 0xFFFF0000, 19, 0x06c0a531, 0x01d09759, 27, 30],
                [("Self::value", EntityIdType, 4, new byte[4])]));
            await Steps(2);
            var lateAfter = _onLate?.State;
            var statusAfter = Abilities.Status;
            var e4 = await Abilities.RunProgram(LateProgram, P(9));
            _spawned.Add(e4);
            await Steps(4);
            Check(_lateAtLoad == NativeAttachmentState.Pending && lateBefore == NativeAttachmentState.Off && lateAfter == NativeAttachmentState.Active &&
                  Interlocked.Read(ref _lateCalls) == 1,
                $"an attachment to a program registered after the readiness check: {_lateAtLoad} at load, {lateBefore} at readiness, {lateAfter} " +
                $"once the program went in, {Interlocked.Read(ref _lateCalls)} call when it ran");
            // this mod's two attachments are the lab's only ones that name a program (run it alone)
            Check(statusBefore is { AttachmentsOff: 1, AttachmentsPending: 0 } && statusAfter is { AttachmentsOff: 0, AttachmentsPending: 0 } &&
                  statusAfter.AttachmentsActive == statusBefore.AttachmentsActive + 1,
                $"Abilities.Status counts the attachments' states: off {statusBefore?.AttachmentsOff} -> {statusAfter?.AttachmentsOff}, " +
                $"active {statusBefore?.AttachmentsActive} -> {statusAfter?.AttachmentsActive}, pending {statusAfter?.AttachmentsPending}");

            // a loop over 5 with Sum5, all in the start step
            const uint hSum = 0xFFFF0000, hI = 0xFFFF0001;
            var loopGuid = new Guid("06c0a5e0-0000-4000-8000-000000000002");
            Abilities.Programs.Register(Program(loopGuid,
                [11, hI, 11, hSum, 19, 0x06c0a511, Natives.Sum5.HashFor(Context.Info.Id), 9, 0, 11, 0x6F, 11, hSum, 19, 0x06c0a512, Fnv("kiln.Signal"), 27, 30],
                [("Sum::value", Sint32, 4, new byte[4]), ("I::value", Sint32, 4, new byte[4])]));
            _signals.Clear();
            _started.Clear();
            var s0 = GenNatives.SumCalls;
            var e2 = await Abilities.RunProgram(loopGuid, P(3));
            _spawned.Add(e2);
            await Steps(6);
            st = _started.FirstOrDefault(s => s.Impact.Self == e2);
            sig = _signals.FirstOrDefault(s => s.Impact.Self == e2);
            Check(GenNatives.SumCalls - s0 == 5 && sig is { AsInt: 10 } && st is not null && sig.Step == st.Step,
                $"a loop over 5 targets with a generated native finishes in 1 step: {GenNatives.SumCalls - s0} call(s), sum {sig?.AsInt}, started {st?.Step}, signal {sig?.Step}");

            // aliased TimeSpan refs: Alias($t, $t), then kiln.Signal($t, 0x71): 4 s
            const uint hT = 0xFFFF0000;
            var aliasGuid = new Guid("06c0a5e0-0000-4000-8000-000000000005");
            Abilities.Programs.Register(Program(aliasGuid,
                [11, hT, 11, hT, 19, 0x06c0a551, Natives.Alias.HashFor(Context.Info.Id), 27, 11, 0x71, 11, hT, 19, 0x06c0a552, Fnv("kiln.Signal"), 27, 30],
                [("T::value", Time, 8, BitConverter.GetBytes(2_000_000_000L))]));
            _signals.Clear();
            var e5 = await Abilities.RunProgram(aliasGuid, P(12));
            _spawned.Add(e5);
            await Steps(6);
            var sig5 = _signals.FirstOrDefault(s => s.Impact.Self == e5 && s.Tag == 0x71);
            var ns5 = sig5 is { Value.Length: >= 8 } ? BitConverter.ToInt64(sig5.Value) : -1;
            Check(ns5 == 4_000_000_000L, $"two ref TimeSpan arguments naming one variable share it: 2 s + 1 s + 1 s = {ns5} ns");

            // a throwing generated method: Throws($t) answers 0x11, the instance stops, $t holds the 5 s it wrote
            var throwGuid = new Guid("06c0a5e0-0000-4000-8000-000000000006");
            Abilities.Programs.Register(Program(throwGuid,
                [11, hT, 19, 0x06c0a561, Natives.Throws.HashFor(Context.Info.Id), 27, 11, 0x72, 11, hT, 19, 0x06c0a562, Fnv("kiln.Signal"), 27, 30],
                [("T::value", Time, 8, BitConverter.GetBytes(1_000_000_000L))]));
            _signals.Clear();
            _started.Clear();
            var t0c = GenNatives.ThrowCalls;
            var e6 = await Abilities.RunProgram(throwGuid, P(15));
            _spawned.Add(e6);
            await Steps(6);
            var st6 = _started.FirstOrDefault(s => s.Impact.Self == e6);
            var inst = st6 is null ? null : Abilities.Instance(st6.Impact.ImpactId);
            var tv = inst?.Find("T::value") is { } v && inst.Read(v) is { Length: 8 } bytes ? BitConverter.ToInt64(bytes) : -1;
            var logged = LogHas("abilities: sync native natives-gen-test.Throws threw: InvalidOperationException: natives-gen-test: Throws throws on purpose");
            Check(GenNatives.ThrowCalls - t0c == 1 && !_signals.Any(s => s.Impact.Self == e6) && tv == 5_000_000_000L && logged,
                $"a generated method that throws: the call answers 0x11 (no signal after it), the TimeSpan it changed is kept ({tv} ns), " +
                $"the exception is in kiln.log under the mod ({(logged ? "found" : "NOT found")}); instance flags {inst?.Flags}");

            await TestEcs(P(18));

            // round trip: Round reads the initial values and writes new ones, Check reads them back
            uint[] push = [11, 0xFFFF0005, 11, 0xFFFF0004, 11, 0xFFFF0003, 11, 0xFFFF0002, 11, 0xFFFF0001, 11, 0xFFFF0000];
            var roundGuid = new Guid("06c0a5e0-0000-4000-8000-000000000003");
            Abilities.Programs.Register(Program(roundGuid,
                [.. push, 19, 0x06c0a521, Natives.Round.HashFor(Context.Info.Id), 27, .. push, 19, 0x06c0a522, Natives.Check.HashFor(Context.Info.Id), 27, 30],
                [("S::value", Sint32, 4, BitConverter.GetBytes(7)), ("F::value", Float32, 4, BitConverter.GetBytes(1.5f)), ("B::value", Bool, 1, [1]),
                 ("T::value", Time, 8, BitConverter.GetBytes(1000L)), ("E::value", EntityIdType, 4, BitConverter.GetBytes(42)),
                 ("V::value", Float3, 12, [.. BitConverter.GetBytes(1f), .. BitConverter.GetBytes(2f), .. BitConverter.GetBytes(3f)])]));
            var r0 = GenNatives.RoundCalls;
            var e3 = await Abilities.RunProgram(roundGuid, P(6));
            _spawned.Add(e3);
            await Steps(6);
            Check(GenNatives.RoundCalls - r0 == 1 && GenNatives.RoundRead && GenNatives.CheckRead,
                $"types sint32/float32/bool/Time/EntityId/float3 round trip through generated natives: read {(GenNatives.RoundRead ? "as written" : "WRONG")}, " +
                $"written and read back {(GenNatives.CheckRead ? "as written" : "WRONG")}");
        }
        catch (Exception e)
        {
            Fail($"threw: {e}");
        }
        finally
        {
            foreach (var e in _spawned) try { if (Spawn.Exists(e)) Spawn.Destroy(e); } catch { }
            Log.Info($"natives-gen-test: INFO summary: {_pass} PASS, {_fail} FAIL");
        }
    }

    // S5 rows: the self-check, TryRead/TryWrite<Health> of a Vukah seen by World.TryRead, an undeclared type, one event
    // per entity and component per step on the tick thread with Writer "<modid>/native:<name>"; across a restart
    // (data/ecs-restart: written by the first run, checked by the next) the written Health in the save.
    private readonly List<ComponentWrittenEvent> _written = new();
    [Subscribe] private void OnWritten(ComponentWrittenEvent e) => _written.Add(e);

    private async Task TestEcs(WorldPosition p)
    {
        var row = Capabilities.Get("abilities.natives.ecs");
        Check(row.State == CapabilityState.Available && LogHas("NativeContext's reads and writes of other entities are on in this world"),
            $"the self-check of the first native call: abilities.natives.ecs {row.State} {row.ReasonText} (kiln.log: the view, the counts, the registry)");
        var restartFile = Path.Combine(Context.DataDirectory, "ecs-restart");
        if (File.Exists(restartFile))
        {
            // "<template> <x> <y> <z> <health>": the saved entity the previous run's native wrote, found again by template and place
            var parts = File.ReadAllText(restartFile).Split(' ');
            if (parts.Length != 5) { File.Delete(restartFile); parts = []; }
        }
        if (File.Exists(restartFile))
        {
            var parts = File.ReadAllText(restartFile).Split(' ');
            var at = new WorldPosition(double.Parse(parts[1], System.Globalization.CultureInfo.InvariantCulture),
                double.Parse(parts[2], System.Globalization.CultureInfo.InvariantCulture), double.Parse(parts[3], System.Globalization.CultureInfo.InvariantCulture));
            var want = long.Parse(parts[4]);
            var again = Spawn.FindByName(parts[0], at, 1.0, max: 4).FirstOrDefault(x => Spawn.Inspect(x)?.TemplateName == parts[0]);
            var got = again.IsNone ? long.MinValue : TorchY(again);
            Check(got == want, $"after a restart, the CurrentTransform a native wrote through impact_simulation is in the save: {parts[0]} at {at} y " +
                $"{(again.IsNone ? "not found" : got.ToString())} (written {want})");
            File.Delete(restartFile);
        }
        var vukah = await Spawn.Entity("Enemy_Vukah_Melee", p, 0, new SpawnOptions { Tag = "natives-gen-test" });  // not transient: saved
        await Steps(10);
        var start = HealthOf(vukah);
        const uint hT = 0xFFFF0000;
        var hurtGuid = new Guid("06c0a5e0-0000-4000-8000-000000000007");
        Abilities.Programs.Register(Program(hurtGuid, [11, hT, 19, 0x06c0a571, Natives.Hurt.HashFor(Context.Info.Id), 27, 30],
            [("Target::value", EntityIdType, 4, BitConverter.GetBytes(vukah.Value))]));
        _written.Clear();
        _started.Clear();
        var calls0 = GenNatives.HurtCalls;
        // the instance's hierarchy root is the Vukah (ImpactOptions.Root): NativeContext.Root must say the same as ImpactRef.Root
        var e = await Abilities.RunProgram(hurtGuid, p with { X = p.X + 3 }, new ImpactOptions { Root = vukah });
        _spawned.Add(e);
        await Steps(8);
        var after = HealthOf(vukah);
        Check(GenNatives.HurtCalls - calls0 == 1 && GenNatives.HurtRead && GenNatives.HurtWrite1 && GenNatives.HurtWrite2 &&
              GenNatives.HurtBefore == start && after == start - 7,
            $"TryWrite<Health> inside the native's call is seen by World.TryRead: {start} -> {after} (the call read {GenNatives.HurtBefore})");
        Check(!GenNatives.UndeclaredRead, "TryRead of a type impact_simulation does not declare for other entities returns false with no access");
        var mine = _written.Where(w => w.Entity == vukah && w.Component == "Health").ToList();
        Check(mine.Count == 1 && mine[0].Writer == $"{Context.Info.Id}/native:{Context.Info.Id}.Hurt" && mine[0].Writers.Count == 1 &&
              BitConverter.ToInt32(mine[0].Result.Before.Span[8..]) == start && BitConverter.ToInt32(mine[0].Result.After.Span[8..]) == start - 7,
            $"one ComponentWrittenEvent per entity and component per step, on the tick thread: {mine.Count} event(s), writer " +
            $"{mine.FirstOrDefault()?.Writer}, before {start}, after {start - 7}");
        var st = _started.FirstOrDefault(s => s.Impact.Self == e);
        Check(st is not null && GenNatives.HurtRoot == vukah && st.Impact.Root == vukah,
            $"NativeContext.Root is the instance's hierarchy root, as ImpactRef.Root reads it: {GenNatives.HurtRoot} / {st?.Impact.Root} (root {vukah})");

        // a saved prop of the base (a wall torch: CurrentTransform is in impact_simulation's other_write list and the save
        // tracks it): written through a native for the next run, which reads it after the restart
        var torch = Spawn.FindByName("Prop_Torch01_Wall_On_Craftable", max: 1).FirstOrDefault();
        if (!torch.IsNone && Spawn.Inspect(torch) is { TemplateName: { } template } info)
        {
            var before = TorchY(torch);
            var prog = new Guid("06c0a5e0-0000-4000-8000-000000000008");
            Abilities.Programs.Register(Program(prog, [11, hT, 19, 0x06c0a581, Natives.Nudge.HashFor(Context.Info.Id), 27, 30],
                [("Target::value", EntityIdType, 4, BitConverter.GetBytes(torch.Value))]));
            _spawned.Add(await Abilities.RunProgram(prog, p with { Z = p.Z + 4 }));
            await Steps(8);
            var now = TorchY(torch);
            Check(before != long.MinValue && now == before + (1L << 28) && GenNatives.NudgeAfter == now,
                $"a native writes a saved prop's CurrentTransform through impact_simulation (save-tracked: {World.WriteInfo<Ecs.CurrentTransform>().SaveTracked}): " +
                $"{template} ({torch}) y {before / 4294967296.0:F4} -> {now / 4294967296.0:F4} m");
            if (now == before + (1L << 28))
            {
                Directory.CreateDirectory(Context.DataDirectory);
                var inv = System.Globalization.CultureInfo.InvariantCulture;
                File.WriteAllText(restartFile, $"{template} {info.Position.X.ToString(inv)} {info.Position.Y.ToString(inv)} {info.Position.Z.ToString(inv)} {now}");
            }
        }
        else Fail("no saved wall torch (Prop_Torch01_Wall_On_Craftable) in this world for the restart row");
    }

    private void Info(string what) => Log.Info($"natives-gen-test: INFO {what}");

    private static int HealthOf(EntityId e) => World.TryRead<Ecs.Health>(e, "Health", out var h) ? h.Values[0] : -1;
    private static long TorchY(EntityId e) => World.TryRead<Ecs.CurrentTransform>(e, "CurrentTransform", out var c) ? GenNatives.YOf(c) : long.MinValue;

    private bool LogHas(string text)
    {
        var log = Path.GetFullPath(Path.Combine(Context.DataDirectory, "..", "..", "logs", "kiln.log"));
        try
        {
            using var f = new FileStream(log, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete);
            using var r = new StreamReader(f);
            return r.ReadToEnd().Contains(text, StringComparison.Ordinal);
        }
        catch (IOException) { return false; }
    }

    private void Check(bool ok, string what) { if (ok) { _pass++; Log.Info($"natives-gen-test: PASS {what}"); } else { _fail++; Log.Error($"natives-gen-test: FAIL {what}"); } }
    private void Fail(string what) { _fail++; Log.Error($"natives-gen-test: FAIL {what}"); }

    private static uint Fnv(string s) { uint h = 0x811c9dc5; foreach (var x in Encoding.UTF8.GetBytes(s)) h = (h ^ x) * 0x01000193; return h; }

    // keen::ImpactProgram in memory (the layout of mods/tests/abilities-test's ProgramBuilder)
    private static byte[] Program(Guid guid, uint[] code, (string Name, uint Type, int Size, byte[] Init)[] vars)
    {
        var names = vars.Select(v => Encoding.UTF8.GetBytes(v.Name)).ToArray();
        var dataSize = vars.Sum(v => v.Size);
        const int codeOff = 64;
        var shutOff = codeOff + code.Length * 4;
        var layoutOff = shutOff + 4;
        var namesOff = layoutOff + vars.Length * 24;
        var dataOff = namesOff + names.Sum(n => n.Length);
        var b = new byte[(dataOff + dataSize + 3) & ~3];
        var g = guid.ToByteArray();
        BitConverter.GetBytes(Fnv(g)).CopyTo(b, 0);
        g.CopyTo(b, 4);
        BitConverter.GetBytes((ushort)256).CopyTo(b, 0x14);
        void Arr(int field, int target, int count) { BitConverter.GetBytes(count == 0 ? 0 : target - field).CopyTo(b, field); BitConverter.GetBytes(count).CopyTo(b, field + 4); }
        Arr(0x18, codeOff, code.Length);
        Arr(0x20, shutOff, 1);
        Arr(0x28, layoutOff, vars.Length);
        Arr(0x30, dataOff, dataSize);
        for (var i = 0; i < code.Length; i++) BitConverter.GetBytes(code[i]).CopyTo(b, codeOff + i * 4);
        BitConverter.GetBytes(30u).CopyTo(b, shutOff);
        int nameAt = namesOff, dataAt = 0;
        for (var i = 0; i < vars.Length; i++)
        {
            var e = layoutOff + i * 24;
            BitConverter.GetBytes(Fnv(names[i])).CopyTo(b, e);
            BitConverter.GetBytes(vars[i].Type).CopyTo(b, e + 8);
            BitConverter.GetBytes((ushort)vars[i].Size).CopyTo(b, e + 12);
            BitConverter.GetBytes((ushort)dataAt).CopyTo(b, e + 14);
            BitConverter.GetBytes(nameAt - (e + 16)).CopyTo(b, e + 16);
            BitConverter.GetBytes(names[i].Length).CopyTo(b, e + 20);
            names[i].CopyTo(b, nameAt);
            nameAt += names[i].Length;
            vars[i].Init.CopyTo(b, dataOff + dataAt);
            dataAt += vars[i].Size;
        }
        return b;
    }

    private static uint Fnv(byte[] g) { uint h = 0x811c9dc5; foreach (var x in g) h = (h ^ x) * 0x01000193; return h; }
}
