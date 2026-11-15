// SPDX-License-Identifier: MIT
// The broker of the kit's fake kiln.decisions: native/services/decisions/queue.h written again in C#, rule for rule
// (requests, outcomes, frozen batches, deadlines, limits), on the harness's steps. One per harness.
//
// Steps. The native service's windows sit between the Pres: window_begin(N) after the Pre subscribers of step N,
// window_end(N) after the world's jobs of step N. A harness step is one Pre, so the fake runs window_begin(N-1) and
// window_end(N-1) at the start of step N, before Kiln's service tick. What a test captures between two steps is "held
// by a hook during the simulation of the step that just ran".
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Text;
using N = Kiln.DecisionsNative;

[assembly: InternalsVisibleTo("Kiln.Decisions.Tests")]

namespace Kiln.TestKit;

/// <summary>What the fake's finisher gets when an answer arrives for a request (the native producers' finishers: the
/// health write of a canceled death).</summary>
internal readonly record struct FakeDecisionAnswer(DecisionId Id, uint Kind, EntityId Subject, bool Allow, int SurvivalHealth);

/// <summary>What a scripted finisher makes of an answer.</summary>
internal readonly record struct FakeDecisionFinish(DecisionTerminal Status, string? Reason = null, int? HealthBefore = null, int? AppliedHealth = null);

/// <summary>The fake service's counters (queue.h <c>Counters</c>).</summary>
internal readonly record struct FakeDecisionCounters(ulong Filed, ulong QueueDropped, ulong Timeouts, ulong Outcomes, ulong OutcomeGap, ulong MetadataMisses);

internal sealed unsafe class DecisionsModel
{
    public const int MaxActive = 4096, MaxOutcomes = 8192;

    private sealed class Batch<T>
    {
        public bool Open;
        public ulong Id, LastAcked, LastAckedStep;
        public List<T> Items = [];
    }

    private readonly object _lock = new();

    // the clock
    private readonly byte[] _session = Guid.NewGuid().ToByteArray();
    private ulong _epoch = 1, _step;
    private uint _phase, _thread;

    // the broker
    private bool _attached;
    private ulong _owner, _nextId, _batchSeq;
    private readonly SortedDictionary<ulong, N.Request> _active = [];
    private readonly List<ulong> _arrivals = [];
    private readonly Batch<N.Request> _requests = new();
    private readonly Queue<N.Outcome> _outbox = new();
    private readonly Batch<N.Outcome> _outcomes = new();
    private readonly Dictionary<ulong, (ulong Epoch, uint Kind, uint Status)> _terminal = [];
    private readonly Queue<ulong> _terminalOrder = new();
    private readonly Dictionary<uint, (bool All, uint[] Ids)> _interest = [];
    private readonly HashSet<(uint Drop, uint Receiver)> _suppressed = [];  // walk-over exposures a cancel suppressed
    private readonly Dictionary<(ulong Epoch, uint Entity), ulong> _lives = [];
    private ulong _nextLife;
    private ulong _filed, _dropped, _timeouts, _outcomeCount, _gap, _misses;

    // the script and the record
    private readonly List<DecisionResult> _record = [];
    private readonly List<FakeDecisionAnswer> _answers = [];
    public Func<FakeDecisionAnswer, FakeDecisionFinish?>? Finisher;
    public int FailRequestReads, FailRequestAcks, FailOutcomeReads, FailOutcomeAcks;
    public ulong SupportedHookKinds;
    public bool Faulted, PreparePending;
    public int BrokerCallsAfterFault;
    /// <summary>prepare_kind's refusals by kind (the native install of that kind's hook failed with this reason): the
    /// kind's row is lowered (faulted) and its bit leaves <see cref="SupportedHookKinds"/>, as the native service does.</summary>
    public readonly Dictionary<uint, (string Row, string Reason)> PrepareRefusals = [];
    /// <summary>How many prepare_kind calls the fake answered, by kind (a subscription asks it).</summary>
    public readonly Dictionary<uint, int> Prepared = [];

    // ---- the harness's life ----
    public void Start(KilnHarness harness)
    {
        lock (_lock)
        {
            _phase = 0;
            _step = 0;
        }
    }

