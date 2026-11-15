// SPDX-License-Identifier: MIT
// A weapon's follow-up routes, with no game: what is refused as written, how a route resolves against a motion's own windows (the cases of the
// research contract: all against one occurrence, identical routes, disjoint occurrences, different sub-sequences, a window with no follow-up,
// explicit over the chain's own link), and the application as one generation over a made-up game that can refuse any step: nothing is half applied,
// the status and the event say Applied only after the switch, and the same plan twice changes nothing. Every row below is invented.
using System.Text.Json.Nodes;
using Xunit;

namespace Kiln.BlockTests;

[Collection("player-state seam")]
public sealed class MoveFollowUpTests : IDisposable
{
    private const string Mod = "routes-test";
    private readonly Func<string, bool> _held = BlockCapabilities.Held;
    private readonly IMoveHost _host = MoveApplications.Host;
    private readonly Action<KilnEvent> _sink = MoveApplications.Sink;
    private readonly List<WeaponMovesApplicationChangedEvent> _events = new();
    private readonly FakeGame _game = new();

    public MoveFollowUpTests()
    {
        PlayerStateSeam.Lookup = null;
        BlockCapabilities.Held = _ => false;
        MoveApplications.Reset();
        MoveApplications.Host = _game;
        MoveApplications.Sink = ev => _events.Add((WeaponMovesApplicationChangedEvent)ev);
    }

    public void Dispose()
    {
        PlayerStateSeam.Lookup = PlayerAnimLookup.Instance;
        BlockCapabilities.Held = _held;
        MoveApplications.Reset();
        MoveApplications.Host = _host;
        MoveApplications.Sink = _sink;
    }

    private static ModBlocks NewMod() => new(Mod, 7) { ManifestClients = "mod" };
    private static JsonObject Lower(WeaponBlock b, string name = "warden") => Compiler.Lower(NewMod(), name, b).Low.Json;
    private static BlockError Refused(WeaponBlock b) => Assert.Throws<BlockError>(() => Lower(b));

    private static MoveFollowUpBlock Route(MoveFollowUpTrigger trigger, WeaponMoveSlot to, int? occurrence = null, int sub = 0, int link = 1) =>
        new() { Trigger = trigger, To = to, Occurrence = occurrence, SubSequence = sub, Link = link };

    // a sword whose block hands over to its own parry on a parried blow and to its first light attack on Primary
    private static WeaponBlock Warden(params MoveFollowUpBlock[] blockRoutes) => new()
    {
        Name = "Warden", Class = WeaponClass.Sword,
        Moves = new WeaponMoves
        {
            Primary = [new MoveBlock { Like = "Light1", HitAt = 0.3 }, new MoveBlock { Like = "Light2" }],
            Parry = new MoveBlock { Like = "ParryAll" },
            Block = new MoveBlock
            {
                FollowUps = blockRoutes.Length > 0 ? blockRoutes : [Route(MoveFollowUpTrigger.Parried, WeaponMoveSlot.Parry), Route(MoveFollowUpTrigger.Primary, WeaponMoveSlot.Primary)],
            },
        },
    };

    // ---- the made-up game ----

    private static EventRow Ev(string type, double t, double d = 0) => new(type, 0, t, d);
    private static EventRow Window(string trigger, double t, Guid to, double at = 0.4) => Ev("SetActionOportunity", t) with { TriggeredBy = trigger, FollowUp = to, FollowUpTime = at };

    private static readonly Guid L1 = Guid.NewGuid(), L2 = Guid.NewGuid(), HeavyG = Guid.NewGuid(), BlockG = Guid.NewGuid(), ParryG = Guid.NewGuid(), Kick = Guid.NewGuid(), VanillaParry = Guid.NewGuid();

    private static SubSequence Light(Guid self, Guid next) => MoveTimingTests.Sub(1.6,
        Ev("SetAnimationEvent", 0) with { State = Guid.NewGuid() },
        Window("Evade", 0.18, Guid.Empty, 0.5),
        // one input, two rows: a skill decides which the game takes. They must stay two rows
        Window("Primary", 0.2, next, 0.45), Window("Primary", 0.2, next, 0.55),
        Window("Heavy", 0.2, HeavyG, 0.5),
        Ev("SpawnImpact", 0.25, 0.15)) with { Sequence = self };

    private static readonly Dictionary<string, IReadOnlyList<SubSequence>> Game = new()
    {
        ["Light1"] = [Light(L1, L2)], ["Light2"] = [Light(L2, L1)],
        ["ParryAll"] = [MoveTimingTests.Sub(0.8, Ev("SetAnimationEvent", 0) with { State = Guid.NewGuid() }, Window("Primary", 0, Kick, 0.25)) with { Sequence = ParryG }],
        // the block: a parried blow leads to the game's parry, a Primary to the first light attack; its second sub-sequence has a Primary window of its own
        [BlockG.ToString()] =
        [
            MoveTimingTests.Sub(0.4, Ev("SetAnimationEvent", 0) with { State = Guid.NewGuid() }, Window("Parried", 0, VanillaParry, 0), Window("Primary", 0.066, L1, 0.35),
                Ev("LoopEvent", 0.26, 0.13)) with { Sequence = BlockG },
            MoveTimingTests.Sub(0.4, Window("Primary", 0.1, L1, 0.3)) with { Sequence = BlockG, Index = 1 },
        ],
    };

    private static readonly IReadOnlyList<SlotRow> Slots =
    [
        new("Block", [], "Invalid", BlockG), new("Heavy", ["IsAirborne"], "Invalid", L1), new("Heavy", ["IsGrounded"], "Invalid", HeavyG),
        new("Primary", [], "Invalid", L1), new("Parry", [], "Invalid", VanillaParry),
    ];

    private sealed class FakeGame : IMoveHost
    {
        public ulong ContentGeneration { get; set; } = 3;
        public IReadOnlyList<SlotRow>? SlotRows = MoveFollowUpTests.Slots;
        public readonly Dictionary<Guid, (string State, string? Error)> Defined = new();
        public readonly List<string> DefinedNames = new();
        public readonly Dictionary<(string Clone, int Sub, int K), Guid> LinksNow = new();
        public readonly Dictionary<int, Guid> SlotsNow = new();
        public readonly Dictionary<string, Guid> ByName = new();
        public readonly List<JsonObject> Applied = new();
        public readonly Queue<Action<Action<string>>> Pending = new();
        public int RefuseDefineAt = -1, Defines;
        public Func<JsonObject, string?>? ApplyError;       // before the document changes anything
        public Func<JsonObject, string?>? LateError;        // after it changed what it could (a partial write)
        public Action<JsonObject>? AfterApply;
        public Action? AfterCommit;                         // where the game raises ContentChangedEvent
        public bool SwitchTakesOnlyFirst;
        public bool FailRestore;   // a rollback that does not read back: kiln.content takes every layer of the owner off
        public bool CancelOnApply; // the owner's layers are taken off while a transaction is open
        public bool NoTransactions;
        public bool ThrowLinkRead, ThrowSlotRead;
        public int Begins, Commits, Rollbacks, Revokes;
        public FakeTxn? Open;

        /// <summary>A made-up content transaction: the slots and links as they were at its start, put back on rollback.</summary>
        public sealed class FakeTxn(FakeGame game) : IMoveTransaction
        {
            private readonly Dictionary<int, Guid> _slots = new(game.SlotsNow);
            private readonly Dictionary<(string, int, int), Guid> _links = new(game.LinksNow);
            public bool Ended, Cancelled;

