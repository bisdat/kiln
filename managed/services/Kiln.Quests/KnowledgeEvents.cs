// SPDX-License-Identifier: MIT
// Kiln.Quests on the event bus: KnowledgeChangedEvent, DialogueLineEvent, DialogueKnowledgeEvent, KnowledgeRequestEvent
// (docs/kiln/events.md, docs/kiln/services/knowledge.md "Events"). All posted on the tick thread from kiln.knowledge's
// event ring, one step after the game's step that produced them; built only while someone subscribes.
namespace Kiln;

/// <summary>A knowledge value changed in the world map or a player's map: the game (a quest step, a dialogue line, a
/// rescue, loot, a query trigger, ...) or a Kiln change (<see cref="Request"/>). Seen in the change lists of the game's
/// knowledge update, after the change was applied and replicated. Initial loads (the world save, a player joining) are
/// not events. Posted on the tick thread in the step after the change. Not cancelable (it already happened: revoke
/// it to undo).</summary>
public sealed class KnowledgeChangedEvent : KilnEvent
{
    static KnowledgeChangedEvent() => KnowledgeEventPump.Start();

    internal KnowledgeChangedEvent(EntityId target, uint id, uint oldValue, uint newValue, KnowledgeType type, ulong request, ulong step)
    {
        Target = target;
        Id = id;
        OldValue = oldValue;
        NewValue = newValue;
        Type = type;
        Request = request;
        Step = step;
    }

    /// <summary>0 = the world map, else the player whose map changed.</summary>
    public EntityId Target { get; }
    /// <summary>True when the world's map changed.</summary>
    public bool IsWorld => Target.IsNone;
    /// <summary>The player (null for the world, or if they left in the meantime).</summary>
    public Player? Player => Target.IsNone || !Players.Available ? null : Players.Find(Target);
    /// <summary>The knowledge id that changed.</summary>
    public uint Id { get; }
    /// <summary>The catalog name ("npc:….unlock", "quest:0x…"), or null.</summary>
    public string? Name => Knowledge.NameOf(Id);
    /// <summary>The value before the change.</summary>
    public uint OldValue { get; }
    /// <summary>The value after the change.</summary>
    public uint NewValue { get; }
    /// <summary>The knowledge object's type.</summary>
    public KnowledgeType Type { get; }
    /// <summary>The Kiln request that caused it (<see cref="KnowledgeResult.Request"/>), 0 = the game.</summary>
    public ulong Request { get; }
    /// <summary>True when a Kiln request caused the change (<see cref="Request"/> is not 0).</summary>
    public bool ByKiln => Request != 0;
    /// <summary>True when the value went from 0 to a non-zero value.</summary>
    public bool Gained => OldValue == 0 && NewValue != 0;
    /// <summary>True when the value went from non-zero to 0 (a revoke).</summary>
    public bool Lost => OldValue != 0 && NewValue == 0;
    /// <summary>The server-world step in which the game applied the change.</summary>
    public ulong Step { get; }

    /// <summary>Which map changed, the id (and its name), the values, and the Kiln request if there was one.</summary>
    public override string ToString() =>
        $"{(IsWorld ? "world" : $"player {Target}")} 0x{Id:x8}{(Name is { } n ? $" ({n})" : "")}: {OldValue} -> {NewValue}{(ByKiln ? $" (Kiln request {Request})" : "")}";
}

/// <summary>What a player did with a dialogue line.</summary>
public enum DialogueAction : byte
{
    /// <summary>The player picked a line (or the dialogue opened: the client sends its greeting line).</summary>
    Start = 0,
    /// <summary>The line stopped (the player left the line / the dialogue).</summary>
    Stop = 1,
}

/// <summary>A player picked (or stopped) a dialogue line of an NPC: the game's ServerSelectDialogOptionEvent, read by
/// Kiln's program kiln_knowledge. <see cref="LineId"/> is the line's DialogOptionId (also its "heard" knowledge id), 0 =
/// farewell (the server picks a random farewell line), 1 = an option whose requirement the client found missing (the NPC
/// plays its refusal). Posted on the tick thread one step after the choice. Not cancelable: to keep a line's knowledge
/// from running, handle <see cref="DialogueKnowledgeEvent"/>.</summary>
public sealed class DialogueLineEvent : KilnEvent
{
    static DialogueLineEvent() => KnowledgeEventPump.Start();