    public void BeforeStep(ulong step)
    {
        lock (_lock)
        {
            _thread = (uint)Environment.CurrentManagedThreadId;
            if (_phase != 0)
            {
                WindowBegin(_step);
                WindowEnd(_step);
            }
            _step = step;
            _phase = N.PhasePre;
        }
    }

    public void End()
    {
        lock (_lock)
        {
            _phase = N.PhaseStopping;
            ReleaseAll(11, "the world stopped", _step);
        }
    }

    /// <summary>The server world stops and the next one starts (a hosted world switch): what is open ends WorldEnded,
    /// the epoch goes up by one. The consumer stays attached, as in the native service.</summary>
    public void RestartWorld()
    {
        lock (_lock)
        {
            ReleaseAll(11, "the world stopped", _step);
            _arrivals.Clear();
            _requests.Open = false;
            _requests.Items.Clear();
            _suppressed.Clear();
            _epoch++;
        }
    }

    /// <summary>Runs <paramref name="work"/> while the clock says "inside the simulation" (phase 3), as a game program
    /// that runs on the world thread would see it.</summary>
    public void InSimulation(Action work)
    {
        uint before;
        lock (_lock)
        {
            before = _phase;
            _phase = N.PhaseSim;
        }
        try
        {
            work();
        }
        finally
        {
            lock (_lock) _phase = before;
        }
    }

    // ---- the clock table ----
    public void Clock(N.WindowState* s)
    {
        lock (_lock)
        {
            *s = default;
            s->Size = (uint)sizeof(N.WindowState);
            s->Phase = _phase;
            s->WorldEpoch = _epoch;
            s->Step = _step;
            for (var i = 0; i < 16; i++) s->ProcessSession[i] = _session[i];
            s->WorldThread = _thread;
        }
    }

    private bool WorldSafe => FakeTables.OnTickThread && _phase is N.PhasePre or N.PhasePreparing or N.PhaseAfterJobs or N.PhasePost;

    // ---- consumer ----
    public ulong SupportedKinds => (1ul << (int)N.KindEquipSingle) | (1ul << (int)N.KindEquipCollection) | (1ul << (int)N.KindEquipSet) | SupportedHookKinds;

    public int Attach(ulong owner)
    {
        lock (_lock)
        {
            if (!WorldSafe) return FakeTables.WrongThread;
            if (_attached && _owner != owner) return FakeTables.Unavailable;
            _attached = true;
            _owner = owner;
            return FakeTables.Ok;
        }
    }

    public int Detach(ulong owner)
    {
        lock (_lock)
        {
            if (!WorldSafe) return FakeTables.WrongThread;
            if (!_attached || _owner != owner) return FakeTables.Unavailable;
            ReleaseAll(8, "the decision consumer detached", _step);
            _attached = false;
            _owner = 0;
            _arrivals.Clear();
            _requests.Open = false;
            _requests.Items.Clear();
            _interest.Clear();
            return FakeTables.Ok;
        }
    }

    private int BrokerRefused() { BrokerCallsAfterFault++; return FakeTables.Unavailable; }
    private int Consumer(ulong owner) => Faulted ? BrokerRefused() : !WorldSafe ? FakeTables.WrongThread : _attached && _owner == owner ? FakeTables.Ok : FakeTables.Unavailable;

    public int Interest(ulong owner, N.Interest* i)
    {
        if (i == null || i->Size < (uint)sizeof(N.Interest) || i->Kind >= 64 || N.PayloadSize(i->Kind) == 0 ||
            i->EntityCount > N.MaxInterest || (i->EntityCount != 0 && i->Entities == null))
            return FakeTables.InvalidArgument;
        lock (_lock)
        {
            if (Consumer(owner) is var rc and not FakeTables.Ok) return rc;
            var ids = new ReadOnlySpan<uint>(i->Entities, (int)i->EntityCount).ToArray();
            Array.Sort(ids);
            _interest[i->Kind] = (i->AllEntities != 0, [.. ids.Distinct()]);
            return FakeTables.Ok;
        }
    }

    public (bool All, IReadOnlyList<uint> Ids) InterestOf(uint kind)
    {
        lock (_lock) return _interest.TryGetValue(kind, out var have) ? (have.All, have.Ids) : (false, []);
    }

