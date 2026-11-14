// SPDX-License-Identifier: MIT
// The model under the kit's fake kiln.routines: what the native service decides, in C#, with no game. It takes the wire
// document (format 1) as the service does and keeps the service's rules: one owner per villager, changes queued and
// committed at the next Pre, each completed once at that step's Post, 256 queued changes and then a refusal, revisions
// that only rise, and two separate event queues (assignment changes, task changes).
//
// It also carries the pieces a test needs to stand in for the game: a clock (the game time in nanoseconds), villagers
// with the fields the chooser and the sleep check read, the plans seen per key (key -> version and hash), and a log of
// the records the keep-awake prefix marks as written, in order.
//
// The table over it (native/core/abi/services/kiln_routines.h) and the harness's surface come with the header; until
// then this is internal and has no IKitFake.
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;

namespace Kiln.TestKit;

/// <summary>A task's kind, in the wire's order.</summary>
internal enum FakeRoutineKind : byte { Vanilla, Idle, Roam, Sleep, Sit, Inspect }

/// <summary>keen::active_npc::State as the service reads it (the 18 states).</summary>
internal enum FakeVillagerState : byte
{
    Invalid = 0, Start, FindBed, Idle, DecideTask, FindTarget, GoToTarget, Roam, Sleep, Sitting, Interacting, NpcSocializing,
    Eating, WaitingCloseToPlayer, AvoidBuildingCursor, AvoidWater, InsideWater, FinishTask,
}

internal enum FakeAssignmentState { Assigned, ManualSuspended, PendingSpawn, VersionMismatch, Removed }

internal enum FakeTaskState { None, Chosen, Started, Completed, Refused, Suspended }

internal readonly record struct FakeRoutineWindow(int Start, int End, bool AllDay, int Priority, FakeRoutineKind Task);

/// <summary>A plan as the service holds it after its checks.</summary>
internal sealed record FakeRoutinePlan(string Key, uint Version, string ContentHash, IReadOnlyList<FakeRoutineWindow> Windows, FakeRoutineKind Fallback)
{
    public const int MaxWindows = 64;

    public static bool Covers(FakeRoutineWindow w, int minute) =>
        w.AllDay || (w.Start < w.End ? minute >= w.Start && minute < w.End : minute >= w.Start || minute < w.End);

    /// <summary>The window of a minute, -1 for the fallback: highest priority, the first written among equals.</summary>
    public int Select(int minute)
    {
        var best = -1;
        for (var i = 0; i < Windows.Count; i++)
            if (Covers(Windows[i], minute) && (best < 0 || Windows[i].Priority > Windows[best].Priority))
                best = i;
        return best;
    }

    public FakeRoutineKind TaskAt(int window) => window < 0 ? Fallback : Windows[window].Task;
}

/// <summary>What the service answers for one villager (its inspect).</summary>
internal sealed record FakeRoutineSnapshot(uint NpcId, string Owner, string Key, uint PlanVersion, ulong Revision, FakeAssignmentState Assignment,
    int WindowIndex, ulong Invocation, FakeRoutineKind? Task, FakeTaskState TaskState, FakeVillagerState NativeState, string? LastReason);

/// <summary>The one completion of a queued change.</summary>
internal sealed record FakeRoutineCompletion(ulong Ticket, bool Ok, string? Reason, uint NpcId, ulong Revision);

internal sealed record FakeRoutineAssignmentEvent(FakeRoutineSnapshot? Before, FakeRoutineSnapshot After, string Reason, bool Teardown);

internal sealed record FakeRoutineTaskEvent(uint NpcId, string Key, uint PlanVersion, ulong Revision, ulong Invocation, FakeRoutineKind? Task,
    FakeTaskState From, FakeTaskState To, string? Reason, bool Teardown);

