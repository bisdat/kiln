// SPDX-License-Identifier: MIT
// The kit's fake kiln.routines (16g §5): the service's rules (RoutinesModel) on the harness's steps, so Kiln.Blocks' own
// Routines code and a mod's use of it run offline.
using System.Text;
using System.Text.Json;

namespace Kiln.TestKit;

/// <summary>Plugs the fake kiln.routines into the kit (<see cref="IKitFake"/>).</summary>
internal sealed class RoutinesFake : IKitFake
{
    public string Service => RoutinesAbi.Service;

    public void Register()
    {
        KitBackend.Instance.RegisterTable(RoutinesAbi.Service, RoutinesTable.Create());
        FakeTables.Features([RoutinesAbi.Decisions]);
    }

    public void Start(KilnHarness harness) => StateOf(harness).Start();

    public void BeforeStep(KilnHarness harness, ulong step) => StateOf(harness).Step();

    public void End(KilnHarness harness) => StateOf(harness).Stop();

    public void AddCoverage(List<CoverageRow> rows) => rows.Add(new CoverageRow("Villager routines", CoverageKind.RecordingFake,
        [RoutinesAbi.Service, RoutinesAbi.Decisions],
        "Kiln.Blocks' Routines (Assign, Remove, Resume, Inspect) and both routine events on the kit's kiln.routines: one owner per " +
        "villager (named by NpcId), changes queued and committed at the start of the next step, each completed once in that step, " +
        "256 queued changes and then QueueFull, revisions per world, OwnedBy/NotAssigned/Revoked/WorldStopped as the service " +
        "answers them; the harness's end stops the world (teardown events). The villager's decisions are the test's " +
        "(KilnHarness.Routines: SetClock, Decide, Finish, SetSleepAllowed); within one step assignment events come before task " +
        "events. Offline: the game's villager worker, its chooser and sleep check (nothing chooses by itself), manual suspension " +
        "(S5), restore after a restart"));

    public static RoutinesState StateOf(KilnHarness harness) => KitFakes.State<RoutinesState>(harness);
}

/// <summary>The fake service of one harness: the model, the owners' bindings, and the completions it owes (tick thread,
/// except <see cref="Revoke"/> under the lock).</summary>
internal sealed unsafe class RoutinesState
{
    private readonly object _lock = new();
    private RoutinesModel _model = new();
    private readonly Dictionary<ulong, string> _ownerMod = [];
    private readonly Dictionary<string, ulong> _modOwner = new(StringComparer.Ordinal);
    private readonly Dictionary<ulong, (nint Done, nint User)> _waiting = [];
    private int _completions, _assignmentEvents, _taskEvents;

    public void Start()
    {
        lock (_lock)
        {
            _model = new RoutinesModel();
            _ownerMod.Clear();
            _modOwner.Clear();
            _waiting.Clear();
            _completions = _assignmentEvents = _taskEvents = 0;
        }
    }

    /// <summary>The model, for the harness's surface. Use under <see cref="With{T}"/>.</summary>
    public T With<T>(Func<RoutinesModel, T> read)
    {
        lock (_lock) return read(_model);
    }

    // ---- the table's calls ----

    public int Assign(ulong owner, uint npcId, string json, nint done, nint user, ulong* ticket)
    {
        if (owner == 0 || npcId == 0 || done == 0 || ticket == null) return FakeTables.InvalidArgument;
        if (!RoutinesModel.TryParse(json, out var plan, out _)) return FakeTables.InvalidArgument;
        var mod = plan!.Key[..plan.Key.IndexOf(':')];
        lock (_lock)
        {
            // the owner's mod is the key's: the first plan binds it, and neither side moves while it lasts (as native)
            if (_ownerMod.TryGetValue(owner, out var bound) && bound != mod) return FakeTables.InvalidArgument;
            if (_modOwner.TryGetValue(mod, out var holder) && holder != owner) return FakeTables.InvalidArgument;
            var rc = Answer(_model.Assign(Name(owner), npcId, json, out var t, out _, mod));
            if (rc != FakeTables.Ok) return rc;
            _ownerMod[owner] = mod;
            _modOwner[mod] = owner;
            _waiting[t] = (done, user);
            *ticket = t;
            return FakeTables.Ok;
        }
    }

    public int Change(bool remove, ulong owner, uint npcId, nint done, nint user, ulong* ticket)
    {
        if (owner == 0 || npcId == 0 || done == 0 || ticket == null) return FakeTables.InvalidArgument;
        lock (_lock)
        {
            ulong t;
            var rc = Answer(remove ? _model.Remove(Name(owner), npcId, out t) : _model.Resume(Name(owner), npcId, out t));
            if (rc != FakeTables.Ok) return rc;
            _waiting[t] = (done, user);
            *ticket = t;
            return FakeTables.Ok;
        }
    }

    public void Revoke(ulong owner)
    {
        lock (_lock)
        {
            _model.RevokeOwner(Name(owner));
            // a revoked owner no longer holds its mod: the mod's next owner (a reload) can queue its plans at once
            if (_ownerMod.Remove(owner, out var mod)) _modOwner.Remove(mod);
        }
    }

