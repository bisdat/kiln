// SPDX-License-Identifier: MIT
// Package 14: Kiln programs of a mod's own in this player's client world and prediction storage (world.programs.client, on
// since 0.2.0-dev.29 without a run in a real game; these checks are the record of that). The programs are the fixture
// mod's (mods/tests/client-programs-fixture: it binds its own declarations in the main menu, counts their calls per
// client world session and writes "client-programs-fixture: session ..." lines to kiln.log), the EMF half the probe's
// (mods/tests/emf-client-test, an EMF mod installed by hand). The checks read those lines and kiln.world's own, so a
// check can cover worlds the player left already. In the package review's order (out/14/REVIEW-codex.md):
//   programs.client-baseline   no client declaration at all: the client worlds registered nothing (the fixture off)
//   14-client-world-programs   hosted: SYSTEM on the main thread, an entity program on the workers, both sides, a name
//                              no world has refused, the event path
//   programs.client-joined     a joined client's session: prediction counts required there
//   14-q4                      the client world's event types by name
//   programs.client-emf        EMF register_program, register_program_def and on_event in the client world (the probe)
//   programs.client-visual     a harmless visual write: a barrel drawn 1 m higher, with its bytes and its event
//   programs.client-rejoin     leave and join again with the fixture's programs running; an unload and a load with them
// Not here, manual (the notes): a second, vanilla client watching the visual write; an exit with programs running.
using System.Text.RegularExpressions;
using Kiln;
using Kiln.Testing;
using static Kiln.Testing.ClientCheckTags;

namespace ClientPass;

public sealed partial class ClientPassMod
{
    private const int ClientWorldProgramsOrder = 2000 + (28140 - 28000) * 10;
    private const string Fixture = "client-programs-fixture";
    private const string NoSuchComponent = "KilnClientProgramsFixtureNoSuchComponent";

    // One fixture summary line: its session, hosted or joined, its state and each program's counts.
    private sealed record Summary(uint Session, bool Joined, string State, ulong Steps, string Line)
    {
        public (long Calls, long Client, long Prediction, long Server, long OffMain, int Threads) Of(string program)
        {
            var m = Regex.Match(Line, Regex.Escape(program) + @" (\d+) \(client (\d+), prediction (\d+), server (\d+); off the main thread (\d+), (\d+) thread");
            return m.Success
                ? (long.Parse(m.Groups[1].Value), long.Parse(m.Groups[2].Value), long.Parse(m.Groups[3].Value), long.Parse(m.Groups[4].Value),
                   long.Parse(m.Groups[5].Value), int.Parse(m.Groups[6].Value))
                : (0, 0, 0, 0, 0, 0);
        }

        public long Number(string pattern) => Regex.Match(Line, pattern) is { Success: true } m ? long.Parse(m.Groups[1].Value) : 0;
    }

    private static List<Summary> Summaries(string[] log) =>
        [.. log.Select(l => Regex.Match(l, Fixture + @": session (\d+) (hosted|joined) (\w+) after (\d+) client steps: (.*)$"))
            .Where(m => m.Success)
            .Select(m => new Summary(uint.Parse(m.Groups[1].Value), m.Groups[2].Value == "joined", m.Groups[3].Value,
                ulong.Parse(m.Groups[4].Value), m.Groups[5].Value))];

    private static async Task<bool> NeedClientPrograms(CheckContext c)
    {
        if (!await NeedOpen(c, EcsPrograms.ClientWorldCapability) || !await NeedPlayer(c)) return false;
        if (Capabilities.Is(EcsPrograms.ClientWorldCapability)) return true;
        await c.Skip($"{EcsPrograms.ClientWorldCapability} is unavailable: {Capabilities.Why(EcsPrograms.ClientWorldCapability)}");
        return false;
    }

    // ---------------------------------------------------------------------------------------- 1. the baseline ----