/// <summary>A villager as the hooks see it: the fields of its records the chooser and the sleep check read or write.</summary>
internal sealed class FakeRoutineVillager(uint npcId)
{
    public uint NpcId { get; } = npcId;
    public FakeVillagerState State = FakeVillagerState.DecideTask, Previous = FakeVillagerState.DecideTask;
    /// <summary>keen::active_npc::Decision (Sleep is 5).</summary>
    public byte Decision;
    /// <summary>NpcCommon's first byte; bit 0x10 is the forced-sleep flag the sleep check clears after the wake time.</summary>
    public byte CommonFlags;
    public long SleepNs = 21 * 3_600_000_000_000L, WakeNs = 5 * 3_600_000_000_000L;
    public byte TaskFailures;
    /// <summary>What the game's own sleep check answers when it is called (the game lets this villager sleep now).</summary>
    public bool NativeSleepAllowed;
    /// <summary>How often the game's own chooser and sleep check ran for it.</summary>
    public int VanillaChoices, NativeSleepChecks;
}

/// <summary>What a call of the table answers at once.</summary>
internal enum FakeRoutineCall { Ok, InvalidArgument, QueueFull, WorldStopped }

internal sealed class RoutinesModel
{
    public const int MaxQueued = 256;
    public const string OwnedBy = "OwnedBy:", NotAssigned = "NotAssigned", WorldStoppedReason = "WorldStopped", Revoked = "Revoked",
        SleepNotAllowed = "NativeSleepNotCurrentlyAllowed", OwnerUnloaded = "OwnerUnloaded";
    private const long NsPerMinute = 60_000_000_000;

    private enum Op { Assign, Remove, Resume }

    private sealed record Change(ulong Ticket, Op Op, string Owner, uint NpcId, FakeRoutinePlan? Plan);

    private sealed class Assignment
    {
        public required string Owner;
        public required FakeRoutinePlan Plan;
        public ulong Revision;
        public bool Suspended;
        public FakeAssignmentState State = FakeAssignmentState.Assigned;
        public int Window = -1;
        public ulong Invocation;
        public FakeRoutineKind? Task;
        public FakeTaskState TaskState;
        public string? LastReason;
    }

    private readonly List<Change> _queue = [];
    private readonly List<FakeRoutineCompletion> _done = [];
    private readonly List<FakeRoutineAssignmentEvent> _assignmentEvents = [];
    private readonly List<FakeRoutineTaskEvent> _taskEvents = [];
    private readonly Dictionary<uint, Assignment> _assignments = [];
    private readonly Dictionary<uint, FakeRoutineVillager> _villagers = [];
    private readonly HashSet<string> _revoked = new(StringComparer.Ordinal);
    private ulong _tickets, _revision, _invocations;
    private bool _stopped;

    // ---- what a test stands in for --------------------------------------------------------------------------------

    /// <summary>The game time, in nanoseconds.</summary>
    public long NowNs;

    /// <summary>The minute of the game day: floor(ns / one minute) modulo 1440.</summary>
    public int Minute
    {
        get
        {
            var minutes = Math.DivRem(NowNs, NsPerMinute, out var rest);
            if (rest < 0) minutes--;
            var m = minutes % 1440;
            return (int)(m < 0 ? m + 1440 : m);
        }
    }

    /// <summary>Sets the clock to a time of the game day.</summary>
    public void SetClock(int hour, int minute = 0) => NowNs = (hour * 60L + minute) * NsPerMinute;

    public FakeRoutineVillager Villager(uint npcId) =>
        _villagers.TryGetValue(npcId, out var v) ? v : _villagers[npcId] = new FakeRoutineVillager(npcId);

    /// <summary>The plans the service has bound, by key: the version and hash of the last one committed.</summary>
    public Dictionary<string, (uint Version, string ContentHash)> Plans { get; } = new(StringComparer.Ordinal);

    /// <summary>The records the keep-awake prefix marked as written, in order: "dyn" (DynamicActiveNpcState) and
    /// "common" (NpcCommon), with the villager.</summary>
    public List<(uint NpcId, string Record)> Marks { get; } = [];

    // ---- what the service answers ---------------------------------------------------------------------------------

    /// <summary>Completions of queued changes, in the order they were completed: one per ticket, ever.</summary>
    public List<FakeRoutineCompletion> Completions { get; } = [];

