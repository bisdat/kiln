// SPDX-License-Identifier: MIT
// A routed weapon's moves, applied as one generation (chapter 16c, "Atomic generation decision"). MoveRuntime's own path applies move by move and logs
// what it could not do; a weapon with follow-ups must not end up half routed, so it goes through here instead:
//   1. plan    MoveRoutes.Plan on the game's rows: every source, target, selector, state and timing is checked before anything changes;
//   2. stage   a fresh set of clones, named by the plan's hash, that no item slot points at yet. A clone the game refuses ends the attempt;
//   3. write   one pass later, when the clones are in the world, in one content transaction of the mod's owner (kiln.content txn_begin/txn_end):
//              the clones' rules (states, retime, links), every link read back, the item's slots, every slot read back;
//   4. commit  only when everything read back: then the status says Applied and the event is raised. Anything else rolls the transaction back:
//              kiln.content puts the owner's layers back exactly as they were (fields, list headers, the sequence ids that follow a reference) and
//              reads every one back; the slots are read again here. A rollback that does not read back revokes every layer of the mod's owner
//              (fail closed) and the status says so.
// A clone cannot be taken out of a running game, so the clones of an attempt that was refused or replaced stay registered and unused; that is why a
// weapon takes at most 16 distinct plans per process. The applied generation is written again, through the same transaction, after every content
// application of the mod (BlockRuntime.ApplyRules -> Replay): a replay that does not read back is a refusal in the status, not only a log line.
using System.Collections.Concurrent;
using System.Collections.Frozen;
using System.Text.Json.Nodes;

namespace Kiln;

/// <summary>How a rolled-back content transaction ended.</summary>
internal enum MoveRestore
{
    /// <summary>The owner's layers are what they were at the start, every touched field read back.</summary>
    Restored,
    /// <summary>The rollback did not read back: every layer of the owner was taken off.</summary>
    Revoked,
    /// <summary>The owner's layers were taken off while it was open: nothing was put back.</summary>
    Cancelled,
}

/// <summary>A content transaction of one owner: kiln.content's on a game, a made-up one in tests.</summary>
internal interface IMoveTransaction
{
    /// <summary>Applies a rules document as the owner's layers, inside the transaction; the errors of its report.</summary>
    IReadOnlyList<string> Apply(JsonObject document, string source);
    /// <summary>Keeps what was applied. False: the owner's layers were taken off meanwhile (nothing is kept).</summary>
    bool Commit();
    /// <summary>Takes back what was applied and reads it back.</summary>
    (MoveRestore Outcome, string? Error) Rollback();
}

/// <summary>What applying a generation needs from the running game: MoveRuntime's on a game, a made-up one in tests.</summary>
internal interface IMoveHost
{
    /// <summary>The content generation the rows below are read in.</summary>
    ulong ContentGeneration { get; }
    IReadOnlyList<SlotRow>? Slots(string item);
    Func<string, IReadOnlyList<SubSequence>?> Reader();
    IPlayerStateLookup? Lookup { get; }
    /// <summary>Every sequence clone the game data service knows: its state (<c>registered</c>, <c>pending</c>, <c>waiting</c>, <c>error</c>) and error.</summary>
    IReadOnlyDictionary<Guid, (string State, string? Error)> Clones();
    /// <summary>Defines a sequence clone as <paramref name="owner"/>'s. Throws what kiln.gamedata throws when it refuses.</summary>
    void Define(JsonObject definition, ulong owner);
    /// <summary>Opens a content transaction of <paramref name="owner"/>. Throws when it cannot (no transactions here, one is open).</summary>
    IMoveTransaction Begin(ulong owner);
    /// <summary>Takes every layer of <paramref name="owner"/> off (fail closed).</summary>
    void Revoke(ulong owner);
    /// <summary>What a clone's window leads to now. Null: it cannot be read.</summary>
    Guid? Link(string clone, int sub, int k);
    /// <summary>The sequence an item's slot plays now. Null: it cannot be read.</summary>
    Guid? Slot(string item, int index);
    /// <summary>Runs <paramref name="next"/> at the mod's next ready pass (once its clones are in the world).</summary>
    void Later(ModBlocks mod, Action<Action<string>> next);
}

