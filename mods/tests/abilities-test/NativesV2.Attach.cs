// SPDX-License-Identifier: MIT
// abilities-test rows of package 06c, slice S2 (since 0.2.0-dev.29): handlers attached to a vanilla native's calls,
// filtered in native code on (program, site). Called from NativesV2Pass.
using Kiln;

namespace AbilitiesTest;

public sealed partial class AbilitiesTestMod
{
    private const uint GetSelf = 0x01d09759, GetTransform = 0xa18f5959, EntityIdType = 0x737c8f86;
    private const uint WorldTransformType = 0x9493a8c5, Float3Type = 0x3d1f8582, ExplosionGetTransformSite = 0xfcce4e0a;

    private async Task TestAttachments(int pass, string tag, WorldPosition p)
    {
        await TestAttachExplosion(pass, tag, p);
        await TestAttachStages(pass, tag, p with { X = p.X + 4 });
        await TestDetachDuringCast(pass, tag, p with { X = p.X + 8 });
    }

    // getTransform of the vanilla Fireball explosion at its one site: 1 call per cast; a Kiln program's getTransform
    // (another program) never reaches the handler, and no managed code runs for it (the thunk's counters)
    private async Task TestAttachExplosion(int pass, string tag, WorldPosition p)
    {
        long calls = 0;
        var selves = new System.Collections.Concurrent.ConcurrentQueue<EntityId>();  // the guided check 06c-natives.joined binds calls to explosions by it
        var attach = Abilities.Natives.Attach(ExplosionProgram, "getTransform", NativeStage.Before,
            (ref NativeCall c) => { Interlocked.Increment(ref calls); selves.Enqueue(c.Self); }, site: ExplosionGetTransformSite);
        Check(attach.Ok, $"{tag}: Attach(0x{ExplosionProgram:x8}, getTransform, site 0x{ExplosionGetTransformSite:x8}): {attach.Reason ?? "ok"}");
        if (!attach.Ok) return;
        const uint hSelf = 0xFFFF0000, hT = 0xFFFF0001, hPos = 0xFFFF0002;
        uint[] code =
        [
            11, hSelf, 19, 0x06c00050, GetSelf, 27,
            11, hSelf, 11, hPos, 11, hT, 19, 0x06c00051, GetTransform, 27,  // args[0] = T, args[1] = Pos, args[2] = Self
            30,
        ];
        var guid = new Guid($"06c00005-0000-4000-8000-0000000000{pass:x2}");
        var other = Abilities.Programs.Register(ProgramBuilder.Raw(guid, code, [30],
            [("Self::value", EntityIdType, 4, new byte[4]), ("T::value", WorldTransformType, 56, new byte[56]), ("Pos::value", Float3Type, 12, new byte[12])]), out _);
        var c0 = Abilities.Natives.LabCounters;
        var e1 = await Abilities.RunImpact(Explosion, p);
        var e2 = await Abilities.RunProgram(guid, p with { Z = p.Z + 3 });
        _spawned.Add(e1);
        _spawned.Add(e2);
        await Steps(8);
        var c1 = Abilities.Natives.LabCounters;
        attach.Handle!.Dispose();
        var fromKiln = Abilities.Programs.Info(ExplosionProgram)?.FromKiln;
        var entered = (c1?.AttachEntered ?? 0) - (c0?.AttachEntered ?? 0);
        var skipped = (c1?.AttachSkipped ?? 0) - (c0?.AttachSkipped ?? 0);
        var seen = selves.ToArray();
        Check(Interlocked.Read(ref calls) == 1 && fromKiln == false && seen is [var s1] && s1 == e1,
            $"{tag}: Attach(0x{ExplosionProgram:x8}, \"getTransform\", site 0x{ExplosionGetTransformSite:x8}): {Interlocked.Read(ref calls)} call per cast, " +
            $"from that explosion (call's Self {string.Join(", ", seen)}, RunImpact gave {e1}), 0 for other programs (program 0x{other:x8} called getTransform too), FromKiln {fromKiln}");
        Check((long)entered == Interlocked.Read(ref calls) && skipped >= 1,
            $"{tag}: the thunk counter shows 0 managed transitions for non-matching calls: {entered} entered (= the handler's " +
            $"{Interlocked.Read(ref calls)}), {skipped} filtered out in native code");
    }

