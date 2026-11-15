// SPDX-License-Identifier: MIT
// 14-client-world-programs: Kiln programs of a mod's own in this player's client world and in the prediction storage
// (world.programs.client, on since 0.2.0-dev.29 without a run in a real game; this check is the record of that). In a
// hosted game it binds the pass's own client-world declarations (this mod's kiln.programs.json, "world": "client",
// "both", "prediction") on the game's main thread and asserts what the code can see: the client world registered them
// (kiln.log), a SYSTEM program ran every client step on the main thread, an entity program ran on the game's workers, the
// "both" program ran in each world with its own callback, the prediction program never ran in the client world, a name
// the client world lacks was refused for it with the name, and nothing runs after Dispose. What it cannot see: a joined
// client (a joined check's body runs on the server: the same lines are in a joined client's kiln.log, listed in
// docs/kiln/services/world.md), the local-only visual with a second, vanilla viewer, and the EMF matrix.
//
// 14-q4: the client world's event types by name (chapter 14 question 4), from kiln.world's dump at the client world's
// first step (written while a mod declares a client program, as this one does), next to the hosted server world's.
using System.Collections.Concurrent;
using Kiln;
using Kiln.Testing;

namespace ClientPass;

public sealed partial class ClientPassMod
{
    private const int ClientWorldProgramsOrder = 2000 + (28140 - 28000) * 10;
    private const string NoSuchComponent = "KilnClientPassNoSuchComponent";

    private sealed class ProgramCount
    {
        public long Calls, OffMain, InClient, InPrediction, InServer;
        public int MainThread;
        public readonly ConcurrentDictionary<int, byte> Threads = new();

        public void Add(EcsProgramContext c)
        {
            Interlocked.Increment(ref Calls);
            var t = Environment.CurrentManagedThreadId;
            if (t != Volatile.Read(ref MainThread)) Interlocked.Increment(ref OffMain);
            Threads.TryAdd(t, 0);
            switch (c.World)
            {
                case WorldKind.Client: Interlocked.Increment(ref InClient); break;
                case WorldKind.Prediction: Interlocked.Increment(ref InPrediction); break;
                default: Interlocked.Increment(ref InServer); break;
            }
        }

        public override string ToString() =>
            $"{Interlocked.Read(ref Calls)} calls ({Interlocked.Read(ref InClient)} client world, {Interlocked.Read(ref InPrediction)} " +
            $"prediction, {Interlocked.Read(ref InServer)} server world), {Interlocked.Read(ref OffMain)} off the main thread, " +
            $"on {Threads.Count} thread(s)";
    }

    // TryBind for a client's worlds is the main thread's: the body runs on the hosted world's tick thread.
    private static Task<EcsProgramBinding> BindOnMainThread(string key, EcsProgramCallback callback, WorldKind world, ProgramCount count)
    {
        var done = new TaskCompletionSource<EcsProgramBinding>(TaskCreationOptions.RunContinuationsAsynchronously);
        Client.Post(() =>
        {
            Volatile.Write(ref count.MainThread, Environment.CurrentManagedThreadId);
            try { done.SetResult(EcsPrograms.TryBind(key, callback, world)); }
            catch (Exception e) { done.SetException(e); }
        });
        return done.Task;
    }

