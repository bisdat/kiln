// SPDX-License-Identifier: MIT
// Kiln tasks in a game client. For every world the client enters (hosting or joined), once the local player is there:
//   1. 100 tasks started from client code (the game's main thread) each hop back with ToTick() and ToTick(Client): every
//      hop lands on the main thread, in the same client world;
//   2. hosting only: ToTick(WorldKind.Server) lands on the server tick;
//   3. a task reads the client world (WorldConcurrent.Query near the local player);
//   4. a resident task hops every 100 ms until the world ends; when the next world starts, none of its hops ran there.
// "tasks-client-check: PASS|FAIL ...", then "tasks-client-check: world N done: P PASS, F FAIL".
using System.Runtime.InteropServices;
using Kiln;

namespace TasksClientCheck;

public sealed class ClientCheckMod : Mod
{
    private const string Me = "tasks-client-check";
    private int _pass, _fail;
    private uint _session;               // the client world being checked (main thread)
    private bool _running;
    private int _foreignHops;            // resident hops that ran in a client world other than their own
    private uint _lastResidentSession;   // the world of the last resident task

    [StructLayout(LayoutKind.Sequential)]
    private struct Pos { public long X, Y, Z; }

    private void Check(bool ok, string what)
    {
        if (ok) { Interlocked.Increment(ref _pass); Log.Info($"{Me}: PASS {what}"); }
        else { Interlocked.Increment(ref _fail); Log.Error($"{Me}: FAIL {what}"); }
    }

    public override void OnLoad() => Client.Tick += (in TickInfo _) => OnClientTick();

    private void OnClientTick()
    {
        if (_running || !Client.InGame || Client.LocalPlayer.IsNone) return;
        var session = Client.ClientWorldSession;
        if (session == _session) return;
        if (_lastResidentSession != 0)
            Check(Volatile.Read(ref _foreignHops) == 0,
                $"world {session}: the last world's resident task (world {_lastResidentSession}) ran {_foreignHops} hop(s) here (0 expected)");
        _session = session;
        _running = true;
        _ = Run(session).ContinueWith(t =>
        {
            if (t.Exception != null) Check(false, $"the check threw: {t.Exception.InnerException}");
            Log.Info($"{Me}: world {session} ({(Client.IsHosting ? "hosting" : "joined")}) done: {_pass} PASS, {_fail} FAIL");
            _running = false;
        });
    }

    private async Task Run(uint session)
    {
        var hosting = Client.IsHosting;
        // 1. hops from client code come back to the main thread, in this world
        var tasks = Enumerable.Range(0, 100).Select(i => Context.Tasks.Run($"client-hop-{i}", async ctx =>
        {
            long s = 0;
            for (var j = 0; j < 10_000; j++) s += j ^ i;
            var off = !Client.IsClientThread;
            await ctx.ToTick();
            var a = Client.IsClientThread && Client.ClientWorldSession == session;
            await ctx.ToTick(WorldKind.Client);
            var b = Client.IsClientThread && Client.ClientWorldSession == session;
            return off && a && b && ctx.TickWorld == WorldKind.Client;
        })).ToList();
        var ok = 0;
        foreach (var t in tasks) if (await t) ok++;
        Check(ok == 100, $"world {session}: 100 tasks started from client code ran off the main thread and hopped back to it in this world ({ok} of 100)");

        // 2. a host's task reaches the server tick too
        if (hosting)
        {
            var server = await Context.Tasks.Run("client-to-server", async ctx =>
            {
                await ctx.ToTick(WorldKind.Server);
                var onServer = Game.IsTickThread;
                await ctx.ToTick(WorldKind.Client);
                return onServer && Client.IsClientThread;
            });
            Check(server, $"world {session}: hosting: ToTick(Server) lands on the server tick, ToTick(Client) back on the main thread");
        }

        // 3. a task reads the client world near the local player
        var player = Client.LocalPlayer;
        var read = await Context.Tasks.Run("client-read", ctx =>
        {
            var open = WorldConcurrent.Open(player, WorldKind.Client);
            if (!open.IsOk) return Task.FromResult($"open the local player in the client world: {open.Status}");
            var at = WorldConcurrent.Read<Pos>(open.Value, "CurrentTransform");
            if (!at.IsOk) return Task.FromResult($"its CurrentTransform: {at.Status}");
            var where = new WorldPosition(at.Value.X / 4294967296.0, at.Value.Y / 4294967296.0, at.Value.Z / 4294967296.0);
            var refs = new ConcurrentEntity[256];
            var q = WorldConcurrent.Query(new EntityQuery(WorldKind.Client, "CurrentTransform") { Center = where, Radius = 30 }, refs);
            for (var i = 0; i < 5 && q.Status is ReadStatus.Changed or ReadStatus.TooSmall; i++)
            {
                if (q.Status == ReadStatus.TooSmall) refs = new ConcurrentEntity[q.Required + 16];
                q = WorldConcurrent.Query(new EntityQuery(WorldKind.Client, "CurrentTransform") { Center = where, Radius = 30 }, refs);
            }
            return Task.FromResult(q.Status == ReadStatus.Ok && q.Count > 0 ? "" : $"query near {where}: {q.Status}, {q.Count}");
        });
        Check(read.Length == 0 || !WorldConcurrent.IsAvailable,
            $"world {session}: a task reads the client world near the local player{(WorldConcurrent.IsAvailable ? "" : " (skipped: world.ecs_read is off on this build)")}{(read.Length > 0 ? ": " + read : "")}");

        // 4. a resident task: its hops must stay in this world
        _lastResidentSession = session;
        Volatile.Write(ref _foreignHops, 0);
        Context.Tasks.Run("client-resident", async ctx =>
        {
            while (!ctx.Cancellation.IsCancellationRequested)
            {
                await Task.Delay(100, ctx.Cancellation);
                await ctx.ToTick();
                if (Client.ClientWorldSession != session) Interlocked.Increment(ref _foreignHops);
            }
        });
        Log.Info($"{Me}: world {session}: a resident task hops every 100 ms; leave the world and enter another to check it stays behind");
    }
}