internal static class MoveApplications
{
    private sealed class Entry(ModBlocks mod, ulong owner, MovePlan plan, JsonObject json)
    {
        public readonly ModBlocks Mod = mod;
        public readonly ulong Owner = owner;
        public readonly MovePlan Plan = plan;
        public readonly JsonObject Json = json;
        public volatile MoveApplicationResult Current = Pending;
        public ulong Requested;
        /// <summary>The content generation of the last attempt; null before the first.</summary>
        public ulong? Attempted;
        /// <summary>Plan again at the next ready pass whatever the content generation (a replay found the applied generation gone).</summary>
        public bool Retry;
        public RouteGeneration? Active;
        // Include staged and replaced generations: a refused attempt may already have written inactive clone rules.
        public readonly JsonObject Resets = new() { ["format"] = 1 };
        /// <summary>The item's slots any generation of this weapon ever switched (what removal reads back).</summary>
        public readonly HashSet<int> SlotIndexes = new();
        /// <summary>The transaction this weapon has open (only inside Write).</summary>
        public IMoveTransaction? Txn;
        public bool Removed;
    }

    private static readonly MoveApplicationResult Pending = new() { State = MoveApplicationState.PendingContent, Reason = "PendingContent" };
    private static readonly ConcurrentDictionary<(string Mod, string Block), Entry> Entries = new();
    // the plans a weapon has staged clones for in this process: it outlives a reload, as the clones do
    private static readonly ConcurrentDictionary<(string Mod, string Block), HashSet<string>> Staged = new();

    // every generation clone staged in this process and the sequence it was made from: once a generation is applied the item's slots name its
    // clones, and the next plan must still start from what the class has there, not from the clone (a clone of a clone, and another plan each time)
    private static readonly ConcurrentDictionary<Guid, Guid> CloneBases = new();

    /// <summary>The item's slots as the class has them: a slot that plays a generation clone of Kiln's reads as the sequence the clone was made from.</summary>
    private static IReadOnlyList<SlotRow>? ClassSlots(IReadOnlyList<SlotRow>? rows) =>
        rows?.Select(r => CloneBases.TryGetValue(r.Sequence, out var from) ? r with { Sequence = from } : r).ToList();

    /// <summary>The running game (tests set it).</summary>
    internal static IMoveHost Host = MoveRuntime.Host;
    /// <summary>Where the events go (tests set it): the tick pump delivers them.</summary>
    internal static Action<KilnEvent> Sink = ev => Events.Enqueue(ev);
    /// <summary>For the lab and tests: the next clone definitions that are refused as if kiln.gamedata had (a count; 0: none).</summary>
    internal static int RefuseDefines;
    /// <summary>For the lab and tests: the next documents a routed weapon writes are applied and then reported refused, as if kiln.content had
    /// refused a rule in them (a count; 0: none). The transaction then takes back what they wrote.</summary>
    internal static int FailWrites;

    /// <summary>For tests: forget every weapon.</summary>
    internal static void Reset()
    {
        Entries.Clear();
        Staged.Clear();
        CloneBases.Clear();
        RefuseDefines = 0;
        FailWrites = 0;
    }

    /// <summary>A routed weapon was defined: its first attempt runs when content is ready.</summary>
    public static void Start(ModBlocks mod, MovePlan plan, JsonObject json)
    {
        var key = (plan.Mod, plan.Name);
        if (Entries.TryGetValue(key, out var old)) Remove(old);
        var entry = new Entry(mod, mod.Owner, plan, json);
        Entries[key] = entry;
        // a mod's code: its unload takes the weapon's layers off, and the status says so. A data-only mod's blocks go with ModBlocks.Reset (Forget).
        if (ModContext.CurrentMod is { } context && context.Info.Id == plan.Mod)
        {
            context.Retiring += () => Remove(entry);
            if (!context.Alive) Remove(entry);
        }
        Host.Later(mod, problem => Run(entry, problem));
    }