    internal DialogueLineEvent(EntityId player, EntityId npc, uint line, DialogueAction action, ulong step)
    {
        PlayerEntity = player;
        Npc = npc;
        LineId = line;
        Action = action;
        Step = step;
    }

    /// <summary>The player's entity.</summary>
    public EntityId PlayerEntity { get; }
    /// <summary>The player; null when they left in the meantime.</summary>
    public Player? Player => Players.Available ? Players.Find(PlayerEntity) : null;
    /// <summary>The NPC's entity.</summary>
    public EntityId Npc { get; }
    /// <summary>The line's dialogue option id (also its "heard" knowledge id); 0 for a farewell, 1 for an option whose
    /// requirement the client found missing.</summary>
    public uint LineId { get; }
    /// <summary>Whether the player picked the line or stopped it.</summary>
    public DialogueAction Action { get; }
    /// <summary>True when <see cref="LineId"/> is 0: a farewell (the server picks a random farewell line).</summary>
    public bool IsFarewell => LineId == 0;
    /// <summary>True when <see cref="LineId"/> is 1: the client found the option's requirement missing, and the NPC plays
    /// its refusal.</summary>
    public bool IsMissingRequirement => LineId == 1;
    /// <summary>The server-world step of the choice.</summary>
    public ulong Step { get; }

    /// <summary>The player, the action, the line and the NPC.</summary>
    public override string ToString() => $"player {PlayerEntity} {Action} line 0x{LineId:x8} of NPC {Npc}";
}

/// <summary>One knowledge operation (the game's KnowledgeUnlockOperation) and the player it is for.</summary>
/// <param name="Id">The knowledge id.</param>
/// <param name="Type">The knowledge object's type.</param>
/// <param name="Value">The operation's value, as the game stores it; what it means follows <paramref name="Type"/>.</param>
/// <param name="Player">The player entity the operation is for.</param>
public readonly record struct KnowledgeOperation(uint Id, KnowledgeType Type, uint Value, EntityId Player)
{
    /// <summary>The id, the type and the value.</summary>
    public override string ToString() => $"0x{Id:x8} {Type} {Value}";
}

/// <summary>The knowledge a dialogue line is about to run for a player (its knowledgeUnlockOperations and its "heard"
/// knowledge), held by Kiln for a decision. Fires only while someone subscribes to it (that makes Kiln hold every line's
/// knowledge for up to 8 steps; normally one) or when the server setting checkDialogue is on. Posted on the tick
/// thread. Canceling (or <see cref="CancelableResultEvent.Deny"/>) drops the knowledge (the NPC still answers: its
/// animation and texts are the client's); <see cref="CancelableResultEvent.Allow"/> runs it even if a check failed;
/// Default runs it unless checkDialogue is on and a check failed (<see cref="DefaultApplies"/>).</summary>
public sealed class DialogueKnowledgeEvent : CancelableResultEvent
{
    static DialogueKnowledgeEvent() => KnowledgeEventPump.Start();

    internal DialogueKnowledgeEvent(ulong held, EntityId player, EntityId npc, uint line, IReadOnlyList<KnowledgeOperation> ops,
        uint checks, float distance, bool checksOn, ulong step)
    {
        Held = held;
        PlayerEntity = player;
        Npc = npc;
        LineId = line;
        Operations = ops;
        RequirementFailed = (checks & 1) != 0;
        TooFar = (checks & 2) != 0;
        CheckError = (checks & 4) != 0;
        Distance = distance >= 0 ? distance : null;
        ChecksOn = checksOn;
        Step = step;
    }