    /// <summary>Assignment changes, posted on Post (teardown ones at the stop).</summary>
    public List<FakeRoutineAssignmentEvent> AssignmentEvents { get; } = [];

    /// <summary>Task changes, posted on Post (teardown ones at the stop).</summary>
    public List<FakeRoutineTaskEvent> TaskEvents { get; } = [];

    public int Queued => _queue.Count;

    /// <summary>The highest revision given so far.</summary>
    public ulong Revision => _revision;

    /// <summary>Starts the revisions after what a world saved.</summary>
    public void StartRevisionsAfter(ulong last) => _revision = Math.Max(_revision, last);

    public FakeRoutineCall Assign(string owner, uint npcId, string planJson, out ulong ticket, out string? problem)
    {
        ticket = 0;
        problem = null;
        if (string.IsNullOrEmpty(owner) || npcId == 0)
        {
            problem = "an owner and a villager";
            return FakeRoutineCall.InvalidArgument;
        }
        if (!TryParse(planJson, out var plan, out problem)) return FakeRoutineCall.InvalidArgument;
        // the owner comes from the caller, never from the JSON: a key of another mod is a bad argument
        if (!plan!.Key.StartsWith(owner + ":", StringComparison.Ordinal))
        {
            problem = $"the plan {plan.Key} is not a plan of {owner}";
            return FakeRoutineCall.InvalidArgument;
        }
        return Enqueue(Op.Assign, owner, npcId, plan, out ticket, out problem);
    }

    public FakeRoutineCall Remove(string owner, uint npcId, out ulong ticket) => Enqueue(Op.Remove, owner, npcId, null, out ticket, out _);

    public FakeRoutineCall Resume(string owner, uint npcId, out ulong ticket) => Enqueue(Op.Resume, owner, npcId, null, out ticket, out _);

    private FakeRoutineCall Enqueue(Op op, string owner, uint npcId, FakeRoutinePlan? plan, out ulong ticket, out string? problem)
    {
        ticket = 0;
        problem = null;
        if (_stopped)
        {
            problem = WorldStoppedReason;
            return FakeRoutineCall.WorldStopped;
        }
        // overflow refuses before anything changes: no ticket, no completion, no event
        if (_queue.Count >= MaxQueued)
        {
            problem = "QueueFull";
            return FakeRoutineCall.QueueFull;
        }
        ticket = ++_tickets;
        _queue.Add(new Change(ticket, op, owner, npcId, plan));
        return FakeRoutineCall.Ok;
    }

    public FakeRoutineSnapshot? Inspect(uint npcId) => _assignments.TryGetValue(npcId, out var a) ? Snapshot(npcId, a) : null;

    /// <summary>A manual command took the villager (go to, teleport, stay, follow; or the game's own command): the routine
    /// is suspended at once, the event comes on Post. Already suspended, or no routine: nothing.</summary>
    public void ManualSuspend(uint npcId, string reason)
    {
        if (!_assignments.TryGetValue(npcId, out var a) || a.Suspended) return;
        var before = Snapshot(npcId, a);
        a.Suspended = true;
        a.State = FakeAssignmentState.ManualSuspended;
        a.Revision = ++_revision;
        a.LastReason = reason;
        if (a.TaskState is FakeTaskState.Chosen or FakeTaskState.Started) Move(npcId, a, FakeTaskState.Suspended, reason, false);
        _assignmentEvents.Add(new(before, Snapshot(npcId, a), "ManualSuspended:" + reason, false));
    }

    /// <summary>The owner unloads: its queued changes complete now with Revoked, its assignments go at the next Pre.</summary>
    public void RevokeOwner(string owner)
    {
        foreach (var c in _queue.Where(c => c.Owner == owner).ToList())
        {
            _queue.Remove(c);
            Complete(c, false, Revoked, _assignments.TryGetValue(c.NpcId, out var a) ? a.Revision : 0);
        }
        _revoked.Add(owner);
    }