    [ClientCheck("programs.client-baseline", "No client declaration: the client worlds get nothing of Kiln's programs", Area = "Systems",
        Since = Dev29, Needs = ClientCheckNeeds.Hosted, Order = ClientWorldProgramsOrder + 0)]
    public async Task ClientProgramsBaseline(CheckContext c)
    {
        if (!await NeedOpen(c, EcsPrograms.ClientWorldCapability) || !await NeedPlayer(c)) return;
        if (Mods.IsLoaded(Fixture))
        {
            c.Instruct($"This check needs a start without {Fixture} (no mod declares a client program then). Take it out of the " +
                       "profile, start the game, go menu, host, leave, join a server, leave, host again, then run this check.");
            await c.Skip($"{Fixture} is loaded");
            return;
        }
        if (BootLog(c, "kiln.log") is not { } log)
        {
            await c.Verdict();
            return;
        }
        var worlds = log.Count(l => l.Contains("RegisterAllPrograms for a world of kind 2: not the server world; no Kiln programs there", StringComparison.Ordinal));
        c.Expect(worlds >= 3, "this start's client worlds (host, join, host again) each kept the game's table as the game made it",
            $"{worlds} client world(s) with kiln.world's \"no Kiln programs there\" line");
        c.Expect(!log.Any(l => l.Contains("registered into the client world", StringComparison.Ordinal)), "no Kiln program registered into a client world", "");
        c.Expect(!log.Any(l => l.Contains("client-world programs are off until the game restarts", StringComparison.Ordinal)), "no latch", "");
        c.Expect(true, "world.programs.client in this start", Capabilities.Why(EcsPrograms.ClientWorldCapability));
        c.Instruct("Through that sequence: did loading, frame pacing and play look as without Kiln's client programs (a vanilla " +
                   "player joining you is best)? Pass if they did.");
        await c.Verdict();
    }

    // ------------------------------------------------------------------- 2. hosted: the programs and their threads ----

    [ClientCheck("14-client-world-programs", "Programs of a mod's own in the client world and the prediction storage", Area = "Systems",
        Since = Dev29, Needs = ClientCheckNeeds.Hosted, Order = ClientWorldProgramsOrder + 1)]
    public async Task ClientWorldPrograms(CheckContext c)
    {
        if (!await NeedClientPrograms(c) || !await Need(c, Fixture)) return;
        var session = Client.ClientWorldSession;
        c.Instruct("The fixture's programs count in your client world now. Walk and turn around for about 15 seconds.");
        await c.Delay(TimeSpan.FromSeconds(15));
        if (BootLog(c, "kiln.log") is not { } log)
        {
            await c.Verdict();
            return;
        }
        var s = Summaries(log).LastOrDefault(x => x.Session == session && !x.Joined);
        if (!c.Expect(s is not null, "the fixture wrote a summary of this client world", $"session {session}"))
        {
            await c.Verdict();
            return;
        }
        c.Log.Info($"client-pass: 14-client-world-programs {s!.Line}");
        var sys = s.Of("system");
        var ent = s.Of("entities");
        var bothClient = s.Of("both client");
        var bothServer = s.Of("both server");
        var pred = s.Of("prediction");
        c.Expect(sys.Client > 300 && sys.OffMain == 0, "the SYSTEM program ran every client step, on the main thread", $"{sys}");
        c.Expect(ent.Client > 0 && ent.OffMain > 0 && ent.Threads > 1, "the entity program ran on the game's workers", $"{ent}");
        c.Expect(bothClient.Client > 0 && bothClient.Server == 0 && bothServer.Server > 0 && bothServer.Client == 0,
            "the both program ran in each world with that world's callback", $"client side {bothClient}, server side {bothServer}");
        c.Expect(pred.Client == 0 && sys.Prediction == 0, "the prediction program never ran in the client world, the client program never in prediction",
            $"prediction {pred}, client {sys}");
        c.Expect(true, "the prediction program in a hosted game (programs.client-joined requires it joined)", $"{pred}");
        c.Expect(s.Line.Contains($"refused: cpf_refused: read: unknown component '{NoSuchComponent}'", StringComparison.Ordinal),
            "a program naming a component no world has was refused for the client world, with the name", s.Line[(s.Line.IndexOf("refused:", StringComparison.Ordinal))..]);
        var registered = log.Count(l => l.Contains("program kiln_managed_", StringComparison.Ordinal) && l.Contains("registered into the client world as #", StringComparison.Ordinal));
        c.Expect(registered >= 4, "kiln.log: \"program … registered into the client world as #N\"", $"{registered} line(s)");
        c.Expect(log.Any(l => l.Contains($"{Fixture}: refused (event, thread main)", StringComparison.Ordinal) ||
                              l.Contains("bound, refused by the world that runs now", StringComparison.Ordinal)),
            "the refusal reached the mod: TryBind's answer, or EcsProgramRefusedEvent on the main thread", "");
        c.Instruct("While the programs ran: did the game look and run as before (no stutter, nothing drawn wrong)? Pass if it did.");
        await c.Verdict();
    }