    internal ulong Held { get; }
    /// <summary>The player's entity.</summary>
    public EntityId PlayerEntity { get; }
    /// <summary>The player; null when they left in the meantime.</summary>
    public Player? Player => Players.Available ? Players.Find(PlayerEntity) : null;
    /// <summary>The NPC's entity.</summary>
    public EntityId Npc { get; }
    /// <summary>The dialogue line's option id.</summary>
    public uint LineId { get; }
    /// <summary>What would run: the line's operations, then {line id, Bool, 1} ("heard").</summary>
    public IReadOnlyList<KnowledgeOperation> Operations { get; }
    /// <summary>The line's requirement is false for this player on the server (a vanilla client never offers such a line).</summary>
    public bool RequirementFailed { get; }
    /// <summary>The player stands farther from the NPC than the server's dialogueRange.</summary>
    public bool TooFar { get; }
    /// <summary>A check could not run (counted as passed).</summary>
    public bool CheckError { get; }
    /// <summary>Metres between player and NPC, null when unknown.</summary>
    public double? Distance { get; }
    /// <summary>The server setting checkDialogue is on.</summary>
    public bool ChecksOn { get; }
    /// <summary>True when the knowledge runs if no handler decides: false when the <c>checkDialogue</c> setting is on and
    /// the line's requirement failed or the player is too far.</summary>
    public bool DefaultApplies => !(ChecksOn && (RequirementFailed || TooFar));
    /// <summary>The server-world step of the choice.</summary>
    public ulong Step { get; }

    /// <summary>The player, the line, the NPC, the operations and which checks failed.</summary>
    public override string ToString() =>
        $"player {PlayerEntity} line 0x{LineId:x8} of NPC {Npc}: {Operations.Count} op(s){(RequirementFailed ? ", requirement failed" : "")}{(TooFar ? $", too far ({Distance:0.0} m)" : "")}";
}

/// <summary>A client asked the server for knowledge itself (the player input TriggerKnowledgeAction; the vanilla client
/// does this for some tutorial and UI knowledge). The game would apply any id it is sent. Fires while someone subscribes;
/// with the server setting vetoClientKnowledge on the operation is held (<see cref="Held"/>) and the handlers decide:
/// <see cref="CancelableResultEvent.Allow"/> applies it, Deny / Cancel drops it, Default drops it (the veto) unless the id
/// is allowed (allowed ids are never held). Without the veto the operation already went through: the event only
/// reports it. Posted on the tick thread one step after the request.</summary>
public sealed class KnowledgeRequestEvent : CancelableResultEvent
{
    static KnowledgeRequestEvent() => KnowledgeEventPump.Start();

    internal KnowledgeRequestEvent(ulong held, EntityId player, uint id, KnowledgeType type, uint value, ulong step)
    {
        HeldId = held;
        PlayerEntity = player;
        Id = id;
        Type = type;
        Value = value;
        Step = step;
    }

    internal ulong HeldId { get; }
    /// <summary>The veto holds it: the decision of the handlers counts.</summary>
    public bool Held => HeldId != 0;
    /// <summary>The entity of the player whose client sent the request.</summary>
    public EntityId PlayerEntity { get; }
    /// <summary>The player; null when they left in the meantime.</summary>
    public Player? Player => Players.Available ? Players.Find(PlayerEntity) : null;
    /// <summary>The knowledge id the client asked for.</summary>
    public uint Id { get; }
    /// <summary>The catalog name of the id (<c>npc:…</c>, <c>quest:0x…</c>), or null.</summary>
    public string? Name => Knowledge.NameOf(Id);
    /// <summary>The type of the object the client asked for.</summary>
    public KnowledgeType Type { get; }
    /// <summary>The value the client sent.</summary>
    public uint Value { get; }
    /// <summary>The server-world step of the request.</summary>
    public ulong Step { get; }

    /// <summary>The player, the id, the type and the value, and whether the veto holds it.</summary>
    public override string ToString() => $"player {PlayerEntity} asks for 0x{Id:x8} {Type} {Value}{(Held ? " (held by the veto)" : "")}";
}

/// <summary>Reads kiln.knowledge's event ring on the service tick and posts the events above.</summary>
internal static unsafe class KnowledgeEventPump
{
    private static readonly object Lock = new();
    private static IDisposable? _tick;
    private static ulong _cursor;
    private static uint _listen = uint.MaxValue;
    private static bool _rosterOn;

    public static void Start()
    {
        lock (Lock)
        {
            if (_tick is not null) return;
            var t = KnowledgeNative.T;
            _cursor = t != null ? t->LastSeq() : 0;
            _tick = ServiceTick.Add("kiln.knowledge events", Pump);
        }
    }