    /// <summary>A ready pass of the mod (after every content application, and whenever the mod has ready work): a weapon whose last attempt was refused
    /// on other content than the game has now is tried again, since what was missing may be there. On the same content nothing is tried twice.</summary>
    public static void Ready(ModBlocks mod, Action<string> problem)
    {
        if (Entries.IsEmpty) return;
        var content = Host.ContentGeneration;
        foreach (var e in Entries.Values.Where(e => ReferenceEquals(e.Mod, mod)).ToList())
        {
            bool again;
            lock (e)
            {
                again = !e.Removed && e.Current.State == MoveApplicationState.Refused && (e.Retry || (e.Attempted is { } at && at != content));
                e.Retry = false;
            }
            if (again) Run(e, problem);
        }
    }

    /// <summary>For the lab and tests: tries a refused weapon again now, on the same content.</summary>
    internal static bool Retry(string mod, string block, Action<string> problem)
    {
        if (!Entries.TryGetValue((mod, block), out var e)) return false;
        Run(e, problem);
        return true;
    }

    /// <summary>The mod reloads or is gone: its weapons' layers went with its owner.</summary>
    public static void Forget(string mod)
    {
        foreach (var e in Entries.Values.Where(e => e.Plan.Mod == mod).ToList()) Remove(e);
    }

    /// <summary>The snapshot for a weapon, or null: it has no follow-ups (or is not defined).</summary>
    public static MoveApplicationResult? Status(string mod, string block) => Entries.TryGetValue((mod, block), out var e) ? e.Current : null;

    // ---- removal ----

    /// <summary>The weapon goes: every field any of its generations wrote is reset as its owner's, in one transaction, and the item's slots are read
    /// back (none may play a clone of Kiln's). When that does not hold, every layer of the mod's owner is taken off (fail closed): the weapon's
    /// layers cannot be told apart from what is left, so none stays.</summary>
    private static void Remove(Entry e)
    {
        lock (e)
        {
            if (e.Removed) return;
            e.Removed = true;
            e.Retry = false;
            // removed while its own transaction is open (a handler inside an application): what that transaction wrote goes first
            if (e.Txn is { } open)
            {
                e.Txn = null;
                try { open.Rollback(); }
                catch (Exception x) when (Recoverable(x)) { }
            }
            string? failed = null;
            if (e.Resets.Count > 1)
            {
                IMoveTransaction? txn = null;
                try
                {
                    txn = Host.Begin(e.Owner);
                    var errors = txn.Apply(e.Resets.DeepClone().AsObject(), $"remove moves of {e.Plan.Mod}:{e.Plan.Name}");
                    if (errors.Count > 0) failed = errors[0];
                    else
                        foreach (var index in e.SlotIndexes)
                        {
                            var read = Host.Slot(e.Plan.Item, index);
                            if (read == null) failed = $"slot {index} could not be read";
                            else if (CloneBases.ContainsKey(read.Value)) failed = $"slot {index} still plays a clone of the weapon";
                            if (failed != null) break;
                        }
                    if (failed == null && !txn.Commit()) failed = "the mod's layers were taken off meanwhile";
                    txn = null;
                }
                catch (Exception x) when (Recoverable(x)) { failed = x.Message; }
                if (failed != null)
                {
                    // fail closed: what of the weapon is left cannot be told, so every layer of its owner goes
                    try
                    {
                        if (txn != null) txn.Rollback();
                        Host.Revoke(e.Owner);
                    }
                    catch (Exception x) when (Recoverable(x))
                    {
                        Refuse(e, e.Requested, e.Current.ContentGeneration, $"block {e.Plan.Block}, moves: removing the generation's layers failed: {failed}; and taking the mod's layers off failed: {x.Message}", _ => { });
                        return;
                    }
                }
            }
            e.Active = null;
            lock (e.Json) e.Json.Remove("movesReady");
            e.Mod.ReportDirty = true;
            var now = e.Current;
            Publish(e, new MoveApplicationResult
            {
                State = MoveApplicationState.Removed,
                Reason = failed == null ? "Removed" : $"Removed: block {e.Plan.Block}, moves: removing the generation's layers failed ({failed}): every layer of the mod was taken off instead",
                RequestedGeneration = now.RequestedGeneration, AppliedGeneration = now.AppliedGeneration, ContentGeneration = now.ContentGeneration,
            });
            if (failed != null) Revoked(e, $"removing {e.Plan.Block}'s moves failed and every layer of the mod was taken off", _ => { });
        }
    }