    public int ResolveEntity(KilnHarness harness, uint entity, N.EntityKey* key, double* position, uint* hasPosition)
    {
        if (key == null || entity == 0) return FakeTables.InvalidArgument;
        lock (_lock)
        {
            if (!WorldSafe) return FakeTables.WrongThread;  // also the tick thread inside the simulation (phase 3)
            if (harness.EntitiesFake is not { } world || !world.Exists(entity))
            {
                _misses++;
                return FakeTables.NotFound;
            }
            *key = KeyOf(entity);
            var at = world.PositionOf(entity);
            if (hasPosition != null) *hasPosition = at is null ? 0u : 1u;
            if (position != null)
            {
                position[0] = at?.X ?? 0;
                position[1] = at?.Y ?? 0;
                position[2] = at?.Z ?? 0;
            }
            return FakeTables.Ok;
        }
    }

    // The fake's identity of an entity: a spawn time made from the id, and a life serial per (world, entity).
    private N.EntityKey KeyOf(uint entity)
    {
        if (!_lives.TryGetValue((_epoch, entity), out var life)) _lives[(_epoch, entity)] = life = ++_nextLife;
        return new N.EntityKey { Entity = entity, Flags = N.KeySpawnTime, SpawnTime = 1000ul + entity, LifeSerial = life };
    }

    // ---- producers ----
    public int EnqueuePrepared(ulong owner, N.Request* request, ulong* id)
    {
        if (request == null || id == null) return FakeTables.InvalidArgument;
        if (request->Kind is not (N.KindEquipSingle or N.KindEquipCollection or N.KindEquipSet)) return FakeTables.InvalidArgument;
        lock (_lock)
        {
            if (Consumer(owner) is var rc and not FakeTables.Ok) return rc;
            return File(ref *request, out *id);
        }
    }

    /// <summary>A hook's capture (kinds 1, 2, 32), from a test: files the request as the hook would during the
    /// simulation of the step that ran last. Any thread. Returns the KilnResult; <paramref name="id"/> is set when
    /// the request was filed or dropped.</summary>
    public int Capture(uint kind, uint subject, uint actor, ReadOnlySpan<byte> payload, uint flags, ulong exposureEpoch, out DecisionId id)
    {
        lock (_lock)
        {
            N.Request r = default;
            r.Size = (uint)sizeof(N.Request);
            r.Version = 1;
            r.Kind = kind;
            r.PayloadBytes = (uint)payload.Length;
            r.Subject = KeyOf(subject);
            if (actor != 0) r.Actor = KeyOf(actor);
            r.Flags = flags;
            r.ExposureEpoch = exposureEpoch;
            payload.CopyTo(new Span<byte>(r.Payload, N.PayloadCapacity));
            var rc = File(ref r, out var request);
            id = new DecisionId(_epoch, request);
            return rc;
        }
    }

    private static bool OnePre(uint kind) => kind is N.KindEquipSingle or N.KindEquipCollection;

    private int File(ref N.Request input, out ulong id)
    {
        id = 0;
        var want = N.PayloadSize(input.Kind);
        if (input.Size != (uint)sizeof(N.Request) || input.Version != 1 || want == 0 || input.PayloadBytes != (uint)want || input.Subject.Entity == 0)
            return FakeTables.InvalidArgument;
        if (!_attached) return FakeTables.Unavailable;
        id = ++_nextId;
        var r = input;
        r.RequestId = id;
        r.WorldEpoch = _epoch;
        if (_active.Count >= MaxActive)
        {
            _dropped++;
            PushOutcome(ref r, 10, "the decision queue is full; the action was not held", _step, null, 0);
            return FakeTables.BufferTooSmall;
        }
        r.CaptureStep = _step;
        r.DeadlineStep = OnePre(r.Kind) ? _step : _step + 2;
        _active[id] = r;
        _arrivals.Add(id);
        _filed++;
        return FakeTables.Ok;
    }

    // ---- batches ----
    public int BeginRequests(ulong owner, ulong* batch, uint* records)
    {
        if (batch == null || records == null) return FakeTables.InvalidArgument;
        lock (_lock)
        {
            if (Consumer(owner) is var rc and not FakeTables.Ok) return rc;
            if (!_requests.Open)
            {
                _requests.Items.Clear();
                foreach (var id in _arrivals)
                    if (_active.TryGetValue(id, out var r)) _requests.Items.Add(r);
                _arrivals.Clear();
                if (_requests.Items.Count > 0)
                {
                    _requests.Open = true;
                    _requests.Id = ++_batchSeq;
                }
            }
            *batch = _requests.Open ? _requests.Id : 0;
            *records = _requests.Open ? (uint)_requests.Items.Count : 0;
            return FakeTables.Ok;
        }
    }