    private static void Pump(in TickInfo tick)
    {
        var t = KnowledgeNative.T;
        if (t == null) return;
        KnowledgeSettingsFile.Ensure();
        var changed = Events.HasSubscribers<KnowledgeChangedEvent>();
        var line = Events.HasSubscribers<DialogueLineEvent>();
        var dknow = Events.HasSubscribers<DialogueKnowledgeEvent>();
        var request = Events.HasSubscribers<KnowledgeRequestEvent>();
        var roster = Events.HasSubscribers<RosterUnlockChangedEvent>();
        var rosterTable = roster && !_rosterOn ? KnowledgeNative.Roster(t) : null;
        if (rosterTable != null)
        {
            // the worlds publish roster snapshots (and so their changes) only once something reads the roster
            uint gen = 0, readiness = 0;
            rosterTable->RosterWorld(0, &gen, &readiness);
            rosterTable->RosterWorld(2, &gen, &readiness);
            _rosterOn = true;
        }
        var listen = (request ? 1u : 0u) | (dknow ? 2u : 0u);
        if (listen != _listen)
        {
            t->Listen(listen);
            _listen = listen;
        }
        var last = t->LastSeq();
        if (last <= _cursor) return;
        if (!changed && !line && !dknow && !request && !roster)
        {
            _cursor = last;
            return;
        }
        var checksOn = false;
        if (dknow)
        {
            KnowledgeNative.Settings s = default;
            s.Size = (uint)sizeof(KnowledgeNative.Settings);
            if (t->GetSettings(&s) == KnowledgeNative.Ok) checksOn = s.CheckDialogue != 0;
        }
        var buf = stackalloc KnowledgeNative.Event[64];
        var ops = stackalloc KnowledgeNative.Op[16];
        for (var round = 0; round < 16; round++)
        {
            uint n = 0;
            if (t->Events(_cursor, buf, 64, &n) != KnowledgeNative.Ok || n == 0) break;
            for (var i = 0; i < n; i++)
            {
                var e = buf[i];
                if (e.Seq <= _cursor) continue;
                _cursor = e.Seq;
                switch (e.Kind)
                {
                    case 1 when changed:
                        Events.Post(new KnowledgeChangedEvent(new EntityId(e.Target), e.Id, e.OldValue, e.NewValue, (KnowledgeType)e.Extra,
                            e.Request, e.Step));
                        break;
                    case 6 when roster && e.Npc != 0 && e.Target is 0 or 2:
                        Events.Post(new RosterUnlockChangedEvent(new CapacityWorldToken((CapacityWorldKind)e.Target, e.Npc), e.Id,
                            e.OldValue != 0, e.NewValue != 0, (ushort)e.Extra, e.Request));
                        break;
                    case 2 when line:
                        Events.Post(new DialogueLineEvent(new EntityId(e.Target), new EntityId(e.Npc), e.Id, (DialogueAction)e.OldValue, e.Step));
                        break;
                    case 3 when request:
                    {
                        var ev = Events.Post(new KnowledgeRequestEvent(e.Request, new EntityId(e.Target), e.Id, (KnowledgeType)e.OldValue,
                            e.NewValue, e.Step));
                        if (e.Request != 0) t->Decide(e.Request, Decision(ev.Canceled, ev.Result));
                        break;
                    }
                    case 4 when dknow:
                    {
                        uint k = 0;
                        var list = new List<KnowledgeOperation>();
                        if (t->HeldOps(e.Request, ops, 16, &k) is KnowledgeNative.Ok or KnowledgeNative.BufferTooSmall)
                            for (var j = 0; j < Math.Min(k, 16u); j++)
                                list.Add(new KnowledgeOperation(ops[j].Id, (KnowledgeType)ops[j].Type, ops[j].Value, new EntityId(e.Target)));
                        var ev = Events.Post(new DialogueKnowledgeEvent(e.Request, new EntityId(e.Target), new EntityId(e.Npc), e.Id, list,
                            e.Flags, e.Distance, checksOn, e.Step));
                        t->Decide(e.Request, Decision(ev.Canceled, ev.Result));
                        break;
                    }
                }
            }
            if (n < 64) break;
        }
    }

    private static int Decision(bool canceled, EventResult result) =>
        canceled || result == EventResult.Deny ? 0 : result == EventResult.Allow ? 1 : -1;
}