    private static bool Recoverable(Exception x) => MoveRuntime.Readable(x) || x is KilnUnavailableException;

    private static void Publish(Entry e, MoveApplicationResult after)
    {
        var before = e.Current;
        e.Current = after;
        if (before.State == after.State && before.Reason == after.Reason && before.AppliedGeneration == after.AppliedGeneration) return;
        BlocksBridge.Log(after.State == MoveApplicationState.Refused ? LogLevel.Error : LogLevel.Info,
            $"blocks: {e.Plan.Mod}: {e.Plan.Block}: moves {after.State} (attempt {after.RequestedGeneration}, applied {after.AppliedGeneration}){(after.Reason.Length > 0 && !after.Ok ? ": " + after.Reason : "")}");
        try { Sink(new WeaponMovesApplicationChangedEvent(e.Plan.Mod, e.Plan.Name, before, after)); }
        catch (Exception x) when (x is not OutOfMemoryException) { BlocksBridge.Log(LogLevel.Error, $"blocks: posting WeaponMovesApplicationChangedEvent failed: {x.Message}"); }
    }

    private static void Refuse(Entry e, ulong request, ulong content, string reason, Action<string> problem)
    {
        problem(reason);
        // the last applied generation keeps playing: its number and its clones stay in the answer
        Publish(e, e.Current with { Ok = false, State = MoveApplicationState.Refused, Reason = reason, RequestedGeneration = request, ContentGeneration = content });
    }

    private static JsonObject Document(string section, IEnumerable<(string Entry, JsonObject Rules)> entries)
    {
        var sec = new JsonObject();
        foreach (var (entry, rules) in entries)
        {
            if (sec[entry] is JsonObject have)
                foreach (var (k, v) in rules) have[k] = v?.DeepClone();
            else sec[entry] = rules.DeepClone();
        }
        return new JsonObject { ["format"] = 1, [section] = sec };
    }

    private static void Track(Entry e, string section, IEnumerable<(string Entry, JsonObject Rules)> entries)
    {
        var sec = (e.Resets[section] ??= new JsonObject()).AsObject();
        foreach (var (entry, rules) in entries)
        {
            var fields = (sec[entry] ??= new JsonObject()).AsObject();
            foreach (var (path, _) in rules) fields[path] = new JsonObject { ["reset"] = true };
        }
    }

    /// <summary>What the item's slots play now, for the slots a generation switches: index -> the sequence read (absent: it could not be read).</summary>
    private static Dictionary<int, Guid> ReadSlots(RouteGeneration gen)
    {
        var now = new Dictionary<int, Guid>();
        foreach (var index in gen.SlotGuids.Keys)
        {
            Guid? g;
            try { g = Host.Slot(gen.Item, index); }
            catch (Exception x) when (MoveRuntime.Readable(x)) { g = null; }
            if (g is { } read) now[index] = read;
        }
        return now;
    }

    /// <summary>Whether the slots a generation switches are what the weapon's state says: the applied generation's clones, or with none applied, no
    /// clone of any generation (the class's own sequences).</summary>
    private static bool SlotsAsBefore(Entry e, RouteGeneration gen, Dictionary<int, Guid> now)
    {
        foreach (var index in gen.SlotGuids.Keys)
        {
            if (!now.TryGetValue(index, out var read)) return false;
            if (e.Active is { } active ? !(active.SlotGuids.TryGetValue(index, out var was) && was == read) : CloneBases.ContainsKey(read)) return false;
        }
        return true;
    }