    public int ReadRequests(ulong owner, ulong batch, N.Request* buffer, uint capacity, uint stride, uint* required)
    {
        lock (_lock)
        {
            if (Consumer(owner) is var rc and not FakeTables.Ok) return rc;
            if (FailRequestReads > 0 && buffer != null)
            {
                FailRequestReads--;
                return FakeTables.Internal;
            }
            return Read(_requests, batch, (byte*)buffer, capacity, stride, required);
        }
    }

    public int AckRequests(ulong owner, ulong batch)
    {
        lock (_lock)
        {
            if (Consumer(owner) is var rc and not FakeTables.Ok) return rc;
            if (FailRequestAcks > 0)
            {
                FailRequestAcks--;
                return FakeTables.Internal;
            }
            return Ack(_requests, batch);
        }
    }

    public int BeginOutcomes(ulong owner, ulong* batch, uint* records)
    {
        if (batch == null || records == null) return FakeTables.InvalidArgument;
        lock (_lock)
        {
            if (Consumer(owner) is var rc and not FakeTables.Ok) return rc;
            if (!_outcomes.Open && _outbox.Count > 0)
            {
                _outcomes.Items = [.. _outbox];
                _outbox.Clear();
                _outcomes.Open = true;
                _outcomes.Id = ++_batchSeq;
            }
            *batch = _outcomes.Open ? _outcomes.Id : 0;
            *records = _outcomes.Open ? (uint)_outcomes.Items.Count : 0;
            return FakeTables.Ok;
        }
    }

    public int ReadOutcomes(ulong owner, ulong batch, N.Outcome* buffer, uint capacity, uint stride, uint* required)
    {
        lock (_lock)
        {
            if (Consumer(owner) is var rc and not FakeTables.Ok) return rc;
            if (FailOutcomeReads > 0 && buffer != null)
            {
                FailOutcomeReads--;
                return FakeTables.Internal;
            }
            return Read(_outcomes, batch, (byte*)buffer, capacity, stride, required);
        }
    }

    public int AckOutcomes(ulong owner, ulong batch)
    {
        lock (_lock)
        {
            if (Consumer(owner) is var rc and not FakeTables.Ok) return rc;
            if (FailOutcomeAcks > 0)
            {
                FailOutcomeAcks--;
                return FakeTables.Internal;
            }
            return Ack(_outcomes, batch);
        }
    }

    private static int Read<T>(Batch<T> b, ulong batch, byte* buffer, uint capacity, uint stride, uint* required) where T : unmanaged
    {
        if (required == null) return FakeTables.InvalidArgument;
        *required = 0;
        if (!b.Open || batch != b.Id) return FakeTables.NotFound;
        var n = (uint)b.Items.Count;
        *required = n;
        if (buffer == null || capacity < n) return FakeTables.BufferTooSmall;  // nothing written, nothing consumed
        if (stride < (uint)sizeof(T)) return FakeTables.InvalidArgument;
        for (var i = 0; i < n; i++) *(T*)(buffer + (long)i * stride) = b.Items[i];
        return FakeTables.Ok;
    }

    private int Ack<T>(Batch<T> b, ulong batch)
    {
        if (batch == 0) return FakeTables.Ok;  // the empty batch
        if (b.Open && batch == b.Id)
        {
            b.Open = false;
            b.Items.Clear();
            b.LastAcked = batch;
            b.LastAckedStep = _step;
            return FakeTables.Ok;
        }
        // the same ack again is accepted for two Pres
        return batch == b.LastAcked && _step <= b.LastAckedStep + 2 ? FakeTables.Ok : FakeTables.NotFound;
    }