    /// <summary>The step's Pre: an unloaded owner's assignments go, then every queued change commits in order.</summary>
    public void Pre()
    {
        if (_stopped) return;
        foreach (var owner in _revoked)
            foreach (var (npcId, a) in _assignments.Where(x => x.Value.Owner == owner).ToList())
                Drop(npcId, a, OwnerUnloaded);
        _revoked.Clear();
        foreach (var c in _queue)
        {
            _assignments.TryGetValue(c.NpcId, out var a);
            if (a != null && a.Owner != c.Owner)
            {
                // another mod's routine is on this villager: refused, nothing written, no event
                _done.Add(new(c.Ticket, false, OwnedBy + a.Owner, c.NpcId, a.Revision));
                continue;
            }
            switch (c.Op)
            {
                case Op.Assign:
                    var before = a == null ? null : Snapshot(c.NpcId, a);
                    if (a == null) _assignments[c.NpcId] = a = new Assignment { Owner = c.Owner, Plan = c.Plan! };
                    else
                    {
                        // the same owner replaces its plan; a manual suspension stays until an explicit resume
                        if (a.TaskState is FakeTaskState.Chosen or FakeTaskState.Started) Move(c.NpcId, a, FakeTaskState.Suspended, "Replaced", false);
                        a.Plan = c.Plan!;
                        (a.Window, a.Task, a.TaskState) = (-1, null, FakeTaskState.None);
                    }
                    a.Revision = ++_revision;
                    Plans[a.Plan.Key] = (a.Plan.Version, a.Plan.ContentHash);
                    _assignmentEvents.Add(new(before, Snapshot(c.NpcId, a), before == null ? "Assigned" : "Replaced", false));
                    _done.Add(new(c.Ticket, true, null, c.NpcId, a.Revision));
                    break;
                case Op.Remove:
                    // nothing to remove is already as asked: ok, no revision, no event
                    if (a == null) _done.Add(new(c.Ticket, true, null, c.NpcId, 0));
                    else _done.Add(new(c.Ticket, true, null, c.NpcId, Drop(c.NpcId, a, "Removed")));
                    break;
                case Op.Resume:
                    if (a == null) _done.Add(new(c.Ticket, false, NotAssigned, c.NpcId, 0));
                    else if (!a.Suspended) _done.Add(new(c.Ticket, true, null, c.NpcId, a.Revision));
                    else
                    {
                        var was = Snapshot(c.NpcId, a);
                        a.Suspended = false;
                        a.State = FakeAssignmentState.Assigned;
                        a.Revision = ++_revision;
                        _assignmentEvents.Add(new(was, Snapshot(c.NpcId, a), "Resumed", false));
                        _done.Add(new(c.Ticket, true, null, c.NpcId, a.Revision));
                    }
                    break;
            }
        }
        _queue.Clear();
    }

    /// <summary>The step's Post: the completions of what this step's Pre committed, then the events.</summary>
    public void Post()
    {
        Completions.AddRange(_done);
        _done.Clear();
        AssignmentEvents.AddRange(_assignmentEvents);
        _assignmentEvents.Clear();
        TaskEvents.AddRange(_taskEvents);
        _taskEvents.Clear();
    }

    /// <summary>The world stops: queued changes complete with WorldStopped; every assignment stays (it is saved), reads
    /// PendingSpawn, and a running task reads Suspended; the events carry Teardown.</summary>
    public void StopWorld()
    {
        if (_stopped) return;
        _stopped = true;
        Post();
        foreach (var c in _queue) Complete(c, false, WorldStoppedReason, _assignments.TryGetValue(c.NpcId, out var a) ? a.Revision : 0);
        _queue.Clear();
        foreach (var (npcId, a) in _assignments)
        {
            var before = Snapshot(npcId, a);
            if (a.TaskState is FakeTaskState.Chosen or FakeTaskState.Started) Move(npcId, a, FakeTaskState.Suspended, WorldStoppedReason, true);
            a.State = FakeAssignmentState.PendingSpawn;
            AssignmentEvents.Add(new(before, Snapshot(npcId, a), WorldStoppedReason, true));
        }
        TaskEvents.AddRange(_taskEvents);
        _taskEvents.Clear();
    }