            public IReadOnlyList<string> Apply(JsonObject document, string source)
            {
                Assert.False(Ended, "an application in an ended transaction");
                return game.Apply(document, source);
            }

            public bool Commit()
            {
                Assert.False(Ended);
                Ended = true;
                game.Open = null;
                if (Cancelled) return false;
                game.Commits++;
                game.AfterCommit?.Invoke();
                return true;
            }

            public (MoveRestore Outcome, string? Error) Rollback()
            {
                Assert.False(Ended);
                Ended = true;
                game.Open = null;
                game.Rollbacks++;
                if (Cancelled) return (MoveRestore.Cancelled, null);
                if (game.FailRestore)
                {
                    game.Revoke(0);
                    return (MoveRestore.Revoked, "1 of 4 field(s) did not read back after the rollback");
                }
                game.SlotsNow.Clear();
                foreach (var (k, v) in _slots) game.SlotsNow[k] = v;
                game.LinksNow.Clear();
                foreach (var (k, v) in _links) game.LinksNow[k] = v;
                return (MoveRestore.Restored, null);
            }
        }

        public IMoveTransaction Begin(ulong owner)
        {
            if (NoTransactions) throw new KilnUnavailableException("kiln.content", "this kiln.content has no content transactions");
            Assert.Null(Open);      // one open transaction per owner, as kiln.content
            Begins++;
            return Open = new FakeTxn(this);
        }

        // every layer of the owner off: the class's slots, the clones' own links
        public void Revoke(ulong owner)
        {
            Revokes++;
            SlotsNow.Clear();
            LinksNow.Clear();
            if (Open != null) Open.Cancelled = true;
        }

        // as the game: once a slot was switched it names the clone that plays there
        public IReadOnlyList<SlotRow>? Slots(string item) => SlotRows?.Select((r, i) => SlotsNow.TryGetValue(i, out var g) ? r with { Sequence = g } : r).ToList();
        public Func<string, IReadOnlyList<SubSequence>?> Reader() => key => Game.TryGetValue(key, out var s) ? s : null;
        public IPlayerStateLookup? Lookup => null;
        public IReadOnlyDictionary<Guid, (string State, string? Error)> Clones() => Defined;

        public void Define(JsonObject d, ulong owner)
        {
            if (Defines++ == RefuseDefineAt) throw new InvalidOperationException("the registry is full");
            var g = Guid.Parse((string)d["guid"]!);
            Defined[g] = ("registered", null);
            ByName[(string)d["name"]!] = g;
            DefinedNames.Add((string)d["name"]!);
        }

        public IReadOnlyList<string> Apply(JsonObject doc, string source)
        {
            Assert.NotNull(Open);   // every write of a routed weapon is inside a transaction
            Applied.Add(doc.DeepClone().AsObject());
            if (ApplyError?.Invoke(doc) is { } error) return [error];
            if (doc["sequences"] is JsonObject seqs)
                foreach (var (entry, rules) in seqs)
                {
                    var hash = entry.IndexOf('#');
                    var (clone, sub) = hash > 0 ? (entry[..hash], int.Parse(entry[(hash + 1)..])) : (entry, 0);
                    foreach (var (path, v) in rules!.AsObject())
                        if (path.EndsWith(".followUp"))
                        {
                            var key = (clone, sub, int.Parse(path[(path.IndexOf('#') + 1)..path.IndexOf(']')]));
                            if (v is JsonObject) LinksNow.Remove(key);
                            else LinksNow[key] = ByName[(string)v!];
                        }
                }
            if (doc["itemdata"] is JsonObject items)
                foreach (var (_, rules) in items)
                {
                    var n = 0;
                    foreach (var (path, v) in rules!.AsObject())
                    {
                        if (SwitchTakesOnlyFirst && n++ > 0) continue;
                        var i = int.Parse(path[(path.IndexOf('[') + 1)..path.IndexOf(']')]);
                        if (v is JsonObject) SlotsNow.Remove(i);       // {"reset": true}: the class's again
                        else SlotsNow[i] = ByName[(string)v!];
                    }
                }
            if (CancelOnApply)
            {
                CancelOnApply = false;
                Revoke(0);
            }
            AfterApply?.Invoke(doc);
            return LateError?.Invoke(doc) is { } late ? [late] : [];
        }

        public Guid? Link(string clone, int sub, int k) => ThrowLinkRead ? throw new InvalidOperationException("link read unavailable") : LinksNow.TryGetValue((clone, sub, k), out var g) ? g : null;
        public Guid? Slot(string item, int index) => ThrowSlotRead ? throw new InvalidOperationException("slot read unavailable") : SlotsNow.TryGetValue(index, out var g) ? g
            : SlotRows is { } rows && index < rows.Count ? rows[index].Sequence : null;   // not switched: the class's sequence, as the game reads it
        public void Later(ModBlocks mod, Action<Action<string>> next) => Pending.Enqueue(next);

        /// <summary>Ready passes until nothing is waiting.</summary>
        public List<string> Pump()
        {
            var problems = new List<string>();
            while (Pending.Count > 0) Pending.Dequeue()(problems.Add);
            return problems;
        }
    }

    private (ModBlocks Mod, MovePlan Plan) Start(WeaponBlock b, string name = "warden")
    {
        var mod = NewMod();
        var json = Compiler.Lower(mod, name, b).Low.Json;
        Assert.True(Compiler.Plans.TryGetValue(json, out var plan));
        MoveApplications.Start(mod, plan!, json);
        return (mod, plan!);
    }

    private static MoveApplicationResult Status(string name = "warden") => MoveApplications.Status(Mod, name)!;

    // ---- the shape ----

    [Fact]
    public void The_public_slots_are_the_planners_slots_in_the_same_order()
    {
        Assert.Equal(Enum.GetNames<MoveSlot>(), Enum.GetNames<WeaponMoveSlot>());
        foreach (var s in Enum.GetValues<WeaponMoveSlot>()) Assert.Equal(s.ToString(), ((MoveSlot)s).ToString());
    }

    [Fact]
    public void Json_and_csharp_lower_to_the_same_and_the_plan_lists_the_routes()
    {
        const string json = """
            { "name": "Warden", "class": "sword",
              "moves": { "primary": [ { "like": "Light1", "hitAt": 0.3 }, { "like": "Light2" } ], "parry": { "like": "ParryAll" },
                         "block": { "followUps": [ { "trigger": "parried", "to": "parry" }, { "trigger": "primary", "to": "primary" } ] } } }
            """;
        var a = Lower(Warden());
        Assert.Equal(ItemCompileGoldenTests.Text(a), ItemCompileGoldenTests.Text(Lower(BlockJson.Parse<WeaponBlock>(json))));
        var block = a["moves"]!["clones"]!.AsArray().Single(c => (string)c!["slot"]! == "block")!;
        Assert.Equal(["Parried", "Primary"], block["followUps"]!.AsArray().Select(r => (string)r!["trigger"]!));
        Assert.Equal("parry", (string)block["followUps"]![0]!["to"]!);
        // a routed weapon's clones are a set per generation: none is defined at compile, also not those with a like
        Assert.Null(a["sequences"]);
        // lowering alone publishes nothing: the application starts when the block commits (a kind's plan lowers a staged block and may never commit it)
        Assert.Null(MoveApplications.Status(Mod, "warden"));
        Assert.Empty(_game.Pending);
    }

