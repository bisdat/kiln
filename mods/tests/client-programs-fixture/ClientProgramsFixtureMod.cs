// SPDX-License-Identifier: MIT
// The fixture of the guided checks for Kiln programs in a client's worlds (package 14; mods/tests/client-pass
// Dev28ClientWorldPrograms.cs reads what it writes to kiln.log). In a game client it binds its own declarations
// (kiln.programs.json: a SYSTEM and an entity program, the client side of a both program, a prediction program, one that
// names a component no world has, one that writes RenderTransform) on the game's main thread in the main menu, the
// both program's server side when a hosted world starts, and then, per client world session:
//   - counts each program's calls, the calls off the main thread and the threads they ran on;
//   - writes "client-programs-fixture: session S <hosted|joined> ..." every 600 client steps and once more for the
//     session that ended, at the next session's first step;
//   - while the pass named a target position (Mods.Send "visual-target", a double[3] in metres; null clears it), draws
//     the entity found at it 1 m higher: cpf_entities finds it by its RenderTransform and keeps its bytes, cpf_visual
//     writes them back with y + 1 m (no MarkWritten: this player's game only), and ClientComponentWrittenEvent is
//     counted with its thread and its bytes;
//   - while the pass armed a delay (Mods.Send "delay", an int of milliseconds; 0 disarms; a new client world disarms it),
//     one cpf_entities call per client step sleeps that long on its worker and then returns without touching anything:
//     the deliberately delayed callback of the review's step 5, so that a client world's stop catches a call inside;
//   - at unload, says whether every binding drained.
// A new client world session clears the visual target too (a reloaded world must show the barrel where the game has it).
// On a dedicated server only the both program's server side is bound (world.programs.client is wrong-side there).
using System.Collections.Concurrent;
using System.Runtime.InteropServices;
using Kiln;

namespace ClientProgramsFixture;

[StructLayout(LayoutKind.Sequential, Size = 176)]
internal struct RenderTransformBytes  // keen::ecs::RenderTransform: a WorldTransform first (x, y, z as 32.32 fixed point)
{
    public long X, Y, Z;
}

public sealed class ClientProgramsFixtureMod : Mod
{
    private const string Me = "client-programs-fixture";
    private const long OneMetre = 1L << 32;

    private sealed class Count
    {
        public long Calls, OffMain, InClient, InPrediction, InServer;
        public readonly ConcurrentDictionary<int, byte> Threads = new();

        public void Add(EcsProgramContext c, int main)
        {
            Interlocked.Increment(ref Calls);
            var t = Environment.CurrentManagedThreadId;
            if (t != main) Interlocked.Increment(ref OffMain);
            Threads.TryAdd(t, 0);
            switch (c.World)
            {
                case WorldKind.Client: Interlocked.Increment(ref InClient); break;
                case WorldKind.Prediction: Interlocked.Increment(ref InPrediction); break;
                default: Interlocked.Increment(ref InServer); break;
            }
        }

        public string Text() =>
            $"{Interlocked.Read(ref Calls)} (client {Interlocked.Read(ref InClient)}, prediction {Interlocked.Read(ref InPrediction)}, " +
            $"server {Interlocked.Read(ref InServer)}; off the main thread {Interlocked.Read(ref OffMain)}, {Threads.Count} thread(s))";
    }

    private readonly List<IDisposable> _bindings = [];
    private Count _system = new(), _entities = new(), _bothClient = new(), _bothServer = new(), _prediction = new(), _visual = new();
    private int _main;
    private uint _session;
    private ulong _steps;
    private string _refused = "not refused";
    private long _writeEvents, _writeEventsOffMain, _writeEventBytesOk;
    private string _firstWrite = "none";
    // the visual target: a position in metres (fixed point), the entity found there and its last RenderTransform
    private readonly object _visualLock = new();
    private long[]? _target;
    private uint _targetEntity;
    private RenderTransformBytes _targetBytes;
    private bool _haveBytes;
    // the delayed callback: milliseconds (0: off), the client step it last slept in, how many times it slept
    private int _delayMs;
    private long _delayedStep = -1, _delays, _step;