    // ---- the hooks, as a test drives them ---------------------------------------------------------------------------

    /// <summary>The villager's chooser runs (the game's DecideTask) with <paramref name="phase"/>. Without a routine, with a
    /// suspended one, and in phase 2, the game's own code runs once and nothing else happens. Phase 3 is a pause: no
    /// choice. Else the window of the minute decides. Returns the task taken, or null for the game's own.</summary>
    public FakeRoutineKind? Decide(uint npcId, byte phase = 1)
    {
        var v = Villager(npcId);
        if (_stopped || !_assignments.TryGetValue(npcId, out var a) || a.Suspended || a.State != FakeAssignmentState.Assigned || phase == 2)
        {
            v.VanillaChoices++;
            return null;
        }
        if (phase == 3) return null;
        var window = a.Plan.Select(Minute);
        var kind = a.Plan.TaskAt(window);
        Begin(npcId, a, window, kind);
        if (kind == FakeRoutineKind.Sleep)
        {
            // the game's own sleep check, once, at the decision: it either took FindBed itself or sleep is not allowed now
            v.NativeSleepChecks++;
            if (v.NativeSleepAllowed)
            {
                (v.Decision, v.Previous, v.State) = (5, v.State, FakeVillagerState.FindBed);
                Move(npcId, a, FakeTaskState.Started, null, false);
                return kind;
            }
            Move(npcId, a, FakeTaskState.Refused, SleepNotAllowed, false);
            kind = a.Plan.Fallback;
            Begin(npcId, a, -1, kind);
        }
        switch (kind)
        {
            case FakeRoutineKind.Vanilla:
                v.VanillaChoices++;
                break;
            case FakeRoutineKind.Idle:
                Enter(v, 7, FakeVillagerState.Idle);
                break;
            case FakeRoutineKind.Roam:
                Enter(v, 1, FakeVillagerState.Roam);
                break;
            case FakeRoutineKind.Sit:
                Enter(v, 2, FakeVillagerState.FindTarget);
                break;
            case FakeRoutineKind.Inspect:
                Enter(v, 3, FakeVillagerState.FindTarget);
                break;
        }
        Move(npcId, a, FakeTaskState.Started, null, false);
        return kind;
    }

    private static void Enter(FakeRoutineVillager v, byte decision, FakeVillagerState state)
    {
        v.Decision = decision;
        v.TaskFailures = 0;   // the game's SetDecision clears them
        (v.Previous, v.State) = (v.State, state);
    }

    /// <summary>The game finished (<paramref name="ok"/>) or failed the villager's current task state.</summary>
    public void Finish(uint npcId, bool ok, string? reason = null)
    {
        var v = Villager(npcId);
        if (!ok && v.TaskFailures < byte.MaxValue) v.TaskFailures++;
        v.State = FakeVillagerState.DecideTask;
        if (!_assignments.TryGetValue(npcId, out var a) || a.TaskState is not (FakeTaskState.Chosen or FakeTaskState.Started)) return;
        Move(npcId, a, ok ? FakeTaskState.Completed : FakeTaskState.Refused, ok ? null : reason ?? "Failed", false);
    }