    [Fact]
    public void A_weapon_without_follow_ups_keeps_its_clones_and_has_no_status()
    {
        var plain = Lower(Warden() with { Moves = new WeaponMoves { Primary = new MoveBlock { Like = "Light1" } } });
        Assert.Equal(BlockIds.Derive(Mod, "sequence", "warden.primary").ToString(), (string)plain["sequences"]![0]!["guid"]!);
        Assert.Null(plain["moves"]!["generation"]);
        Assert.Null(plain["moves"]!["clones"]![0]!["followUps"]);
        Assert.Null(MoveApplications.Status(Mod, "warden"));
    }

    [Fact]
    public void A_held_capability_refuses_the_definition_by_name_and_leaves_other_weapons_alone()
    {
        BlockCapabilities.Held = name => name == "blocks.move_followups";
        var e = Assert.Throws<KilnUnavailableException>(() => Lower(Warden()));
        Assert.Contains("blocks.move_followups", e.Message);
        Lower(Warden() with { Moves = new WeaponMoves { Primary = new MoveBlock { Like = "Light1" } } });
    }

    // ---- refused as written ----

    [Fact]
    public void Routes_wrong_as_written_are_refused_with_their_field()
    {
        // the target slot is not one of the weapon's moves
        Assert.Equal("moves.block.followUps[0].to", Refused(Warden(Route(MoveFollowUpTrigger.Primary, WeaponMoveSlot.Heavy))).Field);
        // the chain has two links
        Assert.Equal("moves.block.followUps[0].link", Refused(Warden(Route(MoveFollowUpTrigger.Primary, WeaponMoveSlot.Primary, link: 3))).Field);
        Assert.Equal("moves.block.followUps[0].link", Refused(Warden(Route(MoveFollowUpTrigger.Primary, WeaponMoveSlot.Primary, link: 0))).Field);
        Assert.Equal("moves.block.followUps[0].occurrence", Refused(Warden(Route(MoveFollowUpTrigger.Primary, WeaponMoveSlot.Primary, occurrence: -1))).Field);
        Assert.Equal("moves.block.followUps[0].subSequence", Refused(Warden(Route(MoveFollowUpTrigger.Primary, WeaponMoveSlot.Primary, sub: -1))).Field);
        Assert.Equal("moves.block.followUps[0].trigger", Refused(Warden(Route((MoveFollowUpTrigger)9, WeaponMoveSlot.Primary))).Field);
        var many = Enumerable.Range(0, 65).Select(i => Route(MoveFollowUpTrigger.Primary, WeaponMoveSlot.Primary, occurrence: i)).ToArray();
        Assert.Equal("moves.block.followUps", Refused(Warden(many)).Field);
        Lower(Warden(many[..64]));
    }

    [Fact]
    public void Identical_routes_and_all_against_one_occurrence_are_refused_as_written_naming_both()
    {
        var same = Route(MoveFollowUpTrigger.Primary, WeaponMoveSlot.Primary, occurrence: 0);
        var e = Refused(Warden(same, same));
        Assert.Equal("moves.block.followUps[1]", e.Field);
        Assert.Contains("followUps[0]", e.Message);
        // all Primary windows and Primary occurrence 0 select a common window, whichever comes first and whatever they lead to
        e = Refused(Warden(Route(MoveFollowUpTrigger.Primary, WeaponMoveSlot.Primary), Route(MoveFollowUpTrigger.Primary, WeaponMoveSlot.Parry, occurrence: 0)));
        Assert.Equal("moves.block.followUps[1]", e.Field);
        Assert.Contains("followUps[0]", e.Message);
        Refused(Warden(Route(MoveFollowUpTrigger.Primary, WeaponMoveSlot.Parry, occurrence: 0), Route(MoveFollowUpTrigger.Primary, WeaponMoveSlot.Primary)));
        // disjoint occurrences, another trigger and another sub-sequence do not overlap
        Lower(Warden(Route(MoveFollowUpTrigger.Primary, WeaponMoveSlot.Primary, occurrence: 0), Route(MoveFollowUpTrigger.Primary, WeaponMoveSlot.Parry, occurrence: 1),
            Route(MoveFollowUpTrigger.Parried, WeaponMoveSlot.Parry), Route(MoveFollowUpTrigger.Primary, WeaponMoveSlot.Primary, sub: 1)));
    }

    // ---- the resolver, on the game's rows ----

    private static (Dictionary<(PlannedMove Move, int Sub, int K), PlannedMove> Routes, List<string> Problems, MovePlan Plan) Resolve(WeaponBlock b, bool offline = true)
    {
        MovePlan plan;
        if (offline) Assert.True(Compiler.Plans.TryGetValue(Lower(b), out plan!));
        else
        {
            // past the offline check, as a plan whose overlap only the data shows would be: the same plan with the routes put in afterwards
            Assert.True(Compiler.Plans.TryGetValue(Lower(Warden()), out plan!));
            plan = plan with { Moves = plan.Moves.Select(m => m.Slot == MoveSlot.Block ? m with { FollowUps = b.Moves!.Block!.FollowUps } : m).ToList() };
        }
        var (work, bases, _) = MovePlanner.Find(plan, Slots, new FakeGame().Reader());
        Assert.Empty(work.Problems);
        var problems = new List<string>();
        return (MoveRoutes.Resolve(plan, bases, problems), problems, plan);
    }

    [Fact]
    public void A_route_selects_only_windows_with_a_follow_up_of_its_trigger_in_its_sub_sequence()
    {
        var (routes, problems, plan) = Resolve(Warden());
        Assert.Empty(problems);
        var block = plan.Moves.Single(m => m.Slot == MoveSlot.Block);
        // the block's SetActionOportunity events of sub-sequence 0: #0 Parried, #1 Primary. Sub-sequence 1's Primary window is not selected by SubSequence 0
        Assert.Equal([(0, 0), (0, 1)], routes.Keys.Where(k => k.Move == block).Select(k => (k.Sub, k.K)).Order());
        Assert.Equal(MoveSlot.Parry, routes[(block, 0, 0)].Slot);
        Assert.Equal((MoveSlot.Primary, 0), (routes[(block, 0, 1)].Slot!.Value, routes[(block, 0, 1)].Link));
    }

    [Fact]
    public void Occurrences_count_the_triggers_windows_with_a_follow_up_in_the_motions_order()
    {
        // on the first light attack: Evade (no follow-up, never counted) is #0, the two Primary rows #1 and #2, Heavy #3
        var sword = Warden() with
        {
            Moves = Warden().Moves! with
            {
                Primary =
                [
                    new MoveBlock { Like = "Light1", FollowUps = [Route(MoveFollowUpTrigger.Primary, WeaponMoveSlot.Parry, occurrence: 1)] },
                    new MoveBlock { Like = "Light2" },
                ],
                Block = null,
            },
        };
        var (routes, problems, plan) = Resolve(sword);
        Assert.Empty(problems);
        var first = plan.Moves.Single(m => m.Slot == MoveSlot.Primary && m.Link == 0);
        Assert.Equal([(first, 0, 2)], routes.Keys);
        // and through the planner: only that row leaves the chain, the other Primary row and the Heavy row keep the chain's own links
        var (gen, planProblems) = MoveRoutes.Plan(plan, Slots, new FakeGame().Reader(), null);
        Assert.Empty(planProblems);
        var rules = gen!.SequenceRules.Single(r => r.Entry.StartsWith("routes-test.warden.primary.1.r")).Rules;
        var name = gen.Clones.ToDictionary(c => (string)c["name"]!, c => Guid.Parse((string)c["guid"]!));
        Assert.Equal(gen.LiveClones["primary/2"], name[(string)rules["events[SetActionOportunity#1].followUp"]!]);
        Assert.Equal(gen.LiveClones["parry/1"], name[(string)rules["events[SetActionOportunity#2].followUp"]!]);
        Assert.Null(rules["events[SetActionOportunity#0].followUp"]);     // the Evade window has no follow-up: none is made
        Assert.Null(rules["events[SetActionOportunity#3].followUp"]);     // Heavy: the weapon has no heavy move, the game's stays
    }