    // ----------------------------------------------------------------------- 3. a joined client: prediction ----

    [ClientCheck("programs.client-joined", "A joined client: the client programs and the prediction storage's", Area = "Systems",
        Since = Dev29, Needs = ClientCheckNeeds.Hosted, Order = ClientWorldProgramsOrder + 2)]
    public async Task ClientProgramsJoined(CheckContext c)
    {
        if (!await NeedClientPrograms(c) || !await Need(c, Fixture)) return;
        if (BootLog(c, "kiln.log") is not { } log)
        {
            await c.Verdict();
            return;
        }
        var joined = Summaries(log).Where(x => x.Joined && x.Steps >= 1200).ToList();
        if (joined.Count == 0)
        {
            c.Instruct($"With {Fixture} in the profile: join a server (a friend's, or a Kiln lab), walk for 30 seconds, leave, host " +
                       "a world again and run this check (it reads what the fixture wrote while you were joined).");
            await c.Skip("no joined session of 20 s or more in this start yet");
            return;
        }
        var s = joined.Last();
        c.Log.Info($"client-pass: programs.client-joined {s.Line}");
        var sys = s.Of("system");
        var pred = s.Of("prediction");
        c.Expect(sys.Client > 600 && sys.OffMain == 0, "joined: the SYSTEM program ran every client step on the main thread", $"{sys}");
        c.Expect(s.Of("entities") is var e && e.Client > 0 && e.OffMain > 0, "joined: the entity program ran on the workers", $"{s.Of("entities")}");
        c.Expect(pred.Prediction > 0 && pred.OffMain == 0 && pred.Client == 0,
            "joined: the prediction program ran in the prediction storage, on the main thread, never in the client world", $"{pred}");
        c.Expect(s.Of("both server").Calls == 0, "joined: no server world here, so no server side ran", $"{s.Of("both server")}");
        c.Instruct("While you were joined: did the game look and run as before? Pass if it did.");
        await c.Verdict();
    }

    // --------------------------------------------------------------------------- 4. the client world's events ----

    [ClientCheck("14-q4", "The client world's event types by name", Area = "Systems", Since = Dev29, Needs = ClientCheckNeeds.Hosted,
        Order = ClientWorldProgramsOrder + 3)]
    public async Task ClientWorldEventTypes(CheckContext c)
    {
        if (!await NeedClientPrograms(c) || !await Need(c, Fixture)) return;  // kiln.world lists them while a client program exists
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
        c.Expect(client.Contains("NotifyEnvFxEvent") && client.Contains("HitEvent"),
            "the client world has the two events programs.client-emf listens for", "");
        var onlyClient = client.Except(server).Order(StringComparer.Ordinal).ToArray();
        var onlyServer = server.Except(client).Order(StringComparer.Ordinal).ToArray();
        // shown, not required: which names differ is the question's answer, not a property Kiln can be held to
        c.Expect(true, "event types only the client world has", onlyClient.Length == 0 ? "none" : string.Join(", ", onlyClient));
        c.Expect(true, "event types only the server world has", onlyServer.Length == 0 ? "none" : string.Join(", ", onlyServer));
        c.Log.Info($"client-pass: 14-q4 client world {client.Count} event types, server world {server.Count}; only client: " +
                   $"{string.Join(", ", onlyClient)}; only server: {string.Join(", ", onlyServer)}");
        await c.Verdict();
    }

    // ------------------------------------------------------------------------------------------ 5. EMF ----

