// SPDX-License-Identifier: MIT
// Lab helper of abilities-test (NativesV2.cs, "unload during a running cast"): one synchronous native,
// natives-helper.Count(args[0] = Out sint32): Out = calls so far, answers 1. abilities-test unloads this mod while a
// program calls it every step; from the Pre that removed it no call may reach it.
using Kiln;

namespace NativesHelper;

public sealed class NativesHelperMod : Mod
{
    private static int _calls;

    public override void OnLoad()
    {
        var r = Abilities.Natives.RegisterSync("Count", 1, (ref NativeCall c) =>
        {
            c.SetU32(0, (uint)Interlocked.Increment(ref _calls));
            return 1;
        });
        Log.Info($"natives-helper: RegisterSync(Count): {r}");
    }

    public override void OnUnload() => Log.Info($"natives-helper: unloading after {Volatile.Read(ref _calls)} call(s)");
}