    [Fact]
    public void A_route_with_no_window_a_missing_sub_sequence_or_occurrence_is_a_problem_by_field()
    {
        // the block has no Heavy window
        var (_, p1, _) = Resolve(Warden(Route(MoveFollowUpTrigger.Heavy, WeaponMoveSlot.Primary)));
        Assert.Contains("moves.block.followUps[0]: the motion has no follow-up window for Heavy", Assert.Single(p1));
        var (_, p2, _) = Resolve(Warden(Route(MoveFollowUpTrigger.Primary, WeaponMoveSlot.Primary, sub: 2)));
        Assert.Contains("followUps[0].subSequence", Assert.Single(p2));
        var (_, p3, _) = Resolve(Warden(Route(MoveFollowUpTrigger.Primary, WeaponMoveSlot.Primary, occurrence: 1)));
        Assert.Contains("followUps[0].occurrence", Assert.Single(p3));
    }

    [Fact]
    public void Routes_that_meet_on_a_window_in_the_data_are_refused_naming_both_and_the_window()
    {
        var (routes, problems, _) = Resolve(Warden(Route(MoveFollowUpTrigger.Primary, WeaponMoveSlot.Primary), Route(MoveFollowUpTrigger.Primary, WeaponMoveSlot.Parry, occurrence: 0)), offline: false);
        var text = Assert.Single(problems);
        Assert.Contains("followUps[1]", text);
        Assert.Contains("followUps[0]", text);
        Assert.Contains("window 1", text);
        // no last-wins: the first route's target stays in the table, and the plan as a whole is refused
        Assert.Equal(MoveSlot.Primary, routes.Values.Single().Slot);
    }

    [Fact]
    public void The_plan_hash_follows_the_routes_and_the_data_and_nothing_else()
    {
        string Hash(WeaponBlock b, IReadOnlyList<SlotRow>? slots = null)
        {
            Assert.True(Compiler.Plans.TryGetValue(Lower(b), out var plan));
            var (gen, problems) = MoveRoutes.Plan(plan!, slots ?? Slots, new FakeGame().Reader(), null);
            Assert.Empty(problems);
            return gen!.Hash;
        }
        var a = Hash(Warden());
        Assert.Equal(a, Hash(Warden()));
        Assert.NotEqual(a, Hash(Warden(Route(MoveFollowUpTrigger.Parried, WeaponMoveSlot.Parry))));
        Assert.NotEqual(a, Hash(Warden(), [.. Slots.Reverse()]));
    }

    // ---- the application ----

    [Fact]
    public void A_generation_applies_whole_and_says_so_once_after_the_switch()
    {
        Start(Warden());
        Assert.Equal((MoveApplicationState.PendingContent, false, "PendingContent"), (Status().State, Status().Ok, Status().Reason));
        // the first pass plans and stages: the clones are defined, nothing is applied, nothing is said
        _game.Pending.Dequeue()(_ => { });
        Assert.Equal(4, _game.Defined.Count);
        Assert.Empty(_game.Applied);
        Assert.Empty(_events);
        Assert.Equal(MoveApplicationState.PendingContent, Status().State);
        Assert.Empty(_game.Pump());
        var now = Status();
        Assert.Equal((true, MoveApplicationState.Applied, "", 1ul, 1ul, 3ul), (now.Ok, now.State, now.Reason, now.RequestedGeneration, now.AppliedGeneration, now.ContentGeneration));
        Assert.Equal(["block/1", "parry/1", "primary/1", "primary/2"], now.LiveClones.Keys.Order());
        // two documents: the clones' rules, then the item's slots alone
        Assert.Equal(2, _game.Applied.Count);
        Assert.NotNull(_game.Applied[0]["sequences"]);
        Assert.Null(_game.Applied[0]["itemdata"]);
        Assert.Equal(["format", "itemdata"], _game.Applied[1].Select(p => p.Key));
        // block at 0, the air heavy following the primary at 1, the primary at 3, the parry at 4; the heavy at 2 is not the weapon's
        Assert.Equal(new Dictionary<int, Guid> { [0] = now.LiveClones["block/1"], [1] = now.LiveClones["primary/1"], [3] = now.LiveClones["primary/1"], [4] = now.LiveClones["parry/1"] }, _game.SlotsNow);
        // the block's windows lead to this weapon's own parry and first light attack; the rows of the light chain link to each other
        var name = _game.ByName.ToDictionary(x => x.Value, x => x.Key);
        var block = name[now.LiveClones["block/1"]];
        Assert.Equal(now.LiveClones["parry/1"], _game.LinksNow[(block, 0, 0)]);
        Assert.Equal(now.LiveClones["primary/1"], _game.LinksNow[(block, 0, 1)]);
        Assert.False(_game.LinksNow.ContainsKey((block, 1, 0)));        // the second sub-sequence was not routed
        Assert.Equal(now.LiveClones["primary/2"], _game.LinksNow[(name[now.LiveClones["primary/1"]], 0, 1)]);
        Assert.Equal(now.LiveClones["primary/2"], _game.LinksNow[(name[now.LiveClones["primary/1"]], 0, 2)]);
        // the GUIDs are the generation's, not the stable clones'
        Assert.DoesNotContain(BlockIds.Derive(Mod, "sequence", "warden.block"), now.LiveClones.Values);
        var ev = Assert.Single(_events);
        Assert.Equal((Mod, "warden", MoveApplicationState.PendingContent, MoveApplicationState.Applied), (ev.ModId, ev.BlockName, ev.Before.State, ev.After.State));
        Assert.Same(now, ev.After);
        // both documents in one content transaction, committed once everything read back
        Assert.Equal((1, 1, 0), (_game.Begins, _game.Commits, _game.Rollbacks));
    }

    [Theory]
    [InlineData(0)]
    [InlineData(1)]
    [InlineData(2)]
    [InlineData(3)]
    public void A_clone_the_game_refuses_leaves_the_slots_and_the_generation_and_a_retry_applies_once(int nth)
    {
        _game.RefuseDefineAt = nth;
        var (started, _) = Start(Warden());
        var problems = _game.Pump();
        Assert.Contains("the registry is full", Assert.Single(problems));
        var now = Status();
        Assert.Equal((false, MoveApplicationState.Refused, 1ul, 0ul), (now.Ok, now.State, now.RequestedGeneration, now.AppliedGeneration));
        Assert.Contains("was refused: the registry is full", now.Reason);
        Assert.Empty(now.LiveClones);
        Assert.Empty(_game.Applied);            // no rule and no slot was written
        Assert.Empty(_game.SlotsNow);
        Assert.Equal(0, _game.Begins);
        Assert.Equal(MoveApplicationState.Refused, Assert.Single(_events).After.State);
        // on the same content nothing is tried again by itself
        MoveApplications.Ready(started, _ => { });
        Assert.Empty(_game.Pending);
        // the retry: the clones staged before the refusal are not defined twice
        Assert.True(MoveApplications.Retry(Mod, "warden", _ => { }));
        Assert.Empty(_game.Pump());
        now = Status();
        Assert.Equal((true, MoveApplicationState.Applied, 2ul, 2ul), (now.Ok, now.State, now.RequestedGeneration, now.AppliedGeneration));
        Assert.Equal(4, _game.DefinedNames.Distinct().Count());
        Assert.Equal(4, _game.DefinedNames.Count);
        Assert.Equal([MoveApplicationState.Refused, MoveApplicationState.Applied], _events.Select(e => e.After.State));
    }