    [ClientCheck("programs.client-emf", "EMF programs and on_event in the client world", Area = "Frameworks", Since = Dev29,
        Needs = ClientCheckNeeds.Hosted, Order = ClientWorldProgramsOrder + 4)]
    public async Task ClientProgramsEmf(CheckContext c)
    {
        if (!await NeedClientPrograms(c)) return;
        if (BootLog(c, "kiln.log") is not { } log0 || !log0.Any(l => l.Contains("emf_client_probe", StringComparison.Ordinal) && l.Contains("loaded:", StringComparison.Ordinal)))
        {
            c.Instruct("This check needs the EMF probe: copy emf_client_probe.dll and mods/tests/emf-client-test/mod.json into " +
                       "<game>/emf/mods/emf_client_probe/ (kiln-emf loads it), start the game and host a world.");
            await c.Skip("emf_client_probe is not loaded");
            return;
        }
        c.Instruct("Walk for 15 seconds (footsteps send NotifyEnvFxEvent), and hit a tree, a rock or a training dummy twice " +
                   "(HitEvent).");
        await c.Delay(TimeSpan.FromSeconds(20));
        if (BootLog(c, "kiln.log") is not { } log)
        {
            await c.Verdict();
            return;
        }
        var line = log.LastOrDefault(l => l.Contains("emf_client_probe", StringComparison.Ordinal) && l.Contains("client world after", StringComparison.Ordinal));
        if (!c.Expect(line is not null, "the probe wrote its client world line", ""))
        {
            await c.Verdict();
            return;
        }
        c.Log.Info($"client-pass: programs.client-emf {line}");
        long N(string pattern) => Regex.Match(line!, pattern) is { Success: true } m ? long.Parse(m.Groups[1].Value) : -1;
        c.Expect(N(@"program (\d+),") > 300, "register_program(EMF_WORLD_CLIENT) ran every client step", line!);
        c.Expect(N(@"definition (\d+);") > 300, "register_program_def(EMF_WORLD_CLIENT) with HitEvent ran every client step", "");
        c.Expect(N(@"NotifyEnvFxEvent (\d+) ") > 0 && N(@"NotifyEnvFxEvent \d+ \((\d+) bytes") > 0 && N(@"NotifyEnvFxEvent [^;]*?, (\d+) off the main thread") == 0,
            "on_event(EMF_WORLD_CLIENT, NotifyEnvFxEvent): delivered with its payload, on the main thread", "");
        c.Expect(N(@"HitEvent (\d+) ") > 0 && N(@"HitEvent [^;]*?, (\d+) off the main thread") == 0,
            "on_event(EMF_WORLD_CLIENT, HitEvent): delivered on the main thread", "");
        c.Expect(N(@"KilnClientPassNoSuchEvent (\d+) ") == 0 &&
                 log.Any(l => l.Contains("emf_evc_KilnClientPassNoSuchEvent refused for the client world", StringComparison.Ordinal)),
            "on_event for an event the client world lacks: its relay refused for that world by name, nothing delivered", "");
        c.Expect(line!.Contains("on_tick on the main thread", StringComparison.Ordinal), "the probe's client on_tick ran on the main thread", "");
        await c.Verdict();
    }

    // ---------------------------------------------------------------------------- 6. a harmless visual write ----

    [ClientCheck("programs.client-visual", "A client program draws a barrel 1 m higher in this game only", Area = "Systems",
        Since = Dev29, Tags = [Visual], Needs = ClientCheckNeeds.Hosted, Order = ClientWorldProgramsOrder + 5)]
    public async Task ClientProgramsVisual(CheckContext c)
    {
        if (!await NeedClientPrograms(c) || !await Need(c, Fixture)) return;
        if (Ahead(c, 3) is not { } at)
        {
            await c.Skip("the player's place is not known");
            return;
        }
        var barrel = await SpawnHere(c, "Prop_Deco_Steppeprops_Barrel_01", at);
        if (barrel.IsNone)
        {
            await c.Verdict();
            return;
        }
        await c.Delay(TimeSpan.FromSeconds(2));
        var p = World.Position(barrel) ?? at;
        c.OnEnd(() => Mods.Send(Fixture, "visual-target", null));
        c.Expect(Mods.Send(Fixture, "visual-target", new[] { p.X, p.Y, p.Z }), "the fixture takes the barrel's place as its target", $"{p}");
        await c.Delay(TimeSpan.FromSeconds(3));
        c.Instruct("A barrel stands 3 m ahead of you. Is it drawn about 1 m above the ground (floating), while it is still where it " +
                   "was for the game (walk into it: you bump into it on the ground)? Pass if it floats.");
        await c.Delay(TimeSpan.FromSeconds(10));
        if (BootLog(c, "kiln.log") is { } log && Summaries(log).LastOrDefault(x => x.Session == Client.ClientWorldSession) is { } s)
        {
            c.Log.Info($"client-pass: programs.client-visual {s.Line}");
            var events = s.Number(@"writes (\d+) events");
            c.Expect(s.Of("visual").Client > 0, "the fixture's write program ran", $"{s.Of("visual")}");
            c.Expect(events > 0 && s.Number(@"\((\d+) off the main thread, \d+ with y") == 0 && s.Number(@"off the main thread, (\d+) with y \+ 1 m") == events,
                "ClientComponentWrittenEvent for each write, on the main thread, before and after bytes 1 m apart in y",
                s.Line[(s.Line.IndexOf("writes ", StringComparison.Ordinal))..]);
        }
        else c.Expect(false, "the fixture wrote a summary of this client world", "");
        Mods.Send(Fixture, "visual-target", null);
        c.Instruct("The target is cleared now: the barrel should be back on the ground. A second player without Kiln (a vanilla " +
                   "client) should have seen it on the ground all along (a manual step: the notes).");
        await c.Verdict();
    }