    [ClientCheck("14-client-world-programs", "Programs of a mod's own in the client world and the prediction storage", Area = "Systems",
        Since = Dev29, Needs = ClientCheckNeeds.Hosted, Order = ClientWorldProgramsOrder + 0)]
    public async Task ClientWorldPrograms(CheckContext c)
    {
        if (!await NeedOpen(c, EcsPrograms.ClientWorldCapability) || !await NeedPlayer(c)) return;
        if (!Capabilities.Is(EcsPrograms.ClientWorldCapability))
        {
            await c.Skip($"{EcsPrograms.ClientWorldCapability} is unavailable: {Capabilities.Why(EcsPrograms.ClientWorldCapability)}");
            return;
        }
        var system = new ProgramCount();
        var entities = new ProgramCount();
        var bothClient = new ProgramCount();
        var bothServer = new ProgramCount();
        var prediction = new ProgramCount();
        var refusedEvents = new ConcurrentQueue<EcsProgramRefusedEvent>();
        using var refusals = Events.On<EcsProgramRefusedEvent>(e => refusedEvents.Enqueue(e));
        var handles = new List<IDisposable>();
        c.OnEnd(() => { foreach (var h in handles) h.Dispose(); });

        async Task<EcsProgramBinding> Bind(string key, ProgramCount count, WorldKind world)
        {
            var b = await BindOnMainThread(key, ctx => count.Add(ctx), world, count);
            if (b.Handle is { } h) handles.Add(h);
            return b;
        }

        var bSystem = await Bind("client_pass_cw_system", system, WorldKind.Client);
        c.Expect(bSystem.Ok, "a SYSTEM program bound for the client world", bSystem.Ok ? "bound" : bSystem.Reason ?? "");
        var bEntities = await Bind("client_pass_cw_entities", entities, WorldKind.Client);
        c.Expect(bEntities.Ok, "an entity program (RenderTransform) bound for the client world", bEntities.Ok ? "bound" : bEntities.Reason ?? "");
        var bBothClient = await Bind("client_pass_cw_both", bothClient, WorldKind.Client);
        c.Expect(bBothClient.Ok, "the client side of a \"both\" program bound", bBothClient.Ok ? "bound" : bBothClient.Reason ?? "");
        var bBothServer = EcsPrograms.TryBind("client_pass_cw_both", ctx => bothServer.Add(ctx), WorldKind.Server);  // the tick thread
        if (bBothServer.Handle is { } bs) handles.Add(bs);
        c.Expect(bBothServer.Ok, "its server side bound on the hosted world's tick thread", bBothServer.Ok ? "bound" : bBothServer.Reason ?? "");
        var bPrediction = await Bind("client_pass_cw_prediction", prediction, WorldKind.Prediction);
        c.Expect(bPrediction.Ok, "a prediction program bound", bPrediction.Ok ? "bound" : bPrediction.Reason ?? "");
        var refusedCount = new ProgramCount();
        var bRefused = await Bind("client_pass_cw_refused", refusedCount, WorldKind.Client);
        c.Expect(!bRefused.Ok && (bRefused.Reason ?? "").Contains(NoSuchComponent, StringComparison.Ordinal),
            $"a program naming {NoSuchComponent} is refused for the client world, with the name", bRefused.Reason ?? "it was taken");

        c.Instruct("Five small programs of the check run in your client world now (they only count). Walk and turn around for " +
                   "about 10 seconds.");
        await c.Delay(TimeSpan.FromSeconds(10));
        c.Log.Info($"client-pass: 14-client-world-programs SYSTEM: {system}");
        c.Log.Info($"client-pass: 14-client-world-programs entities: {entities}");
        c.Log.Info($"client-pass: 14-client-world-programs both, client side: {bothClient}; server side: {bothServer}");
        c.Log.Info($"client-pass: 14-client-world-programs prediction: {prediction}");
        c.Expect(Interlocked.Read(ref system.InClient) > 100 && Interlocked.Read(ref system.OffMain) == 0,
            "the SYSTEM program ran every client step, on the main thread", system.ToString());
        c.Expect(Interlocked.Read(ref entities.InClient) > 0 && Interlocked.Read(ref entities.OffMain) > 0,
            "the entity program ran on the game's workers (a client world runs its programs as tasks)", entities.ToString());
        c.Expect(Interlocked.Read(ref bothClient.InClient) > 0 && Interlocked.Read(ref bothClient.InServer) == 0 &&
                 Interlocked.Read(ref bothServer.InServer) > 0 && Interlocked.Read(ref bothServer.InClient) == 0,
            "the both program ran in each world with that world's own callback", $"client side {bothClient}; server side {bothServer}");
        c.Expect(Interlocked.Read(ref prediction.InClient) == 0 && Interlocked.Read(ref system.InPrediction) == 0,
            "the prediction program never ran in the client world, and the client program never in the prediction storage",
            $"prediction program {prediction}; client program {system}");
        // shown, not required: whether a hosting game's prediction storage runs programs at all is not established (chapter
        // 14 section 8: measured on a joined client only)
        c.Expect(true, "the prediction program's runs in this hosted game (a joined client's are the measured case)", prediction.ToString());
        c.Expect(Interlocked.Read(ref refusedCount.Calls) == 0, "the refused program never ran", refusedCount.ToString());
        if (BootLog(c, "kiln.log") is { } log)
        {
            var registered = log.Count(l => l.Contains("registered into the client world as #", StringComparison.Ordinal));
            c.Expect(registered >= 3, "kiln.log: \"program … registered into the client world as #N\" for the pass's programs",
                $"{registered} line(s)");
            c.Expect(log.Any(l => l.Contains("refused for the client world", StringComparison.Ordinal) && l.Contains(NoSuchComponent, StringComparison.Ordinal)),
                "kiln.log names the refused program's missing component", "");
        }

        foreach (var h in handles) h.Dispose();
        handles.Clear();
        var before = Interlocked.Read(ref system.Calls) + Interlocked.Read(ref entities.Calls) + Interlocked.Read(ref bothClient.Calls) +
                     Interlocked.Read(ref bothServer.Calls) + Interlocked.Read(ref prediction.Calls);
        await c.Delay(TimeSpan.FromSeconds(2));
        var after = Interlocked.Read(ref system.Calls) + Interlocked.Read(ref entities.Calls) + Interlocked.Read(ref bothClient.Calls) +
                    Interlocked.Read(ref bothServer.Calls) + Interlocked.Read(ref prediction.Calls);
        c.Expect(after == before, "nothing runs after Dispose", $"{after - before} call(s) after it");
        c.Instruct("While the programs ran: did the game look and run as before (no stutter, nothing drawn wrong)? Pass if it " +
                   "did, Fail if not.");
        await c.Verdict();
    }