    // ---- answers ----
    public int Answer(ulong owner, N.Answer* a, N.Receipt* receipt)
    {
        if (a == null) return FakeTables.InvalidArgument;
        Func<FakeDecisionAnswer, FakeDecisionFinish?>? finisher;
        N.Request r;
        lock (_lock)
        {
            if (Consumer(owner) is var rc and not FakeTables.Ok) return rc;
            if (a->Size != (uint)sizeof(N.Answer) || a->RequestId == 0) return FakeTables.InvalidArgument;
            if (receipt != null)
            {
                *receipt = default;
                receipt->Size = (uint)sizeof(N.Receipt);
            }
            if (!_active.TryGetValue(a->RequestId, out r))
            {
                // terminal already (the status again, nothing else), or never known
                if (!_terminal.TryGetValue(a->RequestId, out var done) || done.Epoch != a->WorldEpoch || done.Kind != a->Kind) return FakeTables.NotFound;
                if (receipt != null) receipt->TerminalStatus = done.Status;
                return FakeTables.Ok;
            }
            if (r.WorldEpoch != a->WorldEpoch || r.Kind != a->Kind) return FakeTables.InvalidArgument;
            finisher = Finisher;
        }
        // the kind's finisher runs on the consumer's thread, as the native producers' do
        var said = new FakeDecisionAnswer(new DecisionId(r.WorldEpoch, r.RequestId), r.Kind, new EntityId(r.Subject.Entity), a->Allow != 0, a->SurvivalHealth);
        var finish = finisher?.Invoke(said);
        lock (_lock)
        {
            if (!_active.Remove(a->RequestId)) return FakeTables.NotFound;
            _answers.Add(said);
            var status = finish is { } f ? (uint)f.Status : a->Allow != 0 ? 1u : 2u;
            if (r.Kind == N.KindWalkover && status == 2) _suppressed.Add((r.Subject.Entity, r.Actor.Entity));
            PushOutcome(ref r, status, finish?.Reason, _step, finish, a->SurvivalHealth);
            if (receipt != null)
            {
                receipt->Accepted = 1;
                receipt->TerminalStatus = status;
                WriteText(receipt->Reason, 192, finish?.Reason);
            }
            return FakeTables.Ok;
        }
    }

    /// <summary>kiln.decisions' reevaluate_pickup: forgets the suppressed walk-over exposures of (drop, receiver), 0 for
    /// any, as the native producer reopens them. Unavailable without a walk-over producer (<see cref="SupportedHookKinds"/>
    /// bit 2).</summary>
    public int PrepareKind(uint kind, byte* reason, uint size)
    {
        if (reason != null && size > 0) reason[0] = 0;
        if (kind is 0 or >= 64) return FakeTables.InvalidArgument;
        if (PreparePending && kind == N.KindDeath) {
            KitBackend.Instance.Capabilities.Report(KitBackend.CapabilityOwner, "events.death", CapState.Pending, CapReason.Session,
                "wait for Pre", KitBackend.CapabilityOwner);
            global::Kiln.Capabilities.Report("events.death", CapabilityState.Pending, CapabilityReason.Session, "wait for Pre");
            return FakeTables.Ok;
        }
        (string Row, string Reason) refusal;
        lock (_lock)
        {
            Prepared[kind] = Prepared.GetValueOrDefault(kind) + 1;
            if ((SupportedKinds >> (int)kind & 1) == 0) return FakeTables.Unavailable;
            if (!PrepareRefusals.TryGetValue(kind, out refusal)) return FakeTables.Ok;
            SupportedHookKinds &= ~(1ul << (int)kind);
        }
        KitBackend.Instance.Capabilities.Report(KitBackend.CapabilityOwner, refusal.Row, CapState.Unavailable, CapReason.Faulted, refusal.Reason,
            KitBackend.CapabilityOwner);
        var bytes = System.Text.Encoding.UTF8.GetBytes(refusal.Reason);
        var n = (int)Math.Min((uint)bytes.Length, size == 0 ? 0 : size - 1);
        for (var i = 0; i < n; i++) reason[i] = bytes[i];
        if (size > 0) reason[n] = 0;
        return FakeTables.Unavailable;
    }

    public int ReevaluatePickup(uint drop, uint receiver, uint* reset)
    {
        lock (_lock)
        {
            if (reset != null) *reset = 0;
            if ((SupportedHookKinds & (1ul << (int)N.KindWalkover)) == 0) return FakeTables.Unavailable;
            if (!WorldSafe) return FakeTables.WrongThread;
            var n = _suppressed.RemoveWhere(x => (drop == 0 || x.Drop == drop) && (receiver == 0 || x.Receiver == receiver));
            if (reset != null) *reset = (uint)n;
            return FakeTables.Ok;
        }
    }