    [Fact]
    public void A_refusal_on_older_content_is_tried_again_when_content_is_applied_again()
    {
        _game.SlotRows = null;                  // the item is not in the game yet
        var (mod, _) = Start(Warden());
        _game.Pump();
        Assert.Equal(MoveApplicationState.Refused, Status().State);
        Assert.Contains("no item", Status().Reason);
        _game.SlotRows = Slots;
        MoveApplications.Ready(mod, _ => { });  // same content: nothing
        Assert.Empty(_game.Pending);
        Assert.Empty(_game.Defined);
        _game.ContentGeneration = 4;
        MoveApplications.Ready(mod, _ => { });
        _game.Pump();
        Assert.Equal((MoveApplicationState.Applied, 2ul, 4ul), (Status().State, Status().AppliedGeneration, Status().ContentGeneration));
    }

    [Fact]
    public void A_rule_the_game_refuses_ends_the_attempt_before_any_slot_changes()
    {
        _game.ApplyError = doc => doc["sequences"] != null ? "events[SetActionOportunity#1].followUp: no such field" : null;
        Start(Warden());
        _game.Pump();
        Assert.Equal(MoveApplicationState.Refused, Status().State);
        Assert.Contains("no such field", Status().Reason);
        Assert.Single(_game.Applied);           // the slots' document was never sent
        Assert.Empty(_game.SlotsNow);
        Assert.Equal(0ul, Status().AppliedGeneration);
        Assert.Equal((1, 0, 1), (_game.Begins, _game.Commits, _game.Rollbacks));
    }

    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public void A_readback_exception_refuses_the_attempt_and_restores_any_switched_slots(bool slot)
    {
        Start(Warden());
        if (slot) _game.ThrowSlotRead = true;
        else _game.ThrowLinkRead = true;
        var escaped = Record.Exception(() => _game.Pump());
        Assert.Null(escaped);
        Assert.Equal(MoveApplicationState.Refused, Status().State);
        Assert.Contains(slot ? "slot read unavailable" : "link read unavailable", Status().Reason);
        Assert.Equal(0ul, Status().AppliedGeneration);
        Assert.Empty(_game.SlotsNow);
        Assert.DoesNotContain(_events, e => e.After.Ok);
    }

    [Fact]
    public void An_apply_exception_is_a_refusal_with_a_reason()
    {
        _game.ApplyError = _ => throw new InvalidOperationException("rules unavailable");
        Start(Warden());
        Assert.Null(Record.Exception(() => _game.Pump()));
        Assert.Equal(MoveApplicationState.Refused, Status().State);
        Assert.Contains("rules unavailable", Status().Reason);
        Assert.Empty(_game.SlotsNow);
    }

    [Fact]
    public void A_link_that_does_not_read_back_ends_the_attempt_before_any_slot_changes()
    {
        Start(Warden());
        _game.Pending.Dequeue()(_ => { });
        // the game takes the document and says nothing, but one link is not what was set
        var real = _game.ApplyError;
        _game.ApplyError = doc =>
        {
            if (doc["sequences"] is JsonObject s) s.Remove(s.First().Key);
            return null;
        };
        _game.Pump();
        Assert.Equal(MoveApplicationState.Refused, Status().State);
        Assert.Contains("did not read back", Status().Reason);
        Assert.Empty(_game.SlotsNow);
        _game.ApplyError = real;
    }

    [Fact]
    public void A_switch_that_does_not_take_whole_is_taken_back_in_the_same_pass()
    {
        // first a generation that applies
        Start(Warden());
        _game.Pump();
        var first = Status();
        var slots = new Dictionary<int, Guid>(_game.SlotsNow);
        // then other routes, and a game that takes only the first slot of the switch
        _game.SwitchTakesOnlyFirst = true;
        var (mod2, plan2) = (NewMod(), (MovePlan?)null);
        Assert.True(Compiler.Plans.TryGetValue(Compiler.Lower(mod2, "warden", Warden(Route(MoveFollowUpTrigger.Parried, WeaponMoveSlot.Parry))).Low.Json, out plan2));
        // the same weapon, replanned in place (what a changed block does while its mod stays loaded)
        Replan(plan2!);                         // planned and staged; its completion waits for the next pass
        var problems = new List<string>();
        _game.Pending.Dequeue()(problems.Add);
        var now = Status();
        Assert.Equal((MoveApplicationState.Refused, 2ul, 1ul), (now.State, now.RequestedGeneration, now.AppliedGeneration));
        Assert.Contains("were not switched", now.Reason);
        Assert.Contains("taken back and read back", now.Reason);
        Assert.Equal(first.LiveClones, now.LiveClones);
        // the transaction was rolled back: the slots are the first generation's and the new clones' links are gone again
        Assert.Equal(slots, _game.SlotsNow);
        Assert.Equal((2, 1, 1), (_game.Begins, _game.Commits, _game.Rollbacks));
    }

    // a second plan for a weapon that is applied: the entry is the same, its plan is not (tests only: the product gets a new plan from a new definition)
    private void Replan(MovePlan plan)
    {
        var entries = typeof(MoveApplications).GetField("Entries", System.Reflection.BindingFlags.NonPublic | System.Reflection.BindingFlags.Static)!.GetValue(null)!;
        var entry = ((System.Collections.IDictionary)entries)[(Mod, "warden")]!;
        entry.GetType().GetField("Plan")!.SetValue(entry, plan);
        Assert.True(MoveApplications.Retry(Mod, "warden", _ => { }));
    }

    // ---- fault injection: at each write, at each restore ----

    [Theory]
    [InlineData("sequences")]
    [InlineData("itemdata")]
    public void A_write_refused_after_it_changed_part_of_the_game_is_taken_back_whole(string section)
    {
        Start(Warden());
        _game.Pump();
        var first = Status();
        var (slots, links) = (new Dictionary<int, Guid>(_game.SlotsNow), new Dictionary<(string, int, int), Guid>(_game.LinksNow));
        Assert.True(Compiler.Plans.TryGetValue(Lower(Warden(Route(MoveFollowUpTrigger.Parried, WeaponMoveSlot.Parry))), out var other));
        Replan(other!);
        // the document of that section changes everything it names (the switch: one slot only), then reports a refused rule
        _game.SwitchTakesOnlyFirst = section == "itemdata";
        _game.LateError = doc => doc[section] != null ? $"{section}: a rule was refused" : null;
        var problems = _game.Pump();
        var now = Status();
        Assert.Equal((false, MoveApplicationState.Refused, 2ul, 1ul), (now.Ok, now.State, now.RequestedGeneration, now.AppliedGeneration));
        Assert.Contains("a rule was refused", now.Reason);
        Assert.Contains("taken back and read back", now.Reason);
        Assert.Contains(problems, p => p.Contains("a rule was refused"));
        // exactly the previous state: the first generation's slots and links, nothing of the second's
        Assert.Equal(slots, _game.SlotsNow);
        Assert.Equal(links, _game.LinksNow);
        Assert.Equal(first.LiveClones, now.LiveClones);
        Assert.Equal((2, 1, 1, 0), (_game.Begins, _game.Commits, _game.Rollbacks, _game.Revokes));
    }

