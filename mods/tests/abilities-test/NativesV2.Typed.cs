// SPDX-License-Identifier: MIT
// abilities-test rows of package 06c, slice S3 (since 0.2.0-dev.29): typed native declarations. kiln.abilities read
// this mod's impact/natives.json when it started: NvTyped, NvRound and NvCheck are typed ("sync": true), NvLate and
// NvLate2 argc only. Called once from RunNativesV2 (the declarations are per process).
using System.Text;
using Kiln;

namespace AbilitiesTest;

public sealed partial class AbilitiesTestMod
{
    private const uint Float32Type = 0xe89f7410, BoolType = 0xc894953d, TimeType = 0xdfe4e404;

    private async Task TestTypedNatives(WorldPosition p)
    {
        await TestArgTypeRefused(p);
        await TestArgcOnlyAtRunTime(p with { X = p.X + 4 });
        await TestTypedRoundTrip(p with { X = p.X + 8 });
    }

    // a program passing float32 to the int parameter of a typed native is refused when it registers
    private async Task TestArgTypeRefused(WorldPosition p)
    {
        const uint hOut = 0xFFFF0000, hIn = 0xFFFF0001, hF = 0xFFFF0002;
        var typed = Abilities.HashOf("abilities-test.NvTyped");
        long calls = 0;
        var reg = Abilities.Natives.RegisterSync("NvTyped", 2, (ref NativeCall c) => { Interlocked.Increment(ref calls); c.SetU32(0, c.GetU32(1) * 3); return 1; });
        (string, uint, int, byte[])[] vars = [("Out::value", Sint32, 4, new byte[4]), ("In::value", Sint32, 4, BitConverter.GetBytes(5)), ("F::value", Float32Type, 4, BitConverter.GetBytes(1.5f))];
        string? refused = null;
        try
        {
            Abilities.Programs.Register(ProgramBuilder.Raw(new Guid("06c00008-0000-4000-8000-000000000001"),
                [11, hF, 11, hOut, 19, 0x06c00081, typed, 27, 30], [30], vars));
        }
        catch (ArgumentException e) { refused = e.Message; }
        Check(refused?.Contains("E_ARGTYPE") == true && refused.Contains("args[1]"),
            $"a program passing float32 to an int parameter is refused at registration (E_ARGTYPE): {refused ?? "accepted"}");
        var goodGuid = new Guid("06c00008-0000-4000-8000-000000000002");
        var good = Abilities.Programs.Register(ProgramBuilder.Raw(goodGuid, [11, hIn, 11, hOut, 19, 0x06c00082, typed, 27, 30], [30], vars), out var waiting);
        var e2 = await Abilities.RunProgram(goodGuid, p);
        _spawned.Add(e2);
        await Steps(4);
        Check(reg.Ok && !waiting && Interlocked.Read(ref calls) == 1,
            $"the same native with an int variable: program 0x{good:x8} registered (waiting {waiting}) and the typed call ran ({Interlocked.Read(ref calls)} call)");
    }

    // argc-only declarations (an older natives.json): the program passes with a W_ARGTYPE warning; the native typed at
    // run time answers 0x2C for the float32 and 0x18 for a required argument left unconnected, and its handler never runs
    private async Task TestArgcOnlyAtRunTime(WorldPosition p)
    {
        const uint hOut = 0xFFFF0000, hF = 0xFFFF0001;
        var late = Abilities.HashOf("abilities-test.NvLate");
        var late2 = Abilities.HashOf("abilities-test.NvLate2");
        (string, uint, int, byte[])[] vars = [("Out::value", Sint32, 4, new byte[4]), ("F::value", Float32Type, 4, BitConverter.GetBytes(1.5f))];
        var g1 = new Guid("06c00009-0000-4000-8000-000000000001");
        var g2 = new Guid("06c00009-0000-4000-8000-000000000002");
        var log0 = KilnLogLength();
        var id1 = Abilities.Programs.Register(ProgramBuilder.Raw(g1, [11, hF, 11, hOut, 19, 0x06c00091, late, 27, 30], [30], vars), out var w1);
        var id2 = Abilities.Programs.Register(ProgramBuilder.Raw(g2, [12, 11, hOut, 19, 0x06c00092, late2, 27, 30], [30], vars), out var w2);
        await Steps(1);
        var warned = KilnLogSince(log0).Split('\n').Count(l => l.Contains("W_ARGTYPE") && l.Contains("program from"));
        long calls = 0;
        Abilities.Natives.DeclareTyped("abilities-test.NvLate", new NativeArg("sint32", NativeArgDirection.Out), new NativeArg("sint32"));
        Abilities.Natives.DeclareTyped("abilities-test.NvLate2", new NativeArg("sint32", NativeArgDirection.Out), new NativeArg("sint32"));
        var r1 = Abilities.Natives.RegisterSync("NvLate", 2, (ref NativeCall c) => { Interlocked.Increment(ref calls); return 1; });
        var r2 = Abilities.Natives.RegisterSync("NvLate2", 2, (ref NativeCall c) => { Interlocked.Increment(ref calls); return 1; });
        var c0 = Abilities.Natives.LabCounters;
        _started.Clear();
        var e1 = await Abilities.RunProgram(g1, p);
        var e2 = await Abilities.RunProgram(g2, p with { Z = p.Z + 3 });
        _spawned.Add(e1);
        _spawned.Add(e2);
        await Steps(4);
        var c1 = Abilities.Natives.LabCounters;
        uint? Flags(EntityId e) => _started.FirstOrDefault(s => s.Impact.Self == e) is { } st ? Abilities.Instance(st.Impact.ImpactId)?.Flags : null;
        var f1 = Flags(e1);
        var refusedTypes = (c1?.TypedRefused ?? 0) - (c0?.TypedRefused ?? 0);
        var refusedNone = (c1?.TypedUnconnected ?? 0) - (c0?.TypedUnconnected ?? 0);
        Check(!w1 && !w2 && warned >= 2,
            $"an argc-only declaration still warns: programs 0x{id1:x8} and 0x{id2:x8} registered (waiting {w1}/{w2}), {warned} W_ARGTYPE line(s) in kiln.log");
        Check(r1.Ok && r2.Ok && refusedTypes == 1 && refusedNone == 1 && Interlocked.Read(ref calls) == 0,
            $"... and answers 0x2C at run time once the native is typed: {refusedTypes} call refused 0x2C (float32 for int), {refusedNone} refused 0x18 " +
            $"(not connected), {Interlocked.Read(ref calls)} handler call(s); instance flags {f1?.ToString("x") ?? "gone"}");
    }

