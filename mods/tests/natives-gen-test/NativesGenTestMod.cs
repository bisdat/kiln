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

    public override void OnLoad()
    {
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