    // ------------------------------------------------------------------- 7. leave, rejoin, unload, load again ----

    [ClientCheck("programs.client-rejoin", "Client programs through leaving, joining again, an unload and a load", Area = "Systems",
        Since = Dev29, Needs = ClientCheckNeeds.Hosted, Order = ClientWorldProgramsOrder + 6)]
    public async Task ClientProgramsRejoin(CheckContext c)
    {
        if (!await NeedClientPrograms(c) || !await Need(c, Fixture)) return;
        if (BootLog(c, "kiln.log") is not { } log)
        {
            await c.Verdict();
            return;
        }
        var running = Summaries(log).Where(x => x.Of("system").Client > 0).Select(x => x.Session).Distinct().ToList();
        if (running.Count < 3)
        {
            c.Instruct($"With {Fixture} in the profile: host a world, leave, join a server, leave, host again (wait 10 s in each " +
                       "world), then run this check.");
            await c.Skip($"{running.Count} client world(s) with the fixture's programs running in this start; three are needed");
            return;
        }
        c.Expect(true, "client worlds with the programs running", string.Join(", ", running));
        var stops = log.Where(l => l.Contains("client world stops:", StringComparison.Ordinal) && l.Contains("registration(s) closed", StringComparison.Ordinal)).ToList();
        c.Expect(stops.Count >= 2 && stops.All(l => l.Contains("(0 call(s) running at the stop", StringComparison.Ordinal)),
            "every client world stopped with no call of a program inside", $"{stops.Count} stop(s)");
        c.Expect(!log.Any(l => l.Contains("still running after", StringComparison.Ordinal) || l.Contains("off until the game restarts", StringComparison.Ordinal)),
            "no call outlived its world and nothing latched", "");
        c.Expect(log.Count(l => l.Contains("program kiln_managed_", StringComparison.Ordinal) && l.Contains("registered into the client world as #", StringComparison.Ordinal)) >= 4 * running.Count,
            "each of those client worlds registered the programs anew", "");
        // an unload and a load while the programs run in this world (the reload runs on this tick thread; the fixture
        // binds on the main thread through Client.Post)
        var before = log.Length;
        var r = await Mods.ReloadAsync(Fixture);
        c.Expect(r.Ok, "the fixture reloads", r.ToString());
        await c.Delay(TimeSpan.FromSeconds(12));
        if (BootLog(c, "kiln.log") is not { } after)
        {
            await c.Verdict();
            return;
        }
        var tail = after.Skip(before).ToArray();
        c.Expect(tail.Any(l => l.Contains($"{Fixture}: unbound", StringComparison.Ordinal) && l.Contains(" 0 deferred", StringComparison.Ordinal)),
            "the unload unbound every binding with no call left inside", tail.FirstOrDefault(l => l.Contains($"{Fixture}: unbound", StringComparison.Ordinal)) ?? "no line");
        c.Expect(tail.Count(l => l.Contains($"{Fixture}: cpf_system (Client) bound", StringComparison.Ordinal)) == 1,
            "the new instance bound again in the world that runs", "");
        c.Expect(Summaries(tail).LastOrDefault() is { } s && s.Of("system").Client > 0, "and its programs run", "");
        c.Instruct("Did the game run as before through leaving, joining, hosting and the reload? Pass if it did.");
        await c.Verdict();
    }
}