    public override void OnLoad()
    {
        Context.Messages.On("visual-target", m =>
        {
            lock (_visualLock)
            {
                _target = m.Payload is double[] { Length: 3 } p ? [(long)(p[0] * OneMetre), (long)(p[1] * OneMetre), (long)(p[2] * OneMetre)] : null;
                _targetEntity = 0;
                _haveBytes = false;
            }
            Log.Info($"{Me}: visual target {(m.Payload is double[] { Length: 3 } q ? $"({q[0]:F1}, {q[1]:F1}, {q[2]:F1})" : "cleared")}");
        });
        if (!Capabilities.Is(EcsPrograms.ClientWorldCapability))
        {
            Log.Info($"{Me}: client-world programs are off here: {Capabilities.Why(EcsPrograms.ClientWorldCapability)}");
            return;
        }
        if (Client.IsClientThread) BindClient();
        else Client.Post(BindClient);  // a reload while hosting runs OnLoad on the server's tick: TryBind is the main thread's
        Events.On<EcsProgramRefusedEvent>(e =>
        {
            if (e.ModId != Me) return;
            _refused = $"{e.Key}: {e.Reason}";
            Log.Info($"{Me}: refused (event, thread {(Environment.CurrentManagedThreadId == _main ? "main" : "other")}): {_refused}");
        });
        Events.On<ClientComponentWrittenEvent>(e =>
        {
            if (!e.Writer.StartsWith(Me + "/", StringComparison.Ordinal)) return;
            Interlocked.Increment(ref _writeEvents);
            if (Environment.CurrentManagedThreadId != _main) Interlocked.Increment(ref _writeEventsOffMain);
            if (e.Before.Length >= 24 && e.After.Length >= 24)
            {
                var before = BitConverter.ToInt64(e.Before.Span[8..]);
                var after = BitConverter.ToInt64(e.After.Span[8..]);
                if (after - before == OneMetre) Interlocked.Increment(ref _writeEventBytesOk);
                if (Interlocked.Read(ref _writeEvents) == 1)
                    _firstWrite = $"{e.Component} of entity {e.Entity} ({e.World}): y {before / (double)OneMetre:F2} -> {after / (double)OneMetre:F2} m";
            }
        });
        Context.Messages.On("delay", m =>
        {
            var ms = m.Payload is int v ? Math.Clamp(v, 0, 10_000) : 0;
            Volatile.Write(ref _delayMs, ms);
            Log.Info($"{Me}: delay {(ms > 0 ? $"armed: one cpf_entities call per client step sleeps {ms} ms on its worker" : "off")}");
        });
        Client.Tick += OnClientTick;
    }

    [Subscribe]
    private void OnServerStarted(ServerStartedEvent e)
    {
        // the both program's server side: kiln.world binds a server program on the tick thread (the hosted world's)
        var b = EcsPrograms.TryBind("cpf_both", c => _bothServer.Add(c, _main), WorldKind.Server);
        if (b.Handle is { } h) _bindings.Add(h);
        Log.Info($"{Me}: cpf_both (server world) {(b.Ok ? "bound" : "not bound: " + b.Reason)}");
    }

    private void BindClient()
    {
        _main = Environment.CurrentManagedThreadId;
        Bind("cpf_system", c => _system.Add(c, _main), WorldKind.Client);
        Bind("cpf_entities", Entities, WorldKind.Client);
        Bind("cpf_both", c => _bothClient.Add(c, _main), WorldKind.Client);
        Bind("cpf_prediction", c => _prediction.Add(c, _main), WorldKind.Prediction);
        Bind("cpf_refused", c => { }, WorldKind.Client);
        Bind("cpf_visual", Visual, WorldKind.Client);
    }

    private void Bind(string key, EcsProgramCallback callback, WorldKind world)
    {
        var b = EcsPrograms.TryBind(key, callback, world);
        if (b.Handle is { } h) _bindings.Add(h);
        if (!b.Ok && b.Reason is { } r && key == "cpf_refused") _refused = $"{key}: {r}";
        Log.Info($"{Me}: {key} ({world}) {(b.Ok ? "bound" : b.Handle is null ? "not bound: " + b.Reason : "bound, refused by the world that runs now: " + b.Reason)}");
    }