    /// <summary>Whether a generation plays: every slot it switches and every link it sets read back as written. Null: it does; else what does not.</summary>
    private static string? Holds(RouteGeneration gen)
    {
        var now = ReadSlots(gen);
        if (!gen.SlotGuids.All(s => now.TryGetValue(s.Key, out var g) && g == s.Value)) return "its slots do not play the applied generation";
        foreach (var ((clone, sub, k), want) in gen.Links)
        {
            Guid? read;
            try { read = Host.Link(clone, sub, k); }
            catch (Exception x) when (MoveRuntime.Readable(x)) { read = null; }
            if (read != want) return $"the follow-up of {clone}{(sub > 0 ? "#" + sub : "")}, window {k}, does not read back";
        }
        return null;
    }

    /// <summary>No generation plays any more (or what plays cannot be told): the status says what the slots play, nothing is written again at a later
    /// content application, and the next attempt plans from the class's sequences.</summary>
    private static void Lost(Entry e, RouteGeneration gen, ulong request, ulong content, string reason, Action<string> problem)
    {
        var now = ReadSlots(gen);
        e.Active = null;
        lock (e.Json) e.Json.Remove("movesReady");
        e.Mod.ReportDirty = true;
        var news = gen.SlotGuids.Count(s => now.TryGetValue(s.Key, out var g) && g == s.Value);
        var clones = now.Count(s => CloneBases.ContainsKey(s.Value));
        var text = $"block {e.Plan.Block}, moves: no generation is applied: {reason} The item's {gen.SlotGuids.Count} routed slots: {news} play these moves, "
            + $"{clones - news} another clone of the weapon, {gen.SlotGuids.Count - now.Count} could not be read, the rest the class's sequences.";
        problem(text);
        Publish(e, new MoveApplicationResult
        {
            State = MoveApplicationState.Refused, Reason = text, RequestedGeneration = request, AppliedGeneration = 0, ContentGeneration = content,
            LiveClones = now.ToFrozenDictionary(s => $"slot/{s.Key}", s => s.Value, StringComparer.Ordinal),
        });
    }

    /// <summary>Every layer of <paramref name="e"/>'s owner was taken off: the owner's other weapons lost theirs too. They say so and are planned again at
    /// the next ready pass (they did nothing wrong); the mod's other rules are applied again at that pass.</summary>
    private static void Revoked(Entry e, string why, Action<string> problem)
    {
        e.Mod.RulesDirty = true;
        foreach (var other in Entries.Values.Where(o => !ReferenceEquals(o, e) && o.Owner == e.Owner).ToList())
            lock (other)
            {
                if (other.Removed || other.Active is not { } gen) continue;
                RetrySoon(other);
                Lost(other, gen, other.Current.RequestedGeneration, Host.ContentGeneration, $"every layer of the mod was taken off ({why}).", problem);
            }
    }

    /// <summary>Plans the weapon again at the mod's next ready pass (one is asked for), on the same content.</summary>
    private static void RetrySoon(Entry e)
    {
        e.Retry = true;
        Host.Later(e.Mod, problem =>
        {
            bool again;
            lock (e)
            {
                again = e.Retry && !e.Removed && e.Current.State == MoveApplicationState.Refused;
                e.Retry = false;
            }
            if (again) Run(e, problem);
        });
    }

    // ---- the write ----

    private enum Wrote { Committed, Failed, Aborted }