    // Before x2 (priority 10, 5), Instead, After on getSelf at one site of a Kiln program; a second Instead, an unknown
    // site and an unknown program are refused
    private async Task TestAttachStages(int pass, string tag, WorldPosition p)
    {
        const uint hSelf = 0xFFFF0000, siteA = 0x06c00031, siteB = 0x06c00032, tagS = 0x5E1F;
        uint[] code =
        [
            11, hSelf, 19, siteA, GetSelf, 27,
            11, hSelf, 19, siteB, GetSelf, 27,
            11, tagS, 11, hSelf, 19, 0x06c00033, Abilities.HashOf("kiln.Signal"), 27,
            30,
        ];
        var guid = new Guid($"06c00006-0000-4000-8000-0000000000{pass:x2}");
        var prog = Abilities.Programs.Register(ProgramBuilder.Raw(guid, code, [30], [("Self::value", EntityIdType, 4, new byte[4])]), out _);
        var order = new List<string>();
        uint afterSaw = 0;
        bool insteadSawUndecided = false;
        var handles = new List<IDisposable>();
        AttachResult Add(NativeStage stage, int priority, NativeWrapHandler h, uint site = siteA) =>
            Abilities.Natives.Attach(prog, "getSelf", stage, h, site, priority);
        var rs = new[]
        {
            Add(NativeStage.After, 0, (ref NativeCall c) => { lock (order) order.Add("After"); afterSaw = c.GetU32(0); }),
            Add(NativeStage.Instead, 0, (ref NativeCall c) =>
            {
                lock (order) order.Add("Instead");
                insteadSawUndecided = !c.Decided;
                c.SetU32(0, 0x06C06C);
                c.Return(1);
            }),
            Add(NativeStage.Before, 5, (ref NativeCall c) => { lock (order) order.Add("Before5"); }),
            Add(NativeStage.Before, 10, (ref NativeCall c) => { lock (order) order.Add("Before10"); }),
        };
        foreach (var r in rs) if (r.Handle is { } h) handles.Add(h);
        var second = Add(NativeStage.Instead, 9, (ref NativeCall c) => c.Return(2));
        var noSite = Add(NativeStage.Before, 0, (ref NativeCall c) => { }, site: 0xdeadbeef);
        var noProgram = Abilities.Natives.Attach(0x0badf00d, "getSelf", NativeStage.Before, (ref NativeCall c) => { });
        _signals.Clear();
        var e = await Abilities.RunProgram(guid, p);
        _spawned.Add(e);
        await Steps(6);
        foreach (var h in handles) h.Dispose();
        var sig = _signals.FirstOrDefault(s => s.Impact.Self == e);
        string got;
        lock (order) got = string.Join(", ", order);
        Check(rs.All(r => r.Ok) && got == "Before10, Before5, Instead, After" && insteadSawUndecided && afterSaw == 0x06C06C,
            $"{tag}: stage order: Before x2, Instead, After, in recorded order: [{got}]; the Instead answered (After read 0x{afterSaw:x}); " +
            $"site B ran the game's getSelf: signal {sig?.AsInt} (self {e.Value})");
        Check(!second.Ok && second.Reason?.Contains("second Instead") == true, $"{tag}: a second Instead is refused: {second.Reason}");
        Check(!noSite.Ok && noSite.Reason?.Contains("site not found") == true, $"{tag}: site not found is Ok=false: {noSite.Reason}");
        Check(!noProgram.Ok && noProgram.Reason?.Contains("program not loaded") == true, $"{tag}: program not loaded is Ok=false: {noProgram.Reason}");
    }

    // A program calls getSelf every step (paced by a suspending native); the Before on it is detached while it runs: from
    // the Pre that detached it, the handler gets no call
    private async Task TestDetachDuringCast(int pass, string tag, WorldPosition p)
    {
        const uint hSelf = 0xFFFF0000, site = 0x06c00061;
        uint[] code =
        [
            11, hSelf, 19, site, GetSelf, 27,
            11, hSelf, 19, 0x06c00062, _nvWait, 27,
            8, 0,
            30,
        ];
        var guid = new Guid($"06c00007-0000-4000-8000-0000000000{pass:x2}");
        var prog = Abilities.Programs.Register(ProgramBuilder.Raw(guid, code, [30], [("Self::value", EntityIdType, 4, new byte[4])]), out _);
        long calls = 0;
        var a = Abilities.Natives.Attach(prog, "getSelf", NativeStage.Before, (ref NativeCall c) => Interlocked.Increment(ref calls), site);
        var e = await Abilities.RunProgram(guid, p);
        _spawned.Add(e);
        await Steps(10);
        var before = Interlocked.Read(ref calls);
        a.Handle?.Dispose();
        var at = Interlocked.Read(ref calls);
        var loops = _nvWaitCalls;
        await Steps(10);
        var after = Interlocked.Read(ref calls);
        var looped = _nvWaitCalls - loops;  // each loop calls getSelf, then the pacing native
        if (Spawn.Exists(e)) Spawn.Destroy(e);
        Check(a.Ok && before >= 3 && after == at && looped >= 3,
            $"{tag}: detach during a running cast: 0 calls after the Pre that removed it ({before} call(s) while attached, " +
            $"{after - at} after, while the program looped {looped} more time(s) calling getSelf)");
    }
}
