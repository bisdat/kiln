// SPDX-License-Identifier: MIT
// Lab helper of abilities-test (NativesV2.cs, "unload during a running cast") and fixture of the guided check
// 06c-natives.joined (mods/tests/client-pass/Dev28ImpactNatives.cs). One synchronous native,
// natives-helper.Count(args[0] = Out sint32): Out = calls so far, answers 1; and the program natives-helper:count
// (CountProgram): Count(Out); kiln.Signal(Out, 0x06C1); HALT. abilities-test unloads this mod while its own program calls
// Count every step; from the Pre that removed it no call may reach it. Advanced: it registers a program blob.
using System.Text;
using Kiln;

namespace NativesHelper;

public sealed class NativesHelperMod : Mod
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
        Log.Info($"natives-helper: RegisterSync(Count): {r}");
        if (Abilities.Programs.Info(Abilities.Programs.IdOf(CountProgram)) is not null) return;  // loaded again: still there
        try
        {
            var id = Abilities.Programs.Register(Program(r.Hash), out var waiting);
            Log.Info($"natives-helper: program {CountProgram} (0x{id:x8}) registered{(waiting ? ", waiting for the world" : "")}");
        }
        catch (ArgumentException e)
        {
            Log.Info($"natives-helper: program {CountProgram}: {e.Message}");  // a reload: the first one stays
        }
    }

    public override void OnUnload() => Log.Info($"natives-helper: unloading after {Volatile.Read(ref _calls)} call(s)");

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