    public string? SnapshotJson(uint npcId)
    {
        lock (_lock) return _model.Inspect(npcId) is { } s ? Snapshot(s) : null;
    }

    private static string Name(ulong owner) => "owner " + owner;

    private static int Answer(FakeRoutineCall call) => call switch
    {
        FakeRoutineCall.Ok => FakeTables.Ok,
        FakeRoutineCall.QueueFull => FakeTables.BufferTooSmall,
        FakeRoutineCall.WorldStopped => FakeTables.Unavailable,
        _ => FakeTables.InvalidArgument,
    };

    // ---- the steps ----

    /// <summary>The service's Pre and Post of one step: the queue commits, then the completions and the events go out.</summary>
    public void Step()
    {
        lock (_lock)
        {
            _model.Pre();
            _model.Post();
        }
        Deliver();
    }

    /// <summary>The world stops: what is queued completes WorldStopped, and the teardown events go out.</summary>
    public void Stop()
    {
        lock (_lock) _model.StopWorld();
        Deliver();
    }

    private void Deliver()
    {
        List<(nint Done, nint User, FakeRoutineCompletion C)> completions = [];
        List<string> events = [];
        lock (_lock)
        {
            for (; _completions < _model.Completions.Count; _completions++)
            {
                var c = _model.Completions[_completions];
                if (_waiting.Remove(c.Ticket, out var cb)) completions.Add((cb.Done, cb.User, c));
            }
            for (; _assignmentEvents < _model.AssignmentEvents.Count; _assignmentEvents++) events.Add(AssignmentJson(_model.AssignmentEvents[_assignmentEvents]));
            for (; _taskEvents < _model.TaskEvents.Count; _taskEvents++) events.Add(TaskJson(_model.TaskEvents[_taskEvents]));
        }
        foreach (var (done, user, c) in completions) RoutinesTable.Complete(done, user, c);
        foreach (var e in events) RoutinesTable.Send(e);
    }

    // ---- the service's JSON (kiln_routines.h) ----

    private static readonly string[] KindNames = ["vanilla", "idle", "roam", "sleep", "sit", "inspect"];

    private static string Json(Action<Utf8JsonWriter> write)
    {
        var buffer = new MemoryStream();
        using (var w = new Utf8JsonWriter(buffer)) write(w);
        return Encoding.UTF8.GetString(buffer.ToArray());
    }

    private static void Kind(Utf8JsonWriter w, string name, FakeRoutineKind? kind)
    {
        if (kind is { } k) w.WriteString(name, KindNames[(int)k]);
        else w.WriteNull(name);
    }

    private static void Optional(Utf8JsonWriter w, string name, string? value)
    {
        if (string.IsNullOrEmpty(value)) w.WriteNull(name);
        else w.WriteString(name, value);
    }

    private static void WriteSnapshot(Utf8JsonWriter w, FakeRoutineSnapshot s)
    {
        w.WriteStartObject();
        w.WriteNumber("npcId", s.NpcId);
        w.WriteString("owner", s.Owner);
        w.WriteString("key", s.Key);
        w.WriteNumber("planVersion", s.PlanVersion);
        w.WriteNumber("revision", s.Revision);
        w.WriteString("assignment", s.Assignment.ToString());
        w.WriteNumber("windowIndex", s.Task is null ? -1 : s.WindowIndex);
        w.WriteNumber("invocation", s.Invocation);
        Kind(w, "task", s.Task);
        w.WriteString("taskState", s.TaskState.ToString());
        w.WriteNumber("nativeState", (byte)s.NativeState);
        Optional(w, "lastReason", s.LastReason);
        w.WriteBoolean("paused", false);
        w.WriteEndObject();
    }

    internal static string Snapshot(FakeRoutineSnapshot s) => Json(w => WriteSnapshot(w, s));

    internal static string AssignmentJson(FakeRoutineAssignmentEvent e) => Json(w =>
    {
        w.WriteStartObject();
        w.WriteString("type", "assignment");
        w.WritePropertyName("before");
        if (e.Before is { } before) WriteSnapshot(w, before);
        else w.WriteNullValue();
        w.WritePropertyName("after");
        WriteSnapshot(w, e.After);
        w.WriteString("reason", e.Reason);
        w.WriteBoolean("teardown", e.Teardown);
        w.WriteEndObject();
    });

    internal static string TaskJson(FakeRoutineTaskEvent e) => Json(w =>
    {
        w.WriteStartObject();
        w.WriteString("type", "task");
        w.WriteNumber("npcId", e.NpcId);
        w.WriteString("key", e.Key);
        w.WriteNumber("planVersion", e.PlanVersion);
        w.WriteNumber("revision", e.Revision);
        w.WriteNumber("invocation", e.Invocation);
        w.WriteNumber("windowIndex", e.WindowIndex);
        Kind(w, "task", e.Task);
        w.WriteString("from", e.From.ToString());
        w.WriteString("to", e.To.ToString());
        Optional(w, "reason", e.Reason);
        w.WriteBoolean("teardown", e.Teardown);
        w.WriteEndObject();
    });
}