    [Fact]
    public void The_lab_switch_fails_a_write_after_it_applied_and_the_transaction_takes_it_back()
    {
        MoveApplications.FailWrites = 1;
        Start(Warden());
        _game.Pump();
        Assert.Equal((MoveApplicationState.Refused, 0ul), (Status().State, Status().AppliedGeneration));
        Assert.Contains("refused by the test switch", Status().Reason);
        Assert.Single(_game.Applied);            // the clones' rules went in, then were refused; the slots' document never went
        Assert.Empty(_game.LinksNow);
        Assert.Empty(_game.SlotsNow);
        Assert.Equal(0, MoveApplications.FailWrites);
    }

    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public void A_rollback_that_does_not_read_back_revokes_the_owner_and_the_status_says_so(bool applied)
    {
        // with a generation applied before, and as the first attempt
        Start(Warden());
        if (applied) _game.Pump();
        else _game.Pending.Dequeue()(_ => { });
        if (applied)
        {
            Assert.True(Compiler.Plans.TryGetValue(Lower(Warden(Route(MoveFollowUpTrigger.Parried, WeaponMoveSlot.Parry))), out var other));
            Replan(other!);
        }
        _game.SwitchTakesOnlyFirst = true;      // the switch does not take whole, and taking it back does not read back
        _game.FailRestore = true;
        var problems = _game.Pump();
        var now = Status();
        Assert.Equal((false, MoveApplicationState.Refused, 0ul), (now.Ok, now.State, now.AppliedGeneration));
        Assert.Contains("were not switched", now.Reason);
        Assert.Contains("could not be verified", now.Reason);
        Assert.Contains("every layer of the mod was taken off", now.Reason);
        Assert.Contains(problems, p => p.Contains("every layer of the mod was taken off"));
        // fail closed: the owner's layers are off, the slots play the class's sequences, and the status says what they play
        Assert.Empty(_game.SlotsNow);
        Assert.Empty(_game.LinksNow);
        Assert.Equal(4, now.LiveClones.Count);
        Assert.All(now.LiveClones, c => Assert.StartsWith("slot/", c.Key));
        Assert.DoesNotContain(now.LiveClones.Values, g => Slots.All(r => r.Sequence != g));
        // not tried again by itself on the same content; a retry plans from the class's sequences and applies whole
        (_game.SwitchTakesOnlyFirst, _game.FailRestore) = (false, false);
        MoveApplications.Ready(new ModBlocks("other", 1), _ => { });
        Assert.Empty(_game.Pending);
        Assert.True(MoveApplications.Retry(Mod, "warden", _ => { }));
        Assert.Empty(_game.Pump());
        Assert.Equal(MoveApplicationState.Applied, Status().State);
        Assert.Equal(4, Status().LiveClones.Keys.Count(k => !k.StartsWith("slot/")));
    }

    [Fact]
    public void A_revoked_owner_takes_its_other_weapons_off_and_they_say_so_and_are_planned_again()
    {
        var mod = NewMod();
        var a = Compiler.Lower(mod, "warden", Warden()).Low.Json;
        var b = Compiler.Lower(mod, "keeper", Warden(Route(MoveFollowUpTrigger.Parried, WeaponMoveSlot.Parry))).Low.Json;
        Assert.True(Compiler.Plans.TryGetValue(a, out var pa));
        Assert.True(Compiler.Plans.TryGetValue(b, out var pb));
        MoveApplications.Start(mod, pa!, a);
        _game.Pump();
        Assert.Equal(MoveApplicationState.Applied, Status().State);
        // the second weapon of the same mod (the same owner) fails and its rollback does not read back
        _game.SwitchTakesOnlyFirst = true;
        _game.FailRestore = true;
        MoveApplications.Start(mod, pb!, b);
        _game.Pending.Dequeue()(_ => { });      // planned and staged
        _game.Pending.Dequeue()(_ => { });      // written, refused, not taken back
        Assert.Contains("every layer of the mod was taken off", Status("keeper").Reason);
        var warden = Status();
        Assert.Equal((MoveApplicationState.Refused, 0ul), (warden.State, warden.AppliedGeneration));
        Assert.Contains("every layer of the mod was taken off (", warden.Reason);
        Assert.Contains("keeper's moves could not be taken back", warden.Reason);
        Assert.True(mod.RulesDirty);            // the mod's other rules are applied again at the next pass
        // the ready pass asked for plans the innocent weapon again; the one that failed waits for other content
        (_game.SwitchTakesOnlyFirst, _game.FailRestore) = (false, false);
        Assert.Single(_game.Pending);
        _game.Pump();
        MoveApplications.Ready(mod, _ => { });
        Assert.Empty(_game.Pending);
        Assert.Equal(MoveApplicationState.Applied, Status().State);
        Assert.Equal(MoveApplicationState.Refused, Status("keeper").State);
    }

    [Fact]
    public void Owner_layers_taken_off_while_the_transaction_is_open_are_said_so()
    {
        Start(Warden());
        _game.Pending.Dequeue()(_ => { });
        _game.CancelOnApply = true;             // the first document's application takes the owner's layers off (as revert_owner)
        _game.Pump();
        Assert.Equal((MoveApplicationState.Refused, 0ul), (Status().State, Status().AppliedGeneration));
        Assert.Contains("the mod's layers were taken off", Status().Reason);
        Assert.Empty(_game.SlotsNow);
    }

    [Fact]
    public void Without_content_transactions_nothing_is_written_and_the_attempt_is_refused()
    {
        _game.NoTransactions = true;
        Start(Warden());
        _game.Pump();
        Assert.Equal((MoveApplicationState.Refused, 0ul), (Status().State, Status().AppliedGeneration));
        Assert.Contains("the content transaction could not be opened", Status().Reason);
        Assert.Empty(_game.Applied);
    }

    // ---- a content application later in the process ----

    [Fact]
    public void A_replay_writes_the_generation_again_in_a_transaction_and_holds_without_an_event()
    {
        var (mod, _) = Start(Warden());
        _game.Pump();
        var first = Status();
        var (slots, links) = (new Dictionary<int, Guid>(_game.SlotsNow), new Dictionary<(string, int, int), Guid>(_game.LinksNow));
        var problems = new List<string>();
        MoveApplications.Replay(mod, problems.Add);
        Assert.Same(first, Status());
        Assert.Empty(problems);
        Assert.Single(_events);
        Assert.Equal((2, 2), (_game.Begins, _game.Commits));
        // the world was loaded again: every layer went; the replay brings the generation back whole
        _game.SlotsNow.Clear();
        _game.LinksNow.Clear();
        _game.ContentGeneration = 9;
        MoveApplications.Replay(mod, problems.Add);
        Assert.Same(first, Status());
        Assert.Empty(problems);
        Assert.Equal(slots, _game.SlotsNow);
        Assert.Equal(links, _game.LinksNow);
    }