    // sint32 / float32 / bool / Time / EntityId / WorldTransform through a typed native: read, written, read back
    private async Task TestTypedRoundTrip(WorldPosition p)
    {
        var round = Abilities.HashOf("abilities-test.NvRound");
        var check = Abilities.HashOf("abilities-test.NvCheck");
        uint[] types = [Sint32, Float32Type, BoolType, TimeType, EntityIdType, WorldTransformType];
        var w0 = Enumerable.Range(0, 56).Select(i => (byte)i).ToArray();
        var seen = new StringBuilder();
        bool readOk = false, backOk = false;
        int rounds = 0, checks = 0;
        Abilities.Natives.RegisterSync("NvRound", 6, (ref NativeCall c) =>
        {
            rounds++;
            var typesOk = c.ArgCount == 6;
            for (var i = 0; i < 6; i++) typesOk &= c.VariableType(i) == types[i];
            readOk = typesOk && (int)c.GetU32(0) == 7 && c.GetFloat(1) == 1.5f && c.Bytes(2)[0] == 1 &&
                     BitConverter.ToInt64(c.Bytes(3)) == 1000 && c.GetEntity(4).Value == 42 && c.Bytes(5).SequenceEqual(w0);
            c.SetU32(0, unchecked((uint)-7));
            c.SetFloat(1, 2.5f);
            c.Bytes(2)[0] = 0;
            BitConverter.TryWriteBytes(c.Bytes(3), 2000L);
            c.SetU32(4, 43);
            var w = c.Bytes(5);
            for (var i = 0; i < w.Length; i++) w[i] = (byte)(i + 1);
            return 1;
        });
        Abilities.Natives.RegisterSync("NvCheck", 6, (ref NativeCall c) =>
        {
            checks++;
            var w = c.Bytes(5);
            var wOk = w.Length == 56;
            for (var i = 0; i < w.Length; i++) wOk &= w[i] == (byte)(i + 1);
            backOk = (int)c.GetU32(0) == -7 && c.GetFloat(1) == 2.5f && c.Bytes(2)[0] == 0 &&
                     BitConverter.ToInt64(c.Bytes(3)) == 2000 && c.GetEntity(4).Value == 43 && wOk;
            seen.Append($"s {(int)c.GetU32(0)}, f {c.GetFloat(1)}, b {c.Bytes(2)[0]}, t {BitConverter.ToInt64(c.Bytes(3))} ns, e {c.GetU32(4)}, w[55] {w[55]}");
            return 1;
        });
        const uint hS = 0xFFFF0000, hF = 0xFFFF0001, hB = 0xFFFF0002, hT = 0xFFFF0003, hE = 0xFFFF0004, hW = 0xFFFF0005;
        uint[] push = [11, hW, 11, hE, 11, hT, 11, hB, 11, hF, 11, hS];  // args[0] = S is pushed last
        uint[] code = [.. push, 19, 0x06c000a1, round, 27, .. push, 19, 0x06c000a2, check, 27, 30];
        var guid = new Guid("06c0000a-0000-4000-8000-000000000001");
        Abilities.Programs.Register(ProgramBuilder.Raw(guid, code, [30],
        [
            ("S::value", Sint32, 4, BitConverter.GetBytes(7)), ("F::value", Float32Type, 4, BitConverter.GetBytes(1.5f)),
            ("B::value", BoolType, 1, [1]), ("T::value", TimeType, 8, BitConverter.GetBytes(1000L)),
            ("E::value", EntityIdType, 4, BitConverter.GetBytes(42)), ("W::value", WorldTransformType, 56, w0),
        ]), out var waiting);
        var e = await Abilities.RunProgram(guid, p);
        _spawned.Add(e);
        await Steps(4);
        Check(!waiting && rounds == 1 && checks == 1 && readOk && backOk,
            $"types sint32/float32/bool/Time/EntityId/WorldTransform round trip: read {(readOk ? "as written" : "WRONG")}, " +
            $"written and read back {(backOk ? "as written" : "WRONG")} ({seen}); {rounds}/{checks} call(s)");
    }

    private long KilnLogLength()
    {
        try { return new FileInfo(KilnLogPath).Length; } catch { return 0; }
    }

    private string KilnLogSince(long offset)
    {
        try
        {
            using var f = new FileStream(KilnLogPath, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete);
            f.Seek(Math.Min(offset, f.Length), SeekOrigin.Begin);
            return new StreamReader(f).ReadToEnd();
        }
        catch (IOException) { return ""; }
    }

    private string KilnLogPath => Path.GetFullPath(Path.Combine(Context.DataDirectory, "..", "..", "logs", "kiln.log"));
}
