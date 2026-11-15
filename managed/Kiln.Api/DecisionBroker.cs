// SPDX-License-Identifier: MIT
// The managed half of kiln.decisions (native/services/decisions; the C table and its protocol are in
// native/core/abi/services/kiln_decisions.h, the step's phases in kiln_window_clock.h).
//
// The native service holds what the game is about to do and files a request; exactly one consumer reads the requests,
// decides and answers. This file is that consumer for the managed host: once a step, in Pre, DecisionBroker reads the
// frozen request batch, hands each request to the producer of its kind (a class of the service that owns the event:
// Kiln.Combat for a death, Kiln.Gameplay for a pickup, ...), which posts the cancelable event on the bus and says what
// the handlers decided; the broker answers, and posts one DecisionCompletedEvent for every outcome record.
//
// Rules kept here:
//  - A request is dispatched once. The native batch is not consumed by a read, so a read that is repeated (a failed
//    acknowledge) brings the same records again: the broker remembers the ids of the open batch.
//  - An outcome is posted once, for the same reason.
//  - Nothing here runs outside the tick thread's Pre, and no handler ever runs on a game worker: hooks only copy a
//    request and read a verdict.
//  - A producer that throws, or a kind nobody produces, answers "allow": the game goes on (fail open).
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Text;

[assembly: InternalsVisibleTo("Kiln.Decisions.Tests")]

namespace Kiln;

/// <summary>Mirrors of <c>kiln_decisions.h</c> and <c>kiln_window_clock.h</c> (v1). The sizes are the header's
/// (<see cref="LayoutMatchesNative"/>, pinned by a test).</summary>
internal static unsafe class DecisionsNative
{
    public const string Service = "kiln.decisions";
    public const string ClockService = "kiln.window_clock";
    public const uint Version = 1;

    // KilnResult (native/core/abi/kiln_api.h)
    public const int Ok = 0, InvalidArgument = 1, NotFound = 2, Unavailable = 3, WrongThread = 4, BufferTooSmall = 5;

    public const uint KindDeath = 1, KindWalkover = 2, KindEquipSingle = 16, KindEquipCollection = 17, KindEquipSet = 18,
        KindBuildPiece = 32, KindTerraformReserved = 33;

    public const uint KeySpawnTime = 1, KeyPlayerSession = 2;
    public const uint FlagHasPosition = 1, FlagDryRun = 2;
    public const uint OutcomeHealthKnown = 1;
    public const int MaxInterest = 4096, PayloadCapacity = 1024;

    // KilnWindowStateV1.phase
    public const uint PhaseNone = 0, PhasePre = 1, PhasePreparing = 2, PhaseSim = 3, PhaseAfterJobs = 4, PhasePost = 5, PhaseStopping = 6;

    [StructLayout(LayoutKind.Sequential)]
    public struct EntityKey
    {
        public uint Entity, Flags;
        public ulong SpawnTime, LifeSerial, PlayerHandle, JoinedStep;

        public readonly EventEntityKey ToPublic() => new(Entity, (Flags & KeySpawnTime) != 0, SpawnTime, LifeSerial, PlayerHandle, JoinedStep);

