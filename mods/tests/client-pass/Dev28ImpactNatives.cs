// SPDX-License-Identifier: MIT
// 06c-impact-natives: the guided check 06c-natives.joined. Synchronous natives (abilities.natives.sync) and attachments
// to a vanilla native (abilities.natives.attach) are on since 0.2.0-dev.29, proven in a dedicated-server lab without a
// player (also in the forced-parallel window); never with a player in the world, where the server runs its programs on
// its parallel path. Joined to a dedicated server (the body runs there): the fixture natives-fixture's program calls its
// sync native next to the player three times, and a Before attached to the Fireball explosion's getTransform counts
// three explosions; the check asserts the same-step answer, one call per explosion and which threads ran the handlers,
// and the player judges that the explosions looked and sounded as the game's own. A hosting game client has no
// kiln.abilities natives (no client signatures): both rows are unavailable there, and the check is joined only.
using System.Collections.Concurrent;
using Kiln;
using Kiln.Testing;

namespace ClientPass;

public sealed partial class ClientPassMod
{
    private const int ImpactNativesOrder = 2000 + (28080 - 28000) * 10;
    private static readonly Guid FixtureCountProgram = new("06c0c0de-0000-4000-8000-000000000001");  // natives-fixture's program

    [ClientCheck("06c-natives.joined", "Same-step natives and an attachment with a player in the world", Area = "Impact natives",
        Since = Dev29, Tags = [ClientCheckTags.Visual], Needs = ClientCheckNeeds.Joined, Order = ImpactNativesOrder + 0)]
    public async Task ImpactNativesJoined(CheckContext c)
    {
        if (!await Need(c, "natives-fixture") || !await NeedPlayer(c)) return;
        foreach (var row in new[] { "abilities.natives.sync", "abilities.natives.attach" })
            if (!Capabilities.Is(row))
            {
                await c.Skip($"{row} is unavailable: {Capabilities.Why(row)}");
                return;
            }
        var tick = Environment.CurrentManagedThreadId;
        var started = new ConcurrentQueue<ImpactStartedEvent>();
        var signals = new ConcurrentQueue<ImpactSignalEvent>();
        var subs = new List<IDisposable> { Events.On<ImpactStartedEvent>(e => started.Enqueue(e)), Events.On<ImpactSignalEvent>(e => signals.Enqueue(e)) };
        c.OnEnd(() => subs.ForEach(s => s.Dispose()));

        // 1. the fixture's program calls natives-fixture.Count (synchronous) and signals the answer
        var ran = new List<EntityId>();
        for (var i = 0; i < 3; i++)
        {
            if (Ahead(c, 6, (i - 1) * 2) is not { } at) break;
            ran.Add(await Abilities.RunProgram(FixtureCountProgram, at));
            await c.Ticks(8);
        }
        var delays = ran.Select(e => (Start: started.FirstOrDefault(s => s.Impact.Self == e), Signal: signals.FirstOrDefault(s => s.Impact.Self == e && s.Tag == 0x06C1)))
            .Select(x => x.Start is null || x.Signal is null ? -1 : (long)x.Signal.Step - (long)x.Start.Step).ToArray();
        c.Log.Info($"client-pass: 06c-natives.joined sync native: steps from start to answer {string.Join(", ", delays)}; {Abilities.Natives.All.FirstOrDefault(n => n.Name == "natives-fixture.Count")}");
        c.Expect(delays.Length == 3 && delays.All(d => d == 0), "the synchronous native answered in each program's start step with you in the world",
            string.Join(", ", delays));
        // the answer is the fixture's call count: three runs, three consecutive values (the native ran once each, in order)
        var values = ran.Select(e => signals.FirstOrDefault(s => s.Impact.Self == e && s.Tag == 0x06C1)?.AsInt ?? -1).ToArray();
        c.Expect(values.Length == 3 && values[0] > 0 && values[1] == values[0] + 1 && values[2] == values[1] + 1,
            "each run's answer is the native's own count, one call per run", string.Join(", ", values));

        // 2. a Before on the vanilla Fireball explosion's one getTransform call
        long calls = 0, offTick = 0;
        var threads = new ConcurrentDictionary<int, byte>();
        var selves = new ConcurrentQueue<EntityId>();  // which explosion each call came from
        var a = Abilities.Natives.Attach(0x7a120ee8, "getTransform", NativeStage.Before, (ref NativeCall n) =>
        {
            Interlocked.Increment(ref calls);
            selves.Enqueue(n.Self);
            if (Environment.CurrentManagedThreadId != tick) Interlocked.Increment(ref offTick);
            threads.TryAdd(Environment.CurrentManagedThreadId, 0);
        }, site: 0xfcce4e0a);
        if (!c.Expect(a.Ok, "a Before attached to the Fireball explosion's getTransform", a.Reason ?? "attached"))
        {
            await c.Verdict();
            return;
        }
        c.OnEnd(() => a.Handle!.Dispose());
        c.Instruct("Three fireball explosions go off about 12 m in front of you, one every 3 seconds (stand still and face " +
                   "an open spot). Kiln only counts them. Do they look and sound like the game's own fireball, with nothing " +
                   "missing or added? Answer at the end.");
        await c.Delay(TimeSpan.FromSeconds(3));
        var booms = new List<EntityId>();
        for (var i = 0; i < 3; i++)
        {
            if (Ahead(c, 12) is not { } at) break;
            booms.Add(await Abilities.RunImpact("Explosion_Player_Spell_Fireball_1", at));
            await c.Delay(TimeSpan.FromSeconds(3));
        }
        a.Handle!.Dispose();
        c.Log.Info($"client-pass: 06c-natives.joined attachment: {Interlocked.Read(ref calls)} call(s), {Interlocked.Read(ref offTick)} off the tick thread, " +
                   $"{threads.Count} thread(s)");
        var seen = selves.ToArray();
        c.Expect(booms.Count == 3 && seen.Length == 3 && booms.All(b => seen.Count(s => s == b) == 1),
            "one attached call per explosion, each from that explosion",
            $"explosions {string.Join(", ", booms)}; calls from {string.Join(", ", seen)}");
        await c.Verdict();
    }
}