    /// <summary>Ends an active request from its producer's side (the entity went away, the source changed). False when
    /// the id is not active.</summary>
    public bool Complete(ulong request, DecisionTerminal status, string? reason)
    {
        lock (_lock)
        {
            if (!_active.Remove(request, out var r)) return false;
            PushOutcome(ref r, (uint)status, reason, _step, null, 0);
            return true;
        }
    }

    // ---- step windows ----
    private void WindowBegin(ulong step) => Expire(step, r => !OnePre(r.Kind) && step > r.DeadlineStep);

    private void WindowEnd(ulong step) => Expire(step, r => OnePre(r.Kind) && step >= r.DeadlineStep);

    private void Expire(ulong step, Func<N.Request, bool> due)
    {
        List<(ulong Id, uint Status)>? ended = null;
        foreach (var (id, r) in _active)
        {
            if (r.WorldEpoch != _epoch) (ended ??= []).Add((id, 11));
            else if (due(r)) (ended ??= []).Add((id, 7));
        }
        if (ended is null) return;
        foreach (var (id, status) in ended)
        {
            _active.Remove(id, out var r);
            PushOutcome(ref r, status, status == 11 ? "the request belongs to a world that ended" : "no answer in time; the action was released", step, null, 0);
        }
    }

    private void ReleaseAll(uint status, string reason, ulong step)
    {
        foreach (var id in _active.Keys.ToArray())
        {
            _active.Remove(id, out var r);
            PushOutcome(ref r, status, reason, step, null, 0);
        }
    }

    private void PushOutcome(ref N.Request r, uint status, string? reason, ulong step, FakeDecisionFinish? finish, int requested)
    {
        _terminal[r.RequestId] = (r.WorldEpoch, r.Kind, status);
        _terminalOrder.Enqueue(r.RequestId);
        while (_terminalOrder.Count > MaxOutcomes) _terminal.Remove(_terminalOrder.Dequeue());
        if (status == 7) _timeouts++;
        var id = new DecisionId(r.WorldEpoch, r.RequestId);
        var known = finish is { HealthBefore: not null, AppliedHealth: not null };
        var ok = status is >= 1 and <= 3;
        _record.Add(new DecisionResult(id, (DecisionTerminal)status, new EntityId(r.Subject.Entity), new EntityId(r.Actor.Entity),
            r.Kind == N.KindDeath && requested > 0 ? requested : null, known ? finish!.Value.HealthBefore : null,
            known ? finish!.Value.AppliedHealth : null, ok ? null : reason));
        if (_outbox.Count >= MaxOutcomes)
        {
            _gap++;  // nobody reads: the record is lost, and counted as lost
            return;
        }
        N.Outcome o = default;
        o.Size = (uint)sizeof(N.Outcome);
        o.Version = 1;
        o.Kind = r.Kind;
        o.Status = status;
        o.RequestId = r.RequestId;
        o.WorldEpoch = r.WorldEpoch;
        o.Step = step;
        o.Subject = r.Subject;
        o.Actor = r.Actor;
        o.RequestedHealth = requested;
        if (known)
        {
            o.Flags |= N.OutcomeHealthKnown;
            o.HealthBefore = finish!.Value.HealthBefore!.Value;
            o.AppliedHealth = finish.Value.AppliedHealth!.Value;
        }
        WriteText(o.Reason, 192, reason);
        _outbox.Enqueue(o);
        _outcomeCount++;
    }

    private static void WriteText(byte* to, int capacity, string? text)
    {
        if (string.IsNullOrEmpty(text)) return;
        var bytes = Encoding.UTF8.GetBytes(text);
        var n = Math.Min(bytes.Length, capacity - 1);
        bytes.AsSpan(0, n).CopyTo(new Span<byte>(to, capacity));
        to[n] = 0;
    }

    // ---- what a test reads ----
    public IReadOnlyList<DecisionResult> Record { get { lock (_lock) return [.. _record]; } }
    public IReadOnlyList<FakeDecisionAnswer> Answers { get { lock (_lock) return [.. _answers]; } }
    public int ActiveCount { get { lock (_lock) return _active.Count; } }
    public bool Attached { get { lock (_lock) return _attached; } }
    public ulong Epoch { get { lock (_lock) return _epoch; } }
    public FakeDecisionCounters Counters { get { lock (_lock) return new(_filed, _dropped, _timeouts, _outcomeCount, _gap, _misses); } }
}