        public static EntityKey From(EventEntityKey key) => new()
        {
            Entity = key.Entity,
            Flags = (key.HasSpawnTime ? KeySpawnTime : 0) | (key.PlayerHandle != 0 || key.JoinedStep != 0 ? KeyPlayerSession : 0),
            SpawnTime = key.SpawnTime,
            LifeSerial = key.LifeSerial,
            PlayerHandle = key.PlayerHandle,
            JoinedStep = key.JoinedStep,
        };
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct Request
    {
        public uint Size, Version, Kind, PayloadBytes;
        public ulong RequestId, WorldEpoch, CaptureStep, DeadlineStep;
        public EntityKey Subject, Actor;
        public ulong ExposureEpoch;
        public uint Flags, Reserved;
        public fixed byte Payload[PayloadCapacity];
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct Interest
    {
        public uint Size, Kind, AllEntities, Reserved;
        public ulong RegistryRevision;
        public uint* Entities;
        public uint EntityCount, Reserved2;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct Answer
    {
        public uint Size, Kind;
        public ulong RequestId, WorldEpoch;
        public uint Allow;
        public int SurvivalHealth;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct Receipt
    {
        public uint Size, Accepted, TerminalStatus, Reserved;
        public fixed byte Reason[192];
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct Outcome
    {
        public uint Size, Version, Kind, Status;
        public ulong RequestId, WorldEpoch, Step;
        public EntityKey Subject, Actor;
        public int RequestedHealth, HealthBefore, AppliedHealth;
        public uint Flags;
        public fixed byte Reason[192];
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct Table
    {
        public ServiceHeader Header;
        public delegate* unmanaged[Cdecl]<ulong> SupportedKinds;
        public delegate* unmanaged[Cdecl]<ulong, int> Attach;
        public delegate* unmanaged[Cdecl]<uint, EntityKey*, double*, uint*, int> ResolveEntity;
        public delegate* unmanaged[Cdecl]<ulong, Interest*, int> Interest;
        public delegate* unmanaged[Cdecl]<ulong, Request*, ulong*, int> EnqueuePrepared;
        public delegate* unmanaged[Cdecl]<ulong, ulong*, uint*, int> BeginRequests;
        public delegate* unmanaged[Cdecl]<ulong, ulong, Request*, uint, uint, uint*, int> ReadRequests;
        public delegate* unmanaged[Cdecl]<ulong, ulong, int> AckRequests;
        public delegate* unmanaged[Cdecl]<ulong, Answer*, Receipt*, int> Answer;
        public delegate* unmanaged[Cdecl]<ulong, ulong*, uint*, int> BeginOutcomes;
        public delegate* unmanaged[Cdecl]<ulong, ulong, Outcome*, uint, uint, uint*, int> ReadOutcomes;
        public delegate* unmanaged[Cdecl]<ulong, ulong, int> AckOutcomes;
        public delegate* unmanaged[Cdecl]<uint, uint, uint*, int> ReevaluatePickup;
        public delegate* unmanaged[Cdecl]<ulong, int> Detach;
        public delegate* unmanaged[Cdecl]<ulong, ulong, void> WindowBegin;
        public delegate* unmanaged[Cdecl]<ulong, ulong, void> WindowEnd;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct WindowState
    {
        public uint Size, Phase;
        public ulong WorldEpoch, Step;
        public fixed byte ProcessSession[16];
        public uint WorldThread, Reserved;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct ClockTable
    {
        public ServiceHeader Header;
        public delegate* unmanaged[Cdecl]<WindowState*, int> Get;
    }

    /// <summary>The mirrors have the sizes the header asserts.</summary>
    internal static bool LayoutMatchesNative =>
        sizeof(EntityKey) == 40 && sizeof(Request) == 1168 && sizeof(Interest) == 40 && sizeof(Answer) == 32 &&
        sizeof(Receipt) == 208 && sizeof(Outcome) == 328 && sizeof(Table) == 136 && sizeof(WindowState) == 48 &&
        sizeof(ClockTable) == 16;

    /// <summary>The payload size the service accepts for <paramref name="kind"/>; 0 for a kind it refuses.</summary>
    internal static int PayloadSize(uint kind) => kind switch
    {
        KindDeath => 24,
        KindWalkover => 40,
        KindEquipSingle => 56,
        KindEquipCollection => 56,
        KindEquipSet => 824,
        KindBuildPiece => 328,
        _ => 0,
    };

    internal static string Text(byte* utf8, int capacity)
    {
        var n = 0;
        while (n < capacity && utf8[n] != 0) n++;
        return n == 0 ? "" : Encoding.UTF8.GetString(utf8, n);
    }
}

/// <summary>One request of kiln.decisions as its kind's producer gets it: who and what, and the kind's payload as the
/// header declares it (a producer reads its own struct out of it with <see cref="Read{T}"/>).</summary>
internal sealed class DecisionRequest
{
    private readonly byte[] _payload;

    internal DecisionRequest(DecisionId id, uint kind, ulong captureStep, ulong deadlineStep, EventEntityKey subject, EventEntityKey actor,
        ulong exposureEpoch, uint flags, byte[] payload)
    {
        Id = id;
        Kind = kind;
        CaptureStep = captureStep;
        DeadlineStep = deadlineStep;
        Subject = subject;
        Actor = actor;
        ExposureEpoch = exposureEpoch;
        Flags = flags;
        _payload = payload;
    }

    public DecisionId Id { get; }
    public uint Kind { get; }

    /// <summary>The step whose simulation held the action (or whose Pre filed it).</summary>
    public ulong CaptureStep { get; }

    /// <summary>The last step whose Pre may answer.</summary>
    public ulong DeadlineStep { get; }

    public EventEntityKey Subject { get; }

    /// <summary>The acting entity's key; <c>Entity</c> 0 when the kind has none.</summary>
    public EventEntityKey Actor { get; }

    public ulong ExposureEpoch { get; }

    /// <summary><c>KILN_DECISION_FLAG_*</c>.</summary>
    public uint Flags { get; }

    public bool HasPosition => (Flags & DecisionsNative.FlagHasPosition) != 0;
    public bool DryRun => (Flags & DecisionsNative.FlagDryRun) != 0;
    public ReadOnlySpan<byte> Payload => _payload;

    /// <summary>The payload as the kind's struct.</summary>
    /// <exception cref="InvalidOperationException">The payload is smaller than <typeparamref name="T"/>.</exception>
    public T Read<T>() where T : unmanaged
    {
        if (_payload.Length < Unsafe.SizeOf<T>())
            throw new InvalidOperationException($"decision {Id}: kind {Kind} carries {_payload.Length} bytes, {typeof(T).Name} needs {Unsafe.SizeOf<T>()}");
        return MemoryMarshal.Read<T>(_payload);
    }
}

/// <summary>What the handlers of a decision's events said.</summary>
/// <param name="Allow">False: a handler canceled, the action must not happen.</param>
/// <param name="SurvivalHealth">For a canceled death: the health to keep the entity at (positive).</param>
internal readonly record struct DecisionVerdict(bool Allow, int SurvivalHealth = 0)
{
    public static DecisionVerdict Allowed => new(true);
    public static DecisionVerdict Canceled(int survivalHealth = 0) => new(false, survivalHealth);
}

/// <summary>What the service said to an answer.</summary>
/// <param name="Result">The table's KilnResult (0: taken).</param>
/// <param name="Accepted">This answer decided the request; false: it was terminal already (or unknown).</param>
/// <param name="Status">The request's terminal status; null when the service did not know the request.</param>
/// <param name="Reason">The service's words; null when it had none.</param>
internal readonly record struct DecisionReceipt(int Result, bool Accepted, DecisionTerminal? Status, string? Reason);

/// <summary>The part of a service that turns one kind of request into its events (Kiln.Combat: kind 1 into
/// <c>EntityDyingEvent</c>). Registered once with <see cref="DecisionBroker.Register"/>; every member is called on the
/// tick thread in Pre, as Kiln (no current mod).</summary>
internal abstract class DecisionProducer
{
    /// <summary><c>KILN_DECISION_*</c>, 1 to 63.</summary>
    public abstract uint Kind { get; }

    /// <summary>The capability row of the kind (<c>events.death</c>): while it is off, a subscription to one of
    /// <see cref="EventTypes"/> is refused by name and the broker tells the service that nobody is interested.</summary>
    public abstract string Capability { get; }

    /// <summary>The event classes this producer posts for a request. Their handlers are the kind's interest.</summary>
    public abstract IReadOnlyList<Type> EventTypes { get; }

    /// <summary>Posts the kind's events for <paramref name="request"/> and returns what their handlers decided.</summary>
    public abstract DecisionVerdict Decide(DecisionRequest request);

    /// <summary>Right before the answer goes to the service (a death's producer reads the health here).</summary>
    public virtual void Answering(DecisionRequest request, DecisionVerdict verdict) { }

    /// <summary>Right after the service took (or refused) the answer.</summary>
    public virtual void Answered(DecisionRequest request, DecisionVerdict verdict, DecisionReceipt receipt) { }

    /// <summary>The decision ended (also when it was never answered: a timeout, the world's end), before
    /// <see cref="DecisionCompletedEvent"/> is posted.</summary>
    public virtual void Completed(DecisionResult result) { }

    /// <summary>What the service should hold for this kind: nothing when no handler listens for <see cref="EventTypes"/>,
    /// everything as soon as one of them has no entity filter (a handler of a base class counts), else the entities the
    /// filters name.</summary>
    public virtual DecisionInterest Interest()
    {
        HashSet<uint>? ids = null;
        foreach (var type in EventTypes)
        {
            if (Events.EntityFilterUnion(type) is not { } union) return DecisionInterest.Everything;
            foreach (var e in union) (ids ??= []).Add(e.Value);
        }
        if (ids is null) return DecisionInterest.Nothing;
        if (ids.Count > DecisionsNative.MaxInterest) return DecisionInterest.Everything;  // more than one call carries: hold all, the filters still apply
        var sorted = ids.ToArray();
        Array.Sort(sorted);
        return new DecisionInterest(false, sorted);
    }
}

/// <summary>A kind's interest: every entity, or these (ascending); neither is "nothing".</summary>
internal readonly record struct DecisionInterest(bool All, uint[] Entities)
{
    public static DecisionInterest Nothing => new(false, []);
    public static DecisionInterest Everything => new(true, []);
    public bool IsNothing => !All && Entities.Length == 0;

    public bool Same(DecisionInterest other) => All == other.All && Entities.AsSpan().SequenceEqual(other.Entities);
}

/// <summary>Where the server world's step is (kiln.window_clock).</summary>
internal readonly record struct WindowState(uint Phase, ulong WorldEpoch, ulong Step, Guid ProcessSession)
{
    /// <summary>Pre, Preparing, AfterJobs or Post: the phases in which the world may be read on the world thread.</summary>
    public bool WorldSafePhase => Phase is DecisionsNative.PhasePre or DecisionsNative.PhasePreparing or DecisionsNative.PhaseAfterJobs or DecisionsNative.PhasePost;
}

/// <summary>The one consumer of kiln.decisions in the managed host. See the head of this file.</summary>
internal static unsafe class DecisionBroker
{
    /// <summary>How many batches one Pre takes at most (what handlers file while a batch is dispatched is the next batch).</summary>
    private const int MaxRounds = 8;

    /// <summary>How many ended decisions are remembered, so that one whose request arrives late is not dispatched.</summary>
    private const int TerminalMemory = 16384;

    private static readonly object Gate = new();
    private static DecisionProducer?[] _producers = new DecisionProducer?[64];  // copy on write under Gate
    private static IDisposable? _tick;
    private static int _tickGeneration = -1;
    private static bool _watching;

    // table lookup: any thread
    private static IKilnBackend? _backend;
    private static volatile nint _table, _clock;
    private static ulong _retryAt;

    // everything below: tick thread
    private static ulong _owner;
    private static bool _attached, _attachRefusedLogged, _pumping;
    private static Guid _session;
    private static ulong _requestBatch, _step;
    private static readonly HashSet<DecisionId> Dispatched = [];
    private static readonly HashSet<DecisionId> Terminal = [];
    private static readonly Queue<DecisionId> TerminalOrder = new();
    private static readonly Dictionary<DecisionId, bool?> Lab = [];
    private static readonly HashSet<DecisionId> LabOpen = [];  // filed by the lab and not ended yet: they keep the pump running
    private static readonly InterestState?[] Pushed = new InterestState?[64];
    private static byte[] _buffer = [];
    private static int _readFaults, _producerFaults;

    private sealed class InterestState
    {
        public bool On;
        public int Version;
        public DecisionInterest Sent;
    }

    /// <summary>Counters for tests and a diagnostics line.</summary>
    internal static long RequestsDispatched, AnswersSent, OutcomesPosted, BatchesRead;

    /// <summary>Makes <paramref name="producer"/> the producer of its kind and starts the broker's tick. Its event types
    /// are registered with <see cref="EventProducers"/>: a subscription to one of them needs the producer's capability
    /// and a service that produces the kind. Any thread.</summary>
    /// <exception cref="ArgumentException">A kind outside 1..63 or without a payload the service knows, or a second
    /// producer of a kind.</exception>
    public static void Register(DecisionProducer producer)
    {
        ArgumentNullException.ThrowIfNull(producer);
        var kind = producer.Kind;
        if (kind is 0 or >= 64 || DecisionsNative.PayloadSize(kind) == 0)
            throw new ArgumentException($"kiln.decisions has no kind {kind}", nameof(producer));
        ArgumentException.ThrowIfNullOrEmpty(producer.Capability);
        lock (Gate)
        {
            if (_producers[kind] is { } have && !ReferenceEquals(have, producer))
                throw new ArgumentException($"decision kind {kind} already has a producer ({have.GetType().Name})", nameof(producer));
            var next = (DecisionProducer?[])_producers.Clone();
            next[kind] = producer;
            Volatile.Write(ref _producers, next);
        }
        // the row can read on where the service is not (an older runtime, a service that took another consumer): ask it
        foreach (var type in producer.EventTypes)
            EventProducers.Register(type, producer.Capability, () => (SupportedKinds >> (int)kind & 1) != 0 ? null
                : Available ? $"kiln.decisions has no producer of kind {kind} in this build" : "kiln.decisions is not here");
        Start();
    }

    /// <summary>Test hook: takes a producer out again (its event types stop needing a capability).</summary>
    internal static void Unregister(DecisionProducer producer)
    {
        lock (Gate)
        {
            if (!ReferenceEquals(_producers[producer.Kind], producer)) return;
            var next = (DecisionProducer?[])_producers.Clone();
            next[producer.Kind] = null;
            Volatile.Write(ref _producers, next);
        }
        foreach (var type in producer.EventTypes) EventProducers.Forget(type);
    }

    /// <summary>The producer of <paramref name="kind"/>, if one registered.</summary>
    internal static DecisionProducer? ProducerOf(uint kind) => kind < 64 ? Volatile.Read(ref _producers)[kind] : null;

    /// <summary>Adds the broker's handler to Kiln's service tick (once; again after a test reset the tick).</summary>
    internal static void Start()
    {
        lock (Gate)
        {
            if (_tick is null || _tickGeneration != ServiceTick.Generation)
            {
                _tickGeneration = ServiceTick.Generation;
                _tick = ServiceTick.Add("kiln decisions", static (in TickInfo t) => Pump(t.Step));
            }
            if (_watching) return;
            _watching = true;
            Events.SubscriptionAdded += OnSubscriptionAdded;
        }
    }

    // A handler was added: when that happens on the tick thread in a phase the service takes calls in (a mod's OnLoad or
    // Pre), the kind's interest follows at once, so the step that is about to run already holds for it. Otherwise the
    // next Pre's pump does it.
    private static void OnSubscriptionAdded()
    {
        if (_pumping || !Backend.Current.IsTickThread) return;
        try
        {
            if (Bind(_step)) SyncInterest();
        }
        catch (Exception e)
        {
            Backend.Current.Log(0, LogLevel.Warn, $"kiln decisions: the new subscription's interest waits for the next step: {e.Message}");
        }
    }

    // ------------------------------------------------------------------------------------------------ the tables ----

    private static DecisionsNative.Table* Table(ulong step = ulong.MaxValue)
    {
        var backend = Backend.Current;
        if (!ReferenceEquals(backend, Volatile.Read(ref _backend)))
        {
            lock (Gate)
            {
                _backend = backend;
                _table = 0;
                _clock = 0;
                _retryAt = 0;
            }
        }
        var t = (DecisionsNative.Table*)_table;
        if (t != null) return t;
        // a service that is not there is asked for again now and then (a table published late), not every step
        if (step != ulong.MaxValue && step < Volatile.Read(ref _retryAt)) return null;
        lock (Gate)
        {
            if (_table != 0) return (DecisionsNative.Table*)_table;
            var clock = Services.Get<DecisionsNative.ClockTable>(DecisionsNative.ClockService, DecisionsNative.Version);
            var table = clock == null ? null : Services.Get<DecisionsNative.Table>(DecisionsNative.Service, DecisionsNative.Version);
            if (table == null)
            {
                if (step != ulong.MaxValue) _retryAt = step + 256;
                return null;
            }
            _clock = (nint)clock;
            _table = (nint)table;
            return table;
        }
    }

    /// <summary>The step clock's state now; null when kiln.window_clock (or kiln.decisions) is not here. Any thread.</summary>
    internal static WindowState? Window()
    {
        if (Table() == null) return null;
        var clock = (DecisionsNative.ClockTable*)_clock;
        DecisionsNative.WindowState s = default;
        s.Size = (uint)sizeof(DecisionsNative.WindowState);
        if (clock == null || clock->Get(&s) != DecisionsNative.Ok) return null;
        return new WindowState(s.Phase, s.WorldEpoch, s.Step, new Guid(new ReadOnlySpan<byte>(s.ProcessSession, 16)));
    }

    /// <summary>Whether the service's table is here (no call into it).</summary>
    internal static bool Available => Table() != null;

    /// <summary>The kinds the service has a producer for (bit n: kind n); 0 without the service. Any thread.</summary>
    internal static ulong SupportedKinds
    {
        get
        {
            var t = Table();
            return t == null ? 0 : t->SupportedKinds();
        }
    }

    /// <summary>The identity of <paramref name="entity"/> as the service's metadata has it, read now (no scan). World
    /// thread in a world-safe phase only: the service answers <see cref="DecisionsNative.WrongThread"/> anywhere else,
    /// also on the world thread inside the simulation. Returns the KilnResult: 0, or
    /// <see cref="DecisionsNative.NotFound"/> (no such entity, or none with a usable identity),
    /// <see cref="DecisionsNative.Unavailable"/>, <see cref="DecisionsNative.WrongThread"/>.</summary>
    internal static int ResolveEntity(EntityId entity, out EventEntityKey key, out WorldPosition? position)
    {
        key = default;
        position = null;
        var t = Table();
        if (t == null) return DecisionsNative.Unavailable;
        if (entity.IsNone) return DecisionsNative.InvalidArgument;
        DecisionsNative.EntityKey k = default;
        var at = stackalloc double[3];
        uint has = 0;
        var rc = t->ResolveEntity(entity.Value, &k, at, &has);
        if (rc != DecisionsNative.Ok) return rc;
        key = k.ToPublic();
        if (has != 0) position = new WorldPosition(at[0], at[1], at[2]);
        return rc;
    }

    /// <summary>kiln.decisions' reevaluate_pickup (world thread, world-safe phase). Returns the KilnResult.</summary>
    internal static int ReevaluatePickup(EntityId drop, EntityId receiver, out int reset)
    {
        reset = 0;
        var t = Table();
        if (t == null) return DecisionsNative.Unavailable;
        uint n = 0;
        var rc = t->ReevaluatePickup(drop.Value, receiver.Value, &n);
        reset = (int)n;
        return rc;
    }

    // Tick thread: the table, this session's attach. False: nothing can be asked now.
    private static bool Bind(ulong step)
    {
        var t = Table(step);
        if (t == null) return false;
        if (Window() is not { } now) return false;
        if (now.ProcessSession != _session)
        {
            // another process of the game as far as the service is concerned (a test's next harness): nothing of the
            // last one is open any more
            _session = now.ProcessSession;
            _attached = false;
            _attachRefusedLogged = false;
            _requestBatch = 0;
            Dispatched.Clear();
            Terminal.Clear();
            TerminalOrder.Clear();
            Lab.Clear();
        LabOpen.Clear();
            Array.Clear(Pushed);
        }
        if (_attached) return true;
        if (!now.WorldSafePhase) return false;  // no world yet: attach waits for the first Pre
        if (_owner == 0)
        {
            _owner = Backend.Current.OwnerCreate("kiln.decisions.managed");
            if (_owner == 0) return false;
        }
        var rc = t->Attach(_owner);
        if (rc != DecisionsNative.Ok)
        {
            if (rc != DecisionsNative.WrongThread && !_attachRefusedLogged)
            {
                _attachRefusedLogged = true;
                Backend.Current.Log(0, LogLevel.Warn, rc == DecisionsNative.Unavailable
                    ? "kiln decisions: kiln.decisions is off or has another consumer; cancelable decisions are not dispatched to mods"
                    : $"kiln decisions: kiln.decisions refused the managed dispatcher ({rc})");
            }
            return false;
        }
        _attached = true;
        Array.Clear(Pushed);
        return true;
    }

    // ---------------------------------------------------------------------------------------------------- the pump ----

    /// <summary>One Pre: interests, outcomes, requests, and the outcomes of what was just answered. Tick thread. A call
    /// from inside a dispatch (a handler that reaches an adapter) does nothing: the outer pump takes the next batch.</summary>
    internal static void Pump(ulong step)
    {
        if (_pumping) return;
        _step = step;
        // nobody produces a decision's events here: the service keeps its consumer's place free and nothing is asked
        if (LabOpen.Count == 0 && !Array.Exists(Volatile.Read(ref _producers), p => p is not null)) return;
        _pumping = true;
        try
        {
            if (!Bind(step)) return;
            SyncInterest();
            PumpOutcomes();
            if (PumpRequests(step)) PumpOutcomes();
        }
        finally
        {
            _pumping = false;
        }
    }

    private static void SyncInterest()
    {
        var t = (DecisionsNative.Table*)_table;
        var producers = Volatile.Read(ref _producers);
        var supported = t->SupportedKinds();
        var version = Events.Version;
        for (uint kind = 1; kind < 64; kind++)
        {
            if (producers[kind] is not { } producer)
            {
                if (Pushed[kind] is { Sent.IsNothing: false } && Push(t, kind, DecisionInterest.Nothing)) Pushed[kind] = null;
                continue;
            }
            var on = (supported >> (int)kind & 1) != 0 && Capabilities.Is(producer.Capability);
            var state = Pushed[kind];
            if (state is not null && state.On == on && state.Version == version) continue;
            var want = on ? producer.Interest() : DecisionInterest.Nothing;
            if (state is null ? !want.IsNothing : !want.Same(state.Sent))
            {
                if (!Push(t, kind, want)) continue;  // tried again at the next pump
            }
            Pushed[kind] = new InterestState { On = on, Version = version, Sent = want };
        }
    }

    private static bool Push(DecisionsNative.Table* t, uint kind, DecisionInterest want)
    {
        fixed (uint* ids = want.Entities)
        {
            var interest = new DecisionsNative.Interest
            {
                Size = (uint)sizeof(DecisionsNative.Interest),
                Kind = kind,
                AllEntities = want.All ? 1u : 0u,
                RegistryRevision = (ulong)Events.Version,
                Entities = want.Entities.Length == 0 ? null : ids,
                EntityCount = (uint)want.Entities.Length,
            };
            return t->Interest(_owner, &interest) == DecisionsNative.Ok;
        }
    }

    // One pinned array for both kinds of batch, grown to the largest batch seen (4096 requests are 4.7 MB).
    private static byte* Buffer(long bytes)
    {
        if (_buffer.Length < bytes) _buffer = GC.AllocateUninitializedArray<byte>(checked((int)Math.Max(bytes, 4096)), pinned: true);
        return (byte*)Unsafe.AsPointer(ref MemoryMarshal.GetArrayDataReference(_buffer));  // pinned: the address is stable
    }

    // True when something was answered.
    private static bool PumpRequests(ulong step)
    {
        var t = (DecisionsNative.Table*)_table;
        var answered = false;
        for (var round = 0; round < MaxRounds; round++)
        {
            ulong batch = 0;
            uint count = 0;
            if (t->BeginRequests(_owner, &batch, &count) != DecisionsNative.Ok || batch == 0 || count == 0) return answered;
            if (batch != _requestBatch)
            {
                _requestBatch = batch;
                Dispatched.Clear();
            }
            var records = (DecisionsNative.Request*)Buffer((long)count * sizeof(DecisionsNative.Request));
            uint required = 0;
            var rc = t->ReadRequests(_owner, batch, records, count, (uint)sizeof(DecisionsNative.Request), &required);
            if (rc != DecisionsNative.Ok)
            {
                ReadFault("requests", batch, rc, required);
                return answered;  // nothing was consumed: the same batch at the next Pre
            }
            Interlocked.Increment(ref BatchesRead);
            for (uint i = 0; i < count; i++)
            {
                var copy = records[i];  // a handler may reach code that reads another batch into the buffer
                if (Dispatch(t, ref copy, step)) answered = true;
            }
            if (t->AckRequests(_owner, batch) != DecisionsNative.Ok) return answered;  // it comes again; Dispatched keeps handlers from running twice
            _requestBatch = 0;
            Dispatched.Clear();
        }
        return answered;
    }

    private static bool Dispatch(DecisionsNative.Table* t, ref DecisionsNative.Request r, ulong step)
    {
        var id = new DecisionId(r.WorldEpoch, r.RequestId);
        if (!Dispatched.Add(id)) return false;   // read a second time: its handlers ran
        if (Terminal.Contains(id))               // ended before it was delivered (released in time, the world ended)
        {
            Lab.Remove(id);
            return false;
        }
        DecisionRequest? request = null;
        DecisionProducer? producer = null;
        var verdict = DecisionVerdict.Allowed;
        if (Lab.Remove(id, out var lab))
        {
            if (lab is not { } allow) return false;  // the lab asked for no answer: the service's deadline releases it
            verdict = new DecisionVerdict(allow);
        }
        else if (r.Size >= (uint)sizeof(DecisionsNative.Request) && r.Version == 1 && r.PayloadBytes <= DecisionsNative.PayloadCapacity &&
                 (producer = ProducerOf(r.Kind)) is not null)
        {
            byte[] payload;
            fixed (byte* bytes = r.Payload) payload = new ReadOnlySpan<byte>(bytes, (int)r.PayloadBytes).ToArray();
            request = new DecisionRequest(id, r.Kind, r.CaptureStep, r.DeadlineStep, r.Subject.ToPublic(), r.Actor.ToPublic(), r.ExposureEpoch, r.Flags, payload);
            Interlocked.Increment(ref RequestsDispatched);
            try
            {
                verdict = producer.Decide(request);
            }
            catch (Exception e)
            {
                verdict = DecisionVerdict.Allowed;
                ProducerFault(producer, id, "decide", e);
            }
            try
            {
                producer.Answering(request, verdict);
            }
            catch (Exception e)
            {
                ProducerFault(producer, id, "before the answer", e);
            }
        }
        // else: a kind nobody produces here, or a record of a version this Kiln.Api does not read: let the game go on

        var answer = new DecisionsNative.Answer
        {
            Size = (uint)sizeof(DecisionsNative.Answer),
            Kind = r.Kind,
            RequestId = r.RequestId,
            WorldEpoch = r.WorldEpoch,
            Allow = verdict.Allow ? 1u : 0u,
            SurvivalHealth = verdict.Allow ? 0 : verdict.SurvivalHealth,
        };
        DecisionsNative.Receipt receipt = default;
        receipt.Size = (uint)sizeof(DecisionsNative.Receipt);
        var rc = t->Answer(_owner, &answer, &receipt);
        Interlocked.Increment(ref AnswersSent);
        if (producer is not null && request is not null)
        {
            var text = DecisionsNative.Text(receipt.Reason, 192);
            var said = new DecisionReceipt(rc, rc == DecisionsNative.Ok && receipt.Accepted != 0,
                rc == DecisionsNative.Ok && receipt.TerminalStatus is >= 1 and <= 11 ? (DecisionTerminal)receipt.TerminalStatus : null,
                text.Length == 0 ? null : text);
            try
            {
                producer.Answered(request, verdict, said);
            }
            catch (Exception e)
            {
                ProducerFault(producer, id, "after the answer", e);
            }
        }
        return true;
    }

    private static void PumpOutcomes()
    {
        var t = (DecisionsNative.Table*)_table;
        for (var round = 0; round < MaxRounds; round++)
        {
            ulong batch = 0;
            uint count = 0;
            if (t->BeginOutcomes(_owner, &batch, &count) != DecisionsNative.Ok || batch == 0 || count == 0) return;
            var records = (DecisionsNative.Outcome*)Buffer((long)count * sizeof(DecisionsNative.Outcome));
            uint required = 0;
            var rc = t->ReadOutcomes(_owner, batch, records, count, (uint)sizeof(DecisionsNative.Outcome), &required);
            if (rc != DecisionsNative.Ok)
            {
                ReadFault("outcomes", batch, rc, required);
                return;
            }
            Interlocked.Increment(ref BatchesRead);
            for (uint i = 0; i < count; i++)
            {
                var copy = records[i];
                Complete(ref copy);
            }
            if (t->AckOutcomes(_owner, batch) != DecisionsNative.Ok) return;  // it comes again; Terminal keeps an outcome from being posted twice
        }
    }

    private static void Complete(ref DecisionsNative.Outcome o)
    {
        var id = new DecisionId(o.WorldEpoch, o.RequestId);
        if (!Terminal.Add(id)) return;  // posted already (the batch was read again)
        TerminalOrder.Enqueue(id);
        while (TerminalOrder.Count > TerminalMemory) Terminal.Remove(TerminalOrder.Dequeue());
        Lab.Remove(id);
        LabOpen.Remove(id);
        if (o.Status is < 1 or > 11) return;  // a status of a later service: nothing this Kiln.Api can name
        var result = ResultOf(id, ref o);
        if (ProducerOf(o.Kind) is { } producer)
        {
            try
            {
                producer.Completed(result);
            }
            catch (Exception e)
            {
                ProducerFault(producer, id, "completed", e);
            }
        }
        Interlocked.Increment(ref OutcomesPosted);
        if (Events.HasSubscribers<DecisionCompletedEvent>()) Events.Post(new DecisionCompletedEvent(result));
    }

    private static DecisionResult ResultOf(DecisionId id, ref DecisionsNative.Outcome o)
    {
        var status = (DecisionTerminal)o.Status;
        var ok = status is DecisionTerminal.Allowed or DecisionTerminal.Prevented or DecisionTerminal.AlreadyAlive;
        string? reason = null;
        if (!ok)
        {
            fixed (byte* text = o.Reason) reason = DecisionsNative.Text(text, 192);
            if (reason.Length == 0) reason = DefaultReason(status);
        }
        var death = o.Kind == DecisionsNative.KindDeath;
        var known = death && (o.Flags & DecisionsNative.OutcomeHealthKnown) != 0;
        return new DecisionResult(id, status, new EntityId(o.Subject.Entity), new EntityId(o.Actor.Entity),
            death && o.RequestedHealth > 0 ? o.RequestedHealth : null,
            known ? o.HealthBefore : null, known ? o.AppliedHealth : null, reason);
    }

    private static string DefaultReason(DecisionTerminal status) => status switch
    {
        DecisionTerminal.StaleEntity => "the entity was gone before the answer",
        DecisionTerminal.NoPositiveRange => "the entity's maximum health is not positive",
        DecisionTerminal.WriteRefused => "the game refused the write",
        DecisionTerminal.TimeoutReleased => "no answer in time; the action was released",
        DecisionTerminal.CapabilityReleased => "the capability or the listener went away; the action was released",
        DecisionTerminal.SourceChanged => "what was held changed under the hold",
        DecisionTerminal.QueueDropped => "the decision queue was full; the action was not held",
        DecisionTerminal.WorldEnded => "the world stopped",
        _ => status.ToString(),
    };

    private static void ReadFault(string what, ulong batch, int rc, uint required)
    {
        var n = ++_readFaults;
        if (n <= 5 || n % 1000 == 0)
            Backend.Current.Log(0, LogLevel.Warn, $"kiln decisions: batch {batch} of {what} could not be read ({rc}, {required} records); it is read again at the next step ({n}x)");
    }

    private static void ProducerFault(DecisionProducer producer, DecisionId id, string where, Exception e)
    {
        var n = ++_producerFaults;
        if (n <= 5 || n % 1000 == 0)
            Backend.Current.Log(0, LogLevel.Error, $"kiln decisions: {producer.GetType().Name} threw ({where}, decision {id.WorldEpoch}:{id.Request}); the action goes on ({n}x): {e}");
    }

    // ------------------------------------------------------------------------------------------- Pre adapters ----

    /// <summary>Files a request of a kind that has no hook (16, 17: a client's equipment input; 18: a queued equipment
    /// set) from a Pre adapter. Tick thread, Pre. The service copies it and gives it its id, the world's epoch and its
    /// deadline: this Pre for 16 and 17 (call <see cref="Pump"/> right after: the answer is due in the same Pre), two
    /// more Pres for 18.</summary>
    /// <returns>The decision's id; null when the service took nothing. <paramref name="result"/> is the KilnResult:
    /// with <see cref="DecisionsNative.BufferTooSmall"/> the queue is full and the action must go on unheld (an outcome
    /// QueueDropped follows under the returned id).</returns>
    /// <exception cref="ArgumentException">A payload that is not the kind's size.</exception>
    internal static DecisionId? Enqueue(uint kind, EventEntityKey subject, EventEntityKey actor, ReadOnlySpan<byte> payload, uint flags, out int result)
    {
        if (payload.Length != DecisionsNative.PayloadSize(kind) || payload.Length == 0)
            throw new ArgumentException($"decision kind {kind} takes {DecisionsNative.PayloadSize(kind)} payload bytes, not {payload.Length}", nameof(payload));
        Services.RequireTickThread("DecisionBroker.Enqueue");
        result = DecisionsNative.Unavailable;
        if (!Bind(_step) || Window() is not { } now) return null;
        Start();
        var t = (DecisionsNative.Table*)_table;
        DecisionsNative.Request r = default;
        r.Size = (uint)sizeof(DecisionsNative.Request);
        r.Version = 1;
        r.Kind = kind;
        r.PayloadBytes = (uint)payload.Length;
        r.Subject = DecisionsNative.EntityKey.From(subject);
        r.Actor = DecisionsNative.EntityKey.From(actor);
        r.Flags = flags;
        payload.CopyTo(new Span<byte>(r.Payload, DecisionsNative.PayloadCapacity));
        ulong request = 0;
        result = t->EnqueuePrepared(_owner, &r, &request);
        if (request == 0 || result is not (DecisionsNative.Ok or DecisionsNative.BufferTooSmall)) return null;
        return new DecisionId(now.WorldEpoch, request);
    }

    // ------------------------------------------------------------------------------------------------------ lab ----

    /// <summary>Lab: files a decision of kind 18 with an empty set about <paramref name="subject"/> and answers it as
    /// told when its request arrives (null: never, so the service's deadline releases it). No event of a kind is
    /// posted for it; its <see cref="DecisionCompletedEvent"/> is. Tick thread, Pre.</summary>
    internal static DecisionId? LabDecide(EntityId subject, bool? allow, out int result)
    {
        Span<byte> payload = stackalloc byte[DecisionsNative.PayloadSize(DecisionsNative.KindEquipSet)];
        payload.Clear();
        Start();
        var id = Enqueue(DecisionsNative.KindEquipSet, new EventEntityKey(subject.Value, false, 0, 0, 0, 0), default, payload, 0, out result);
        if (id is { } made && result == DecisionsNative.Ok)
        {
            Lab[made] = allow;
            LabOpen.Add(made);
        }
        return id;
    }

    /// <summary>Test hook: forgets the tables and this session, as a process that starts.</summary>
    internal static void ResetForTests()
    {
        lock (Gate)
        {
            _backend = null;
            _table = 0;
            _clock = 0;
            _retryAt = 0;
            _producers = new DecisionProducer?[64];
        }
        _session = default;
        _attached = false;
        _owner = 0;
        _requestBatch = 0;
        Dispatched.Clear();
        Terminal.Clear();
        TerminalOrder.Clear();
        Lab.Clear();
        LabOpen.Clear();
        Array.Clear(Pushed);
    }
}

/// <summary>The seam between Kiln.Players' intake of held client actions and the typed equipment decisions of
/// Kiln.Items (chapter 07 §4.4; the two libraries do not reference each other). Kiln.Players' Pre pump is the one reader
/// of kiln.playerstate's held actions: for an action the typed side decides it files one decision (kind 16 or 17) and
/// posts the generic <c>PlayerClientActionEvent</c> first, then pumps the broker, whose dispatch hands the request to
/// Kiln.Items' producer, which posts <c>PlayerEquippingEvent</c> with the generic vote as its prior and records the
/// typed vote here; Kiln.Players then answers the game once: canceled when the generic OR the typed handlers canceled.
/// Everything here runs on the tick thread in Pre.</summary>
internal static class ClientActionDecisions
{
    /// <summary>One held client action while its decision is open.</summary>
    internal sealed class Held
    {
        public uint Entity, Index, Version;
        public string Name = "";
        public byte[] Payload = [];
        public EventEntityKey Player;
        public bool GenericCanceled;
        public string? GenericCanceledBy;
        /// <summary>Set by the typed producer when it posted its event: what the typed handlers left.</summary>
        public bool? TypedCanceled;
        /// <summary>The typed side's description of the action, made in <see cref="ITyped.Payload"/> (its event's
        /// change record).</summary>
        public object? Context;
    }

    /// <summary>What Kiln.Items gives the intake (registered by its typed producer).</summary>
    internal interface ITyped
    {
        /// <summary>True while a typed handler listens and its capability is on.</summary>
        bool On { get; }

        /// <summary>The decision kind for a client action of this name (16, 17), or 0 when the typed side does not decide it.</summary>
        uint KindOf(string actionName);

        /// <summary>The players to hold the typed actions for: every player, or these ids (a filter's entities that are
        /// not players are covered by "every player": the typed event's own filter then picks).</summary>
        DecisionInterest Interest();

        /// <summary>The kind's request payload for <paramref name="held"/> (read in this Pre), or null when the action is
        /// not one the typed side can describe (then only the generic vote counts).</summary>
        byte[]? Payload(Held held);
    }

    /// <summary>What Kiln.Players gives the typed side (registered at its start).</summary>
    internal interface IIntake
    {
        /// <summary>Starts the intake's Pre pump (a typed handler alone must hold the actions too).</summary>
        void Start();

        /// <summary>Null when kiln.playerstate can hold client actions selectively here (client_action_interest),
        /// else why not.</summary>
        string? WhyNotSelective();
    }

    private static readonly Dictionary<DecisionId, Held> Open = [];
    private static volatile ITyped? _typed;
    private static volatile IIntake? _intake;

    public static ITyped? Typed => _typed;
    public static IIntake? Intake => _intake;

    /// <summary>The typed side registers (its first subscription); the intake's pump starts if Kiln.Players is up.</summary>
    public static void SetTyped(ITyped? typed)
    {
        _typed = typed;
        if (typed is not null) _intake?.Start();
    }

    /// <summary>Kiln.Players registers at its start (the first service tick, which can come after a mod's OnLoad
    /// subscribed): the pump starts if the typed side is already there.</summary>
    public static void SetIntake(IIntake? intake)
    {
        _intake = intake;
        if (intake is not null && _typed is not null) intake.Start();
    }

    /// <summary>The intake opened a decision for <paramref name="held"/>.</summary>
    public static void Opened(DecisionId id, Held held) => Open[id] = held;

    /// <summary>The intake, for one held action: files its typed decision (kind 16 or 17) when the typed side decides
    /// actions of this name, and returns the held record to fill with the generic vote and pass to
    /// <see cref="Finish"/>; null when the typed side does not decide it or the service took nothing (then only the
    /// generic vote counts, as without the typed side). Tick thread, Pre.</summary>
    public static Held? Begin(ITyped? typed, uint entity, uint index, uint version, string name, byte[] payload, out DecisionId id)
    {
        id = default;
        var kind = typed?.KindOf(name) ?? 0;
        if (typed is null || kind == 0) return null;
        var held = new Held { Entity = entity, Index = index, Version = version, Name = name, Payload = payload };
        held.Player = DecisionBroker.ResolveEntity(new EntityId(entity), out var key, out _) == DecisionsNative.Ok
            ? key
            : new EventEntityKey(entity, false, 0, 0, 0, 0);
        if (typed.Payload(held) is not { } request) return null;
        if (DecisionBroker.Enqueue(kind, held.Player, held.Player, request, 0, out var rc) is not { } made || rc != DecisionsNative.Ok) return null;
        id = made;
        return held;
    }

    /// <summary>The intake, after the generic votes of a batch of held actions: opens their decisions, lets the broker
    /// dispatch them in this Pre (kinds 16 and 17 are due in the Pre they are filed in: the typed events are posted
    /// now), closes them and returns each action's final verdict, true for cancel: the generic OR the typed vote. Every
    /// action of <paramref name="begun"/> is in the result once, also when the dispatch threw. Tick thread, Pre.</summary>
    public static List<(Held Held, bool Cancel)> Finish(ulong step, IReadOnlyList<(DecisionId Id, Held Held)> begun)
    {
        foreach (var (id, held) in begun) Open[id] = held;
        var verdicts = new List<(Held, bool)>(begun.Count);
        try
        {
            DecisionBroker.Pump(step);
        }
        finally
        {
            foreach (var (id, held) in begun)
            {
                Open.Remove(id);
                verdicts.Add((held, held.GenericCanceled || held.TypedCanceled == true));
            }
        }
        return verdicts;
    }

    /// <summary>The typed producer's view of an open decision; null for one the intake did not open (a lab's, an old
    /// one).</summary>
    public static Held? Of(DecisionId id) => Open.GetValueOrDefault(id);

    /// <summary>The intake's end of a decision: it is answered to the game now.</summary>
    public static Held? Close(DecisionId id) => Open.Remove(id, out var held) ? held : null;

    /// <summary>How many decisions are open (tests).</summary>
    internal static int OpenCount => Open.Count;

    /// <summary>Test hook.</summary>
    internal static void ResetForTests()
    {
        Open.Clear();
        _typed = null;
        _intake = null;
    }
}