    /// <summary>Writes a generation in one content transaction of the weapon's owner and reads it back: the clones' rules, every link, the item's slots,
    /// every slot. Committed: it all held. Failed: <paramref name="why"/> says what did not, and the transaction was rolled back (<paramref name="back"/>;
    /// null: it never opened, nothing was written). Aborted: the weapon was removed meanwhile (its removal took care of the layers).</summary>
    private static Wrote Write(Entry e, RouteGeneration gen, string source, out string why, out (MoveRestore Outcome, string? Error)? back)
    {
        why = "";
        back = null;
        IMoveTransaction txn;
        try { txn = Host.Begin(e.Owner); }
        catch (Exception x) when (Recoverable(x))
        {
            why = $"the content transaction could not be opened: {x.Message}";
            return Wrote.Failed;
        }
        e.Txn = txn;
        string? wrong = null;
        try
        {
            IReadOnlyList<string> Apply(JsonObject doc)
            {
                var errors = txn.Apply(doc, source);
                if (errors.Count == 0 && FailWrites > 0)
                {
                    FailWrites--;
                    return ["refused by the test switch (after the document was applied)"];
                }
                return errors;
            }
            if (gen.SequenceRules.Count > 0 && Apply(Document("sequences", gen.SequenceRules)) is { Count: > 0 } ruleErrors)
                wrong = $"a rule of the new moves was refused: {ruleErrors[0]}";
            if (e.Txn != txn) return Wrote.Aborted;
            if (wrong == null)
                foreach (var ((clone, sub, k), want) in gen.Links)
                {
                    if (Host.Link(clone, sub, k) == want) continue;
                    wrong = $"the follow-up of {clone}{(sub > 0 ? "#" + sub : "")}, window {k}, did not read back as set";
                    break;
                }
            if (wrong == null && Apply(Document("itemdata", [(gen.Item, gen.SlotRules)])) is { Count: > 0 } slotErrors)
                wrong = $"the item's slots were not switched: {slotErrors[0]}";
            if (e.Txn != txn) return Wrote.Aborted;
            if (wrong == null)
                foreach (var (index, want) in gen.SlotGuids)
                {
                    if (Host.Slot(gen.Item, index) == want) continue;
                    wrong = $"the item's slots were not switched: slot {index} did not read back as set";
                    break;
                }
            if (wrong == null)
            {
                e.Txn = null;   // a handler of the commit's change event may remove the weapon: the transaction is no longer its to take back
                if (txn.Commit()) return e.Removed ? Wrote.Aborted : Wrote.Committed;
                why = "the mod's layers were taken off while the generation was written";
                back = (MoveRestore.Cancelled, null);
                return Wrote.Failed;
            }
        }
        catch (Exception x) when (Recoverable(x))
        {
            if (e.Txn != txn) return Wrote.Aborted;
            wrong = $"applying the generation failed: {x.Message}";
        }
        e.Txn = null;
        why = wrong!;
        try { back = txn.Rollback(); }
        catch (Exception x) when (Recoverable(x))
        {
            // a rollback that cannot say what it did: fail closed
            try { Host.Revoke(e.Owner); back = (MoveRestore.Revoked, $"the rollback failed: {x.Message}"); }
            catch (Exception y) when (Recoverable(y)) { back = (MoveRestore.Revoked, $"the rollback failed: {x.Message}; and taking the mod's layers off failed: {y.Message}"); }
        }
        return Wrote.Failed;
    }

    /// <summary>After a failed write of a new generation: the previous state, read back, keeps the applied generation in the status; anything else is
    /// said as it is.</summary>
    private static void Failed(Entry e, RouteGeneration gen, ulong request, ulong content, string reason, (MoveRestore Outcome, string? Error)? back, Action<string> problem)
    {
        switch (back?.Outcome)
        {
            case null:
                Refuse(e, request, content, reason, problem);
                return;
            case MoveRestore.Restored:
                if (SlotsAsBefore(e, gen, ReadSlots(gen))) Refuse(e, request, content, reason + " (what it wrote was taken back and read back)", problem);
                else if (e.Active is { } active) Lost(e, active, request, content, reason + " What it wrote was taken back, but the slots do not play what the status said.", problem);
                else Lost(e, gen, request, content, reason + " What it wrote was taken back, but the slots do not play the class's sequences.", problem);
                return;
            default:
                var what = back.Value.Outcome == MoveRestore.Revoked
                    ? $"taking it back could not be verified ({back.Value.Error}): every layer of the mod was taken off."
                    : "the mod's layers were taken off meanwhile.";
                Lost(e, e.Active ?? gen, request, content, $"{reason}; {what}", problem);
                if (back.Value.Outcome == MoveRestore.Revoked) Revoked(e, $"{e.Plan.Block}'s moves could not be taken back", problem);
                return;
        }
    }