    // cpf_entities: every entity with a RenderTransform; while a target is named, finds the one at it (0.5 m) and keeps
    // its bytes for cpf_visual, which runs after it in the same step (it writes what this one reads)
    private void Entities(EcsProgramContext c)
    {
        _entities.Add(c, _main);
        var delay = Volatile.Read(ref _delayMs);
        if (delay > 0)
        {
            var step = Interlocked.Read(ref _step);
            if (Interlocked.Exchange(ref _delayedStep, step) != step)
            {
                Interlocked.Increment(ref _delays);
                Thread.Sleep(delay);
                return;  // nothing of the context after the sleep: its world may be gone
            }
        }
        long[]? target;
        lock (_visualLock) target = _target;
        if (target is null || !c.TryRead("RenderTransform", out RenderTransformBytes t)) return;
        if (Math.Abs(t.X - target[0]) > OneMetre / 2 || Math.Abs(t.Z - target[2]) > OneMetre / 2 || Math.Abs(t.Y - target[1]) > 2 * OneMetre)
            return;
        lock (_visualLock)
        {
            _targetEntity = c.Entity.Value;
            _targetBytes = t;
            _haveBytes = true;
        }
    }

    private void Visual(EcsProgramContext c)
    {
        _visual.Add(c, _main);
        RenderTransformBytes v;
        lock (_visualLock)
        {
            if (!_haveBytes || c.Entity.Value != _targetEntity) return;
            v = _targetBytes;
            _haveBytes = false;  // once per step: the next step's cpf_entities reads it again after the game drew it
        }
        v.Y += OneMetre;
        c.TryWrite("RenderTransform", v);
    }

    private void OnClientTick(in TickInfo tick)
    {
        var session = Client.ClientWorldSession;
        Interlocked.Increment(ref _step);
        if (session != _session)
        {
            if (_session != 0) Summary("ended");
            if (Interlocked.Exchange(ref _delayMs, 0) > 0) Log.Info($"{Me}: delay off (a new client world)");
            lock (_visualLock)
            {
                _target = null;
                _targetEntity = 0;
                _haveBytes = false;
            }
            _session = session;
            _steps = 0;
            (_system, _entities, _bothClient, _bothServer, _prediction, _visual) = (new(), new(), new(), new(), new(), new());
            Interlocked.Exchange(ref _writeEvents, 0);
            Interlocked.Exchange(ref _writeEventsOffMain, 0);
            Interlocked.Exchange(ref _writeEventBytesOk, 0);
            _firstWrite = "none";
        }
        if (++_steps % 600 == 0) Summary("running");
    }

    private void Summary(string state) =>
        Log.Info($"{Me}: session {_session} {(Client.IsRemote ? "joined" : "hosted")} {state} after {_steps} client steps: " +
                 $"system {_system.Text()}; entities {_entities.Text()}; both client {_bothClient.Text()}; both server " +
                 $"{_bothServer.Text()}; prediction {_prediction.Text()}; visual {_visual.Text()}; writes {Interlocked.Read(ref _writeEvents)} " +
                 $"events ({Interlocked.Read(ref _writeEventsOffMain)} off the main thread, {Interlocked.Read(ref _writeEventBytesOk)} with y + 1 m), " +
                 $"first: {_firstWrite}; refused: {_refused}; delayed calls {Interlocked.Read(ref _delays)}");

    public override void OnUnload()
    {
        Client.Tick -= OnClientTick;
        if (_session != 0) Summary("unloading");
        var deferred = 0;
        foreach (var b in _bindings)
        {
            b.Dispose();
            if (b is not null && IsPending(b)) deferred++;
        }
        Log.Info($"{Me}: unbound {_bindings.Count} binding(s): {_bindings.Count - deferred} drained, {deferred} deferred");
        _bindings.Clear();
    }

    // a client binding whose call still ran keeps its handle until kiln.world says it drained (EcsPrograms retries)
    private static bool IsPending(IDisposable b) =>
        b.GetType().GetField("Handle", System.Reflection.BindingFlags.NonPublic | System.Reflection.BindingFlags.Instance)?.GetValue(b) is ulong h && h != 0;
}