    /// <summary>The game's sleep check runs for the villager (from the chooser or the interrupt pass). The game's own runs
    /// exactly once for a villager without a routine, a suspended one, or a window of vanilla or sleep. Otherwise the
    /// keep-awake prefix runs instead: the false path's marks and its flag clearing, no transition, and the answer is false.</summary>
    public bool SleepCheck(uint npcId)
    {
        var v = Villager(npcId);
        var owned = !_stopped && _assignments.TryGetValue(npcId, out var a) && !a.Suspended && a.State == FakeAssignmentState.Assigned;
        if (!owned || _assignments[npcId].Plan.TaskAt(_assignments[npcId].Plan.Select(Minute)) is FakeRoutineKind.Vanilla or FakeRoutineKind.Sleep)
        {
            v.NativeSleepChecks++;
            return v.NativeSleepAllowed;
        }
        // 1. the state, and for GoToTarget the decision
        Mark(v, "dyn");
        if (v.State is not (FakeVillagerState.Idle or FakeVillagerState.DecideTask or FakeVillagerState.Roam))
        {
            if (v.State != FakeVillagerState.GoToTarget) return false;
            Mark(v, "dyn");
            if (v.Decision == 5) return false;
        }
        // 2. the sleep window's two reads
        Mark(v, "common");
        Mark(v, "common");
        // 3. the forced-sleep flag is cleared after the wake time, as the false path does
        if ((v.CommonFlags & 0x10) != 0)
        {
            Mark(v, "common");
            Mark(v, "common");
            if (NowNs > v.WakeNs)
            {
                Mark(v, "common");
                v.CommonFlags &= unchecked((byte)~0x10);
            }
        }
        // 4. no transition: decision, state, previous state and failures are as they were
        return false;
    }

    private void Mark(FakeRoutineVillager v, string record) => Marks.Add((v.NpcId, record));

    // ---- inside -------------------------------------------------------------------------------------------------------

    private void Begin(uint npcId, Assignment a, int window, FakeRoutineKind kind)
    {
        (a.Window, a.Task, a.Invocation, a.TaskState) = (window, kind, ++_invocations, FakeTaskState.None);
        Move(npcId, a, FakeTaskState.Chosen, null, false);
    }

    private void Move(uint npcId, Assignment a, FakeTaskState to, string? reason, bool teardown)
    {
        _taskEvents.Add(new(npcId, a.Plan.Key, a.Plan.Version, a.Revision, a.Invocation, a.Task, a.TaskState, to, reason, teardown));
        a.TaskState = to;
        if (reason != null) a.LastReason = reason;
    }

    private ulong Drop(uint npcId, Assignment a, string reason)
    {
        var before = Snapshot(npcId, a);
        if (a.TaskState is FakeTaskState.Chosen or FakeTaskState.Started) Move(npcId, a, FakeTaskState.Suspended, reason, false);
        _assignments.Remove(npcId);
        a.Revision = ++_revision;
        a.State = FakeAssignmentState.Removed;
        _assignmentEvents.Add(new(before, Snapshot(npcId, a), reason, false));
        return a.Revision;
    }

    private void Complete(Change c, bool ok, string? reason, ulong revision) => Completions.Add(new(c.Ticket, ok, reason, c.NpcId, revision));

    private FakeRoutineSnapshot Snapshot(uint npcId, Assignment a) =>
        new(npcId, a.Owner, a.Plan.Key, a.Plan.Version, a.Revision, a.State, a.Window, a.Invocation, a.Task, a.TaskState, Villager(npcId).State, a.LastReason);

    // ---- the wire document, read as the service reads it ------------------------------------------------------------

    private static readonly string[] KindNames = ["vanilla", "idle", "roam", "sleep", "sit", "inspect"];

    /// <summary>The SHA-256 (lower-case hex) of a plan's canonical document: format, planVersion, windows and fallback with
    /// their properties in ordinal order and no whitespace.</summary>
    public static string HashOf(uint version, IReadOnlyList<FakeRoutineWindow> windows, FakeRoutineKind fallback)
    {
        var s = new StringBuilder();
        s.Append("{\"fallback\":{\"kind\":\"").Append(KindNames[(int)fallback]).Append("\"},\"format\":1,\"planVersion\":").Append(version).Append(",\"windows\":[");
        for (var i = 0; i < windows.Count; i++)
        {
            var w = windows[i];
            if (i > 0) s.Append(',');
            s.Append("{\"allDay\":").Append(w.AllDay ? "true" : "false").Append(",\"endMinute\":").Append(w.End).Append(",\"priority\":").Append(w.Priority)
                .Append(",\"startMinute\":").Append(w.Start).Append(",\"task\":{\"kind\":\"").Append(KindNames[(int)w.Task]).Append("\"}}");
        }
        s.Append("]}");
        return Convert.ToHexStringLower(SHA256.HashData(Encoding.UTF8.GetBytes(s.ToString())));
    }