    /// <summary>After the mod's rules were applied again (every content application, and every pass that applies the mod's rules): each applied
    /// generation is written again through a content transaction and read back. One that does not hold is a refusal in its status, with what the
    /// slots play, and the next ready pass plans it again (unless every layer of the mod had to be taken off).</summary>
    public static void Replay(ModBlocks mod, Action<string> problem)
    {
        if (Entries.IsEmpty) return;
        foreach (var e in Entries.Values.Where(e => ReferenceEquals(e.Mod, mod)).ToList())
            lock (e)
            {
                if (e.Removed || e.Active is not { } gen) continue;
                var request = e.Current.RequestedGeneration;
                var wrote = Write(e, gen, $"moves of {e.Plan.Mod}:{e.Plan.Name} (again)", out var why, out var back);
                if (wrote != Wrote.Failed) continue;
                var content = Host.ContentGeneration;
                var reason = $"After the game's content was applied again the weapon's rules did not hold: {why}.";
                e.Attempted = content;     // a failure that is not retried at once is tried again on other content
                switch (back?.Outcome)
                {
                    case MoveRestore.Revoked or MoveRestore.Cancelled:
                        Lost(e, gen, request, content, back.Value.Outcome == MoveRestore.Revoked
                            ? $"{reason} Taking it back could not be verified ({back.Value.Error}): every layer of the mod was taken off."
                            : $"{reason} The mod's layers were taken off meanwhile.", problem);
                        if (back.Value.Outcome == MoveRestore.Revoked) Revoked(e, $"{e.Plan.Block}'s moves could not be taken back", problem);
                        break;
                    default:
                        // taken back (or never written): what plays is what played before the replay. A generation that still holds keeps its
                        // number in the refusal; one that does not is gone
                        RetrySoon(e);
                        if (Holds(gen) is { } gone) Lost(e, gen, request, content, $"{reason} {char.ToUpperInvariant(gone[0])}{gone[1..]}.", problem);
                        else Refuse(e, request, content, $"block {e.Plan.Block}, moves: {reason} The generation still plays (what the replay wrote was taken back).", problem);
                        break;
                }
            }
    }

    // ---- an attempt ----

    // Step 1 and 2: plan on the game's rows, then stage the clones.
    private static void Run(Entry e, Action<string> problem)
    {
        lock (e)
        {
            if (e.Removed) return;
            var plan = e.Plan;
            var content = Host.ContentGeneration;
            e.Attempted = content;
            RouteGeneration? gen;
            List<string> problems;
            try { (gen, problems) = MoveRoutes.Plan(plan, ClassSlots(Host.Slots(plan.Item)), Host.Reader(), Host.Lookup); }
            catch (Exception x) when (MoveRuntime.Readable(x))
            {
                (gen, problems) = (null, [$"block {plan.Block}, moves: the game's data could not be read: {x.Message}"]);
            }
            if (gen == null)
            {
                foreach (var p in problems.Skip(1)) problem(p);
                Refuse(e, ++e.Requested, content, problems.Count > 1 ? $"{problems[0]} (and {problems.Count - 1} more, in the log)" : problems[0], problem);
                return;
            }
            if (e.Active?.Hash == gen.Hash)
            {
                // the same moves on the same data as what plays: nothing to stage, nothing to switch, no event (unless a refused attempt stood in between)
                if (e.Current.State != MoveApplicationState.Applied)
                    Publish(e, e.Current with { Ok = true, State = MoveApplicationState.Applied, Reason = "", RequestedGeneration = ++e.Requested, ContentGeneration = content });
                return;
            }
            var request = ++e.Requested;
            var staged = Staged.GetOrAdd((plan.Mod, plan.Name), _ => new HashSet<string>(StringComparer.Ordinal));
            lock (staged)
            {
                if (!staged.Contains(gen.Hash) && staged.Count >= MoveRoutes.MaxHashes)
                {
                    Refuse(e, request, content, $"RestartRequired: block {plan.Block}, moves: this weapon has had {MoveRoutes.MaxHashes} different sets of moves in this process, and a clone stays "
                        + "in the game until it ends: restart to apply another", problem);
                    return;
                }
                staged.Add(gen.Hash);
            }
            var have = Host.Clones();
            foreach (var def in gen.Clones)
            {
                CloneBases[Guid.Parse((string)def["guid"]!)] = Guid.Parse((string)def["base"]!);
                if (have.ContainsKey(Guid.Parse((string)def["guid"]!))) continue;     // staged by an earlier attempt with this plan
                try
                {
                    if (RefuseDefines > 0)
                    {
                        RefuseDefines--;
                        throw new InvalidOperationException("refused by the test switch");
                    }
                    Host.Define(def.DeepClone().AsObject(), e.Owner);
                }
                catch (Exception x) when (x is ArgumentException or NotSupportedException or InvalidOperationException or KeyNotFoundException)
                {
                    Refuse(e, request, content, $"block {plan.Block}, moves: the clone {def["name"]} of {def["base"]} was refused: {x.Message}", problem);
                    return;
                }
            }
            Host.Later(e.Mod, next => Finish(e, request, content, gen, next));
        }
    }