    [Fact]
    public void A_replay_that_fails_after_a_reload_is_taken_back_and_the_status_says_what_plays()
    {
        var (mod, _) = Start(Warden());
        _game.Pump();
        var problems = new List<string>();
        // the world was loaded again (every layer went) and the replay's slot document is refused after it switched one slot
        _game.SlotsNow.Clear();
        _game.LinksNow.Clear();
        _game.ContentGeneration = 9;
        _game.SwitchTakesOnlyFirst = true;
        _game.LateError = doc => doc["itemdata"] != null ? "sequences[0].sequence: not writable" : null;
        MoveApplications.Replay(mod, problems.Add);
        var now = Status();
        Assert.Equal((false, MoveApplicationState.Refused, 0ul, 9ul), (now.Ok, now.State, now.AppliedGeneration, now.ContentGeneration));
        Assert.Contains("After the game's content was applied again", now.Reason);
        Assert.Contains("not writable", now.Reason);
        Assert.Contains("no generation is applied", now.Reason);
        Assert.Single(problems);
        // taken back to what the reload left: the class's slots, no link
        Assert.Empty(_game.SlotsNow);
        Assert.Empty(_game.LinksNow);
        Assert.Equal(MoveApplicationState.Refused, _events[^1].After.State);
        // a ready pass was asked for: it plans the weapon again, on the same content, and it applies
        (_game.SwitchTakesOnlyFirst, _game.LateError) = (false, null);
        Assert.NotEmpty(_game.Pending);
        Assert.Empty(_game.Pump());
        Assert.Equal((MoveApplicationState.Applied, 2ul), (Status().State, Status().AppliedGeneration));
    }

    [Fact]
    public void A_replay_that_fails_while_the_generation_still_plays_is_a_refusal_that_keeps_it()
    {
        var (mod, _) = Start(Warden());
        _game.Pump();
        var first = Status();
        var slots = new Dictionary<int, Guid>(_game.SlotsNow);
        MoveApplications.FailWrites = 1;
        var problems = new List<string>();
        MoveApplications.Replay(mod, problems.Add);
        var now = Status();
        Assert.Equal((false, MoveApplicationState.Refused, 1ul), (now.Ok, now.State, now.AppliedGeneration));
        Assert.Contains("still plays", now.Reason);
        Assert.Equal(first.LiveClones, now.LiveClones);
        Assert.Equal(slots, _game.SlotsNow);
        // the ready pass it asked for finds the same plan playing: Applied again, nothing written
        var applied = _game.Applied.Count;
        _game.Pump();
        Assert.Equal((MoveApplicationState.Applied, 1ul), (Status().State, Status().AppliedGeneration));
        Assert.Equal(applied, _game.Applied.Count);
    }

    [Fact]
    public void A_replay_whose_rollback_does_not_read_back_revokes_and_is_not_retried_on_the_same_content()
    {
        var (mod, _) = Start(Warden());
        _game.Pump();
        _game.ContentGeneration = 9;
        _game.LateError = doc => doc["sequences"] != null ? "events[SetActionOportunity#1].followUp: not writable" : null;
        _game.FailRestore = true;
        MoveApplications.Replay(mod, _ => { });
        var now = Status();
        Assert.Equal((MoveApplicationState.Refused, 0ul), (now.State, now.AppliedGeneration));
        Assert.Contains("could not be verified", now.Reason);
        Assert.Empty(_game.SlotsNow);
        (_game.LateError, _game.FailRestore) = (null, false);
        MoveApplications.Ready(mod, _ => { });
        Assert.Empty(_game.Pending);            // fail closed: not by itself on the same content
        _game.ContentGeneration = 10;
        MoveApplications.Ready(mod, _ => { });
        _game.Pump();
        Assert.Equal(MoveApplicationState.Applied, Status().State);
    }

    [Fact]
    public void A_first_switch_that_fails_leaves_the_classs_slots_and_no_rule_of_the_clones()
    {
        _game.ApplyError = doc => doc["itemdata"] is JsonObject items && items.First().Value!.AsObject().First().Value is not JsonObject ? "sequences[0].sequence: not a reference" : null;
        Start(Warden());
        _game.Pump();
        Assert.Equal((MoveApplicationState.Refused, 0ul), (Status().State, Status().AppliedGeneration));
        // all or nothing: the clones' rules were taken back with the switch
        Assert.Empty(_game.SlotsNow);
        Assert.Empty(_game.LinksNow);
        Assert.Equal((1, 0, 1), (_game.Begins, _game.Commits, _game.Rollbacks));
    }

    [Fact]
    public void The_same_plan_again_defines_nothing_writes_nothing_and_raises_nothing()
    {
        Start(Warden());
        _game.Pump();
        var (defines, applied, first) = (_game.Defines, _game.Applied.Count, Status());
        // the slots name the generation's clones now (the block's own, which has no like, among them): the plan is still the same plan
        Assert.Equal(first.LiveClones["block/1"], _game.Slots("x")![0].Sequence);
        Assert.True(MoveApplications.Retry(Mod, "warden", _ => { }));
        Assert.Empty(_game.Pending);
        Assert.Equal((defines, applied), (_game.Defines, _game.Applied.Count));
        Assert.Same(first, Status());
        Assert.Single(_events);
    }

    [Fact]
    public void A_stale_completion_is_discarded()
    {
        Start(Warden());
        _game.Pending.Dequeue()(_ => { });      // staged; its completion waits
        var stale = _game.Pending.Dequeue();
        _game.RefuseDefineAt = -1;
        // a newer attempt takes the weapon before the first completes
        Assert.True(Compiler.Plans.TryGetValue(Lower(Warden(Route(MoveFollowUpTrigger.Parried, WeaponMoveSlot.Parry))), out var other));
        Replan(other!);
        stale(_ => { });
        Assert.Empty(_game.Applied);            // the stale one wrote nothing
        Assert.Empty(_events);
        _game.Pump();
        Assert.Equal((MoveApplicationState.Applied, 2ul, 2ul), (Status().State, Status().RequestedGeneration, Status().AppliedGeneration));
        Assert.Single(_events);
    }

    [Fact]
    public void A_seventeenth_plan_is_refused_as_restart_required_without_a_write()
    {
        Start(Warden());
        _game.Pump();
        for (var i = 1; i < MoveRoutes.MaxHashes; i++)
        {
            // another plan each time: the first light attack's blow moves
            Assert.True(Compiler.Plans.TryGetValue(Lower(Warden() with { Moves = Warden().Moves! with { Primary = [new MoveBlock { Like = "Light1", HitAt = 0.3 + i * 0.01 }, new MoveBlock { Like = "Light2" }] } }), out var p));
            Replan(p!);
            Assert.Empty(_game.Pump());
            Assert.Equal((ulong)(i + 1), Status().AppliedGeneration);
        }
        var (defines, applied, before) = (_game.Defines, _game.Applied.Count, Status());
        Assert.True(Compiler.Plans.TryGetValue(Lower(Warden() with { Moves = Warden().Moves! with { Primary = [new MoveBlock { Like = "Light1", HitAt = 0.9 }, new MoveBlock { Like = "Light2" }] } }), out var last));
        Replan(last!);
        Assert.Empty(_game.Pending);
        Assert.StartsWith("RestartRequired", Status().Reason);
        Assert.Equal((MoveApplicationState.Refused, before.AppliedGeneration, defines, applied), (Status().State, Status().AppliedGeneration, _game.Defines, _game.Applied.Count));
        Assert.Equal(before.LiveClones, Status().LiveClones);
        // a plan it has had before is still taken
        Assert.True(Compiler.Plans.TryGetValue(Lower(Warden()), out var again));
        Replan(again!);
        Assert.Empty(_game.Pump());
        Assert.Equal(MoveApplicationState.Applied, Status().State);
        Assert.Equal(defines, _game.Defines);
    }