    [ClientCheck("14-q4", "The client world's event types by name", Area = "Systems", Since = Dev29, Needs = ClientCheckNeeds.Hosted,
        Order = ClientWorldProgramsOrder + 1)]
    public async Task ClientWorldEventTypes(CheckContext c)
    {
        if (!await NeedOpen(c, EcsPrograms.ClientWorldCapability) || !await NeedPlayer(c)) return;
        if (!Capabilities.Is(EcsPrograms.ClientWorldCapability))
        {
            await c.Skip($"{EcsPrograms.ClientWorldCapability} is unavailable: {Capabilities.Why(EcsPrograms.ClientWorldCapability)}");
            return;
        }
        if (BootLog(c, "kiln.log") is not { } log)
        {
            await c.Verdict();
            return;
        }
        static HashSet<string> Names(IEnumerable<string> lines, string world) => lines
            .Where(l => l.Contains(world + " 0x", StringComparison.Ordinal) && l.Contains("event map (question 4)", StringComparison.Ordinal))
            .SelectMany(l => l[(l.IndexOf("]: ", StringComparison.Ordinal) + 3)..].Split(", ", StringSplitOptions.RemoveEmptyEntries))
            .Select(n => n.Trim()).ToHashSet(StringComparer.Ordinal);
        var client = Names(log, "client world");
        var server = Names(log, "server world");
        c.Expect(client.Count > 0, "kiln.log lists the client world's event types (kiln.world, at its first step)", $"{client.Count} names");
        c.Expect(server.Count > 0, "and the hosted server world's", $"{server.Count} names");
        var onlyClient = client.Except(server).Order(StringComparer.Ordinal).ToArray();
        var onlyServer = server.Except(client).Order(StringComparer.Ordinal).ToArray();
        // shown, not required: which names differ is the question's answer, not a property Kiln can be held to
        c.Expect(true, "event types only the client world has", onlyClient.Length == 0 ? "none" : string.Join(", ", onlyClient));
        c.Expect(true, "event types only the server world has", onlyServer.Length == 0 ? "none" : string.Join(", ", onlyServer));
        c.Log.Info($"client-pass: 14-q4 client world {client.Count} event types, server world {server.Count}; only client: " +
                   $"{string.Join(", ", onlyClient)}; only server: {string.Join(", ", onlyServer)}");
        await c.Verdict();
    }
}
