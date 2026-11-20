// SPDX-License-Identifier: MIT
// Fixture of the guided check 06c-natives.joined (mods/tests/client-pass/Dev28ImpactNatives.cs), for the client-pass
// profile only (a lab with abilities-test does not deploy it: that suite starts with no Kiln program in the registry).
// One synchronous native, natives-fixture.Count(args[0] = Out sint32): Out = calls so far, answers 1; and the program
// CountProgram: Count(Out); kiln.Signal(Out, 0x06C1); HALT. Advanced: it registers a program blob.
using System.Text;
using Kiln;

namespace NativesFixture;

public sealed class NativesFixtureMod : Mod
{
    /// <summary>The program the guided check runs (Abilities.RunProgram): its kiln.Signal carries tag 0x06C1.</summary>
    public static readonly Guid CountProgram = new("06c0c0de-0000-4000-8000-000000000001");

    private static int _calls;

    public override void OnLoad()
    {
        var r = Abilities.Natives.RegisterSync("Count", 1, (ref NativeCall c) =>
        {
            c.SetU32(0, (uint)Interlocked.Increment(ref _calls));
            return 1;
        });
        Log.Info($"natives-fixture: RegisterSync(Count): {r}");
        if (Abilities.Programs.Info(Abilities.Programs.IdOf(CountProgram)) is not null) return;  // loaded again: still there
        try
        {
            var id = Abilities.Programs.Register(Program(r.Hash), out var waiting);
            Log.Info($"natives-fixture: program {CountProgram} (0x{id:x8}) registered{(waiting ? ", waiting for the world" : "")}");
        }
        catch (ArgumentException e)
        {
            Log.Info($"natives-fixture: program {CountProgram}: {e.Message}");  // a reload: the first one stays
        }
    }


    private static uint Fnv(ReadOnlySpan<byte> b) { uint h = 0x811c9dc5; foreach (var x in b) h = (h ^ x) * 0x01000193; return h; }

    // keen::ImpactProgram (docs/kiln/services/abilities.md, the contract): one sint32 variable Out
    private static byte[] Program(uint count)
    {
        const uint hOut = 0xFFFF0000, tag = 0x06C1;
        uint[] code = [11, hOut, 19, 0x06c0c001, count, 27, 11, tag, 11, hOut, 19, 0x06c0c002, Fnv("kiln.Signal"u8), 27, 30];
        var name = Encoding.UTF8.GetBytes("Out::value");
        const int codeOff = 64;
        var shutOff = codeOff + code.Length * 4;
        var layoutOff = shutOff + 4;
        var nameOff = layoutOff + 24;
        var dataOff = nameOff + name.Length;
        var b = new byte[(dataOff + 4 + 3) & ~3];
        var g = CountProgram.ToByteArray();
        BitConverter.GetBytes(Fnv(g)).CopyTo(b, 0);
        g.CopyTo(b, 4);
        BitConverter.GetBytes((ushort)256).CopyTo(b, 0x14);
        void Arr(int field, int target, int n) { BitConverter.GetBytes(target - field).CopyTo(b, field); BitConverter.GetBytes(n).CopyTo(b, field + 4); }
        Arr(0x18, codeOff, code.Length);
        Arr(0x20, shutOff, 1);
        Arr(0x28, layoutOff, 1);
        Arr(0x30, dataOff, 4);
        for (var i = 0; i < code.Length; i++) BitConverter.GetBytes(code[i]).CopyTo(b, codeOff + i * 4);
        BitConverter.GetBytes(30u).CopyTo(b, shutOff);
        BitConverter.GetBytes(Fnv(name)).CopyTo(b, layoutOff);
        BitConverter.GetBytes(0x9859f31eu).CopyTo(b, layoutOff + 8);  // sint32
        BitConverter.GetBytes((ushort)4).CopyTo(b, layoutOff + 12);
        BitConverter.GetBytes(nameOff - (layoutOff + 16)).CopyTo(b, layoutOff + 16);
        BitConverter.GetBytes(name.Length).CopyTo(b, layoutOff + 20);
        name.CopyTo(b, nameOff);
        return b;
    }
}