    [Fact]
    public void A_mod_that_goes_reports_removed_with_the_generation_it_had_and_is_not_written_again()
    {
        var (mod, _) = Start(Warden());
        _game.Pump();
        MoveApplications.Forget(Mod);
        var now = Status();
        Assert.Equal((false, MoveApplicationState.Removed, "Removed", 1ul), (now.Ok, now.State, now.Reason, now.AppliedGeneration));
        Assert.Empty(now.LiveClones);
        var applied = _game.Applied.Count;
        MoveApplications.Replay(mod, _ => { });
        Assert.Equal(applied, _game.Applied.Count);
        Assert.Equal([MoveApplicationState.Applied, MoveApplicationState.Removed], _events.Select(e => e.After.State));
        // once: a second Forget and a late pass change nothing
        MoveApplications.Forget(Mod);
        Assert.False(MoveApplications.Retry(Mod, "warden", _ => { }) && _game.Pending.Count > 0);
        Assert.Equal(2, _events.Count);
    }

    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public void Removing_a_generation_resets_its_live_rules_even_when_the_service_owner_survives(bool reset)
    {
        var (mod, _) = Start(Warden());
        _game.Pump();
        Assert.NotEmpty(_game.SlotsNow);
        Assert.NotEmpty(_game.LinksNow);
        if (reset) mod.Reset();
        else MoveApplications.Forget(Mod);
        Assert.Empty(_game.SlotsNow);
        Assert.Empty(_game.LinksNow);
        Assert.Equal(MoveApplicationState.Removed, Status().State);
        // Reset paths remove only this owner's layers in kiln.gamedata; no whole-owner or other-mod revert is needed.
        var drops = _game.Applied.Skip(2).ToList();
        Assert.NotEmpty(drops);
        Assert.All(drops.SelectMany(d => d.Where(p => p.Key != "format").SelectMany(p => p.Value!.AsObject()).SelectMany(p => p.Value!.AsObject())),
            p => Assert.True((bool)p.Value!["reset"]!));
        Assert.Equal(0, _game.Revokes);
    }

    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public void A_removal_that_does_not_take_revokes_the_owner_instead_of_leaving_routed_slots(bool refused)
    {
        Start(Warden());
        _game.Pump();
        // the reset document is refused, or it takes only its first slot and the others still play the weapon's clones
        if (refused) _game.ApplyError = doc => doc["itemdata"] != null ? "the store is busy" : null;
        else _game.SwitchTakesOnlyFirst = true;
        MoveApplications.Forget(Mod);
        var now = Status();
        Assert.Equal(MoveApplicationState.Removed, now.State);
        Assert.Contains("every layer of the mod was taken off instead", now.Reason);
        Assert.Contains(refused ? "the store is busy" : "still plays a clone of the weapon", now.Reason);
        Assert.Equal(1, _game.Revokes);
        Assert.Empty(_game.SlotsNow);
        Assert.Empty(_game.LinksNow);
    }

    [Theory]
    [InlineData("sequences")]
    [InlineData("itemdata")]
    public void An_inline_content_handler_can_remove_the_weapon_without_the_completion_republishing_it(string section)
    {
        Start(Warden());
        _game.AfterApply = doc =>
        {
            if (doc[section] == null) return;
            _game.AfterApply = null;
            MoveApplications.Forget(Mod);
        };
        _game.Pump();
        Assert.Equal(MoveApplicationState.Removed, Status().State);
        Assert.Empty(_game.SlotsNow);
        Assert.Empty(_game.LinksNow);
        Assert.DoesNotContain(_events, e => e.After.Ok);
    }

    [Fact]
    public void A_handler_of_the_commits_change_event_can_remove_the_weapon_without_it_being_applied()
    {
        var (mod, _) = Start(Warden());
        _game.AfterCommit = () =>
        {
            _game.AfterCommit = null;
            MoveApplications.Forget(Mod);
        };
        _game.Pump();
        Assert.Equal(MoveApplicationState.Removed, Status().State);
        Assert.Empty(_game.SlotsNow);
        Assert.Empty(_game.LinksNow);
        Assert.DoesNotContain(_events, e => e.After.Ok);
        var applied = _game.Applied.Count;
        MoveApplications.Replay(mod, _ => { });
        Assert.Equal(applied, _game.Applied.Count);
    }

    // ---- the query ----

    // the class Blocks is hidden by this assembly's namespace Kiln.Blocks
    private static readonly Type TheBlocks = typeof(IBlock).Assembly.GetType("Kiln.Blocks", true)!;

    private static MoveApplicationResult MovesStatus(BlockHandle<WeaponBlock> weapon)
    {
        try { return (MoveApplicationResult)TheBlocks.GetMethod("MovesStatus")!.Invoke(null, [weapon])!; }
        catch (System.Reflection.TargetInvocationException e) when (e.InnerException != null)
        {
            System.Runtime.ExceptionServices.ExceptionDispatchInfo.Capture(e.InnerException).Throw();
            throw;
        }
    }

    [Fact]
    public void The_status_query_refuses_a_held_row_a_stale_handle_and_a_weapon_without_routes()
    {
        var mod = (ModBlocks)TheBlocks.GetMethod("ModFor", System.Reflection.BindingFlags.NonPublic | System.Reflection.BindingFlags.Static)!.Invoke(null, [Mod, (Func<ulong>)(() => 7)])!;
        mod.Reset();
        // the handles as Blocks.Define makes them (it needs kiln.content, which a test has not): the routed weapon started, the plain one not
        var routed = new BlockHandle<WeaponBlock>(Mod, "warden", Guid.NewGuid(), "routes-test.warden", Warden());
        var plain = new BlockHandle<WeaponBlock>(Mod, "plain", Guid.NewGuid(), "routes-test.plain", new WeaponBlock { Name = "Plain", Class = WeaponClass.Sword });
        mod.Handles["warden"] = routed;
        mod.Handles["plain"] = plain;
        Start(Warden());
        var gamedata = BlockCapabilities.GameData;
        try
        {
            BlockCapabilities.GameData = () => true;
            Assert.Equal(MoveApplicationState.PendingContent, MovesStatus(routed).State);
            Assert.Contains("no followUps", Assert.Throws<ArgumentException>(() => MovesStatus(plain)).Message);
            Assert.Throws<ArgumentNullException>(() => MovesStatus(null!));
            // a handle the mod no longer has under that name
            mod.Handles.TryRemove("warden", out _);
            Assert.Throws<ArgumentException>(() => MovesStatus(routed));
            mod.Handles["warden"] = routed;
            BlockCapabilities.Held = name => name == "blocks.move_followups";
            Assert.Contains("blocks.move_followups", Assert.Throws<KilnUnavailableException>(() => MovesStatus(routed)).Message);
            BlockCapabilities.Held = _ => false;
            BlockCapabilities.GameData = () => false;
            Assert.Throws<KilnUnavailableException>(() => MovesStatus(routed));
        }
        finally
        {
            BlockCapabilities.GameData = gamedata;
            mod.Reset();
        }
    }
}