    /// <summary>Reads a wire document (format 1) with the service's checks. False with the problem when it is refused.</summary>
    public static bool TryParse(string json, out FakeRoutinePlan? plan, out string? problem)
    {
        plan = null;
        try
        {
            using var doc = JsonDocument.Parse(json);
            var root = doc.RootElement;
            if (root.ValueKind != JsonValueKind.Object) return Bad("not an object", out problem);
            int? format = null;
            uint? version = null;
            string? key = null, hash = null;
            List<FakeRoutineWindow>? windows = null;
            FakeRoutineKind? fallback = null;
            var seen = new HashSet<string>(StringComparer.Ordinal);
            foreach (var p in root.EnumerateObject())
            {
                if (!seen.Add(p.Name)) return Bad($"{p.Name} is there twice", out problem);
                switch (p.Name)
                {
                    case "format": format = p.Value.GetInt32(); break;
                    case "key": key = p.Value.GetString(); break;
                    case "planVersion": version = p.Value.GetUInt32(); break;
                    case "contentHash": hash = p.Value.GetString(); break;
                    case "fallback":
                        if (Kind(p.Value) is not { } f) return Bad("fallback is not a task of this version", out problem);
                        fallback = f;
                        break;
                    case "windows":
                        if (p.Value.GetArrayLength() > FakeRoutinePlan.MaxWindows) return Bad($"more than {FakeRoutinePlan.MaxWindows} windows", out problem);
                        windows = [];
                        foreach (var w in p.Value.EnumerateArray())
                        {
                            if (w.EnumerateObject().Count() != 5 || Kind(w.GetProperty("task")) is not { } task) return Bad($"windows[{windows.Count}] is not a window of this version", out problem);
                            windows.Add(new(w.GetProperty("startMinute").GetInt32(), w.GetProperty("endMinute").GetInt32(), w.GetProperty("allDay").GetBoolean(), w.GetProperty("priority").GetInt32(), task));
                        }
                        break;
                    default: return Bad($"\"{p.Name}\" is not part of a plan", out problem);
                }
            }
            if (format != 1) return Bad("format is not 1", out problem);
            if (key is null || version is null || hash is null || windows is null || fallback is null) return Bad("a property is missing", out problem);
            if (key.IndexOf(':') <= 0 || key.EndsWith(':')) return Bad("key is not <mod>:<name>", out problem);
            if (version == 0) return Bad("planVersion is 0", out problem);
            foreach (var w in windows)
            {
                if (w.AllDay ? w.Start != 0 || w.End != 0 : w.Start is < 0 or >= 1440 || w.End is < 0 or >= 1440 || w.Start == w.End)
                    return Bad("a window's minutes are wrong", out problem);
            }
            if (fallback is not (FakeRoutineKind.Vanilla or FakeRoutineKind.Idle or FakeRoutineKind.Roam)) return Bad("the fallback is not vanilla, idle or roam", out problem);
            if (HashOf(version.Value, windows, fallback.Value) != hash) return Bad("contentHash is not the hash of the content", out problem);
            plan = new FakeRoutinePlan(key, version.Value, hash, windows, fallback.Value);
            problem = null;
            return true;
        }
        catch (Exception e) when (e is JsonException or InvalidOperationException or FormatException or KeyNotFoundException)
        {
            return Bad(e.Message, out problem);
        }
    }

    private static FakeRoutineKind? Kind(JsonElement task)
    {
        if (task.ValueKind != JsonValueKind.Object || task.EnumerateObject().Count() != 1 || !task.TryGetProperty("kind", out var k) || k.ValueKind != JsonValueKind.String) return null;
        var i = Array.IndexOf(KindNames, k.GetString());
        return i < 0 ? null : (FakeRoutineKind)i;
    }

    private static bool Bad(string why, out string? problem)
    {
        problem = why;
        return false;
    }
}