    // Steps 3 to 5, one ready pass later: the clones are in the world (BlockRuntime waits for that before it runs a mod's ready work).
    private static void Finish(Entry e, ulong request, ulong content, RouteGeneration gen, Action<string> problem)
    {
        lock (e)
        {
            if (e.Removed || e.Requested != request) return;      // stale: the weapon went, or a newer attempt owns it
            var plan = e.Plan;
            try
            {
                if (Host.ContentGeneration != content)
                {
                    // content was applied again since the rows were read: read them again
                    Run(e, problem);
                    return;
                }
                var have = Host.Clones();
                foreach (var def in gen.Clones)
                {
                    var st = have.TryGetValue(Guid.Parse((string)def["guid"]!), out var s) ? s : ("missing", null);
                    if (st.Item1 == "registered") continue;
                    Refuse(e, request, content, $"block {plan.Block}, moves: the clone {def["name"]} is {st.Item1}{(st.Item2 is { Length: > 0 } err ? ": " + err : "")}", problem);
                    return;
                }
            }
            catch (Exception x) when (MoveRuntime.Readable(x))
            {
                Refuse(e, request, content, $"block {plan.Block}, moves: applying the generation failed: {x.Message}", problem);
                return;
            }
            // what any generation of this weapon wrote, before it is written: removal resets it as the owner's
            Track(e, "sequences", gen.SequenceRules);
            Track(e, "itemdata", [(gen.Item, gen.SlotRules)]);
            e.SlotIndexes.UnionWith(gen.SlotGuids.Keys);
            switch (Write(e, gen, $"moves of {plan.Mod}:{plan.Name}", out var why, out var back))
            {
                case Wrote.Aborted:
                    return;
                case Wrote.Failed:
                    Failed(e, gen, request, content, $"block {plan.Block}, moves: {why}", back, problem);
                    return;
            }
            // committed: this generation plays, whatever attempt asked since
            e.Active = gen;
            lock (e.Json)
                e.Json["movesReady"] = new JsonObject
                {
                    ["generation"] = request, ["plan"] = gen.Hash,
                    ["clones"] = new JsonObject(gen.LiveClones.Select(c => KeyValuePair.Create(c.Key, (JsonNode?)c.Value.ToString()))),
                    ["rules"] = new JsonObject(gen.SequenceRules.Select(r => ($"sequences {r.Entry}", r.Rules)).Append(($"itemdata {gen.Item}", gen.SlotRules))
                        .Select(r => KeyValuePair.Create(r.Item1, (JsonNode?)r.Item2.DeepClone()))),
                };
            e.Mod.ReportDirty = true;
            Publish(e, new MoveApplicationResult
            {
                Ok = true, State = MoveApplicationState.Applied, RequestedGeneration = request, AppliedGeneration = request, ContentGeneration = content,
                LiveClones = gen.LiveClones.ToFrozenDictionary(StringComparer.Ordinal),
            });
        }
    }
}
