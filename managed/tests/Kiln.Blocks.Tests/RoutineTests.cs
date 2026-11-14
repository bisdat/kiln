// SPDX-License-Identifier: MIT
// Villager routines with no game: a RoutineBlock's checks, which window a minute takes, the wire document and its hash,
// the plan's identity, what a world keeps and what a restart does with it. The golden wire text and hash below are the
// same constants Kiln.TestKit.Tests (RoutinesModelTests) feeds the kit's model of kiln.routines: they were made outside
// this code (sha256 of the canonical text written by hand), so the two sides cannot agree by being wrong together.
using System.Text.Json;
using System.Text.Json.Nodes;
using Xunit;

namespace Kiln.BlockTests;

public sealed class RoutineTests : IDisposable
{
    private const string Mod = "routine-test";

    // {"fallback":{"kind":"roam"},"format":1,"planVersion":2,"windows":[{"allDay":false,"endMinute":720,"priority":0,"startMinute":360,"task":{"kind":"inspect"}},
    //  {"allDay":false,"endMinute":360,"priority":5,"startMinute":1320,"task":{"kind":"sleep"}},{"allDay":true,"endMinute":0,"priority":-1,"startMinute":0,"task":{"kind":"sit"}}]}
    internal const string GoldenHash = "02dcb72ca219f3a72202cede3638112dda4aa5a71948b89543c43f667c4d3ebb";
    internal const string GoldenWire =
        """{"format":1,"key":"routine-test:day","planVersion":2,"contentHash":"02dcb72ca219f3a72202cede3638112dda4aa5a71948b89543c43f667c4d3ebb","windows":[{"startMinute":360,"endMinute":720,"allDay":false,"priority":0,"task":{"kind":"inspect"}},{"startMinute":1320,"endMinute":360,"allDay":false,"priority":5,"task":{"kind":"sleep"}},{"startMinute":0,"endMinute":0,"allDay":true,"priority":-1,"task":{"kind":"sit"}}],"fallback":{"kind":"roam"}}""";

    private static readonly RoutineBlock Day = new()
    {
        Version = 2,
        Windows =
        [
            new RoutineWindow(360, 720, new InspectTask()),
            new RoutineWindow(1320, 360, new SleepTask(), Priority: 5),
            new RoutineWindow(0, 0, new SitTask(), Priority: -1, AllDay: true),
        ],
        Fallback = new RoamTask(),
    };

    public RoutineTests() => RoutineRegistry.Forget(Mod);

    public void Dispose() => RoutineRegistry.Forget(Mod);

    private static ModBlocks Ledger() => new(Mod, 1) { ManifestClients = "none" };

    private static RoutinePlan Plan(RoutineBlock b, string name = "day") => RoutineCompiler.Plan("routine " + name, Mod, b.Was ?? name, b);

    private static BlockHandle<RoutineBlock> Define(ModBlocks mod, string name, RoutineBlock b)
    {
        var (handle, low, _) = Compiler.Lower(mod, name, b);
        low.Commit();
        mod.Handles[name] = handle;
        return (BlockHandle<RoutineBlock>)handle;
    }

    // ---- the wire document and its hash -----------------------------------------------------------------------------

    [Fact]
    public void A_plan_lowers_to_the_wire_document_of_format_1()
    {
        var plan = Plan(Day);
        Assert.Equal(GoldenHash, plan.ContentHash);
        Assert.Equal(GoldenWire, plan.Wire);
        Assert.Equal("routine-test:day", plan.Key);
    }

    [Fact]
    public void A_block_asks_nothing_of_clients_and_lowers_to_its_wire_document_only()
    {
        // "clients": "none" in the manifest: a routine that asked for more would throw here
        var (handle, low, _) = Compiler.Lower(Ledger(), "day", Day);
        Assert.Equal(GoldenWire, low.Json["routine"]!.ToJsonString());
        Assert.Single(low.Json);
        Assert.Equal("routine-test:day", handle.GameName);
        Assert.Equal(BlockIds.Derive(Mod, "routine", "day"), handle.Guid);
        // nothing is registered before the commit
        Assert.Null(RoutineRegistry.Find("routine-test:day"));
        low.Commit();
        Assert.Equal(GoldenHash, RoutineRegistry.Find("routine-test:day")!.Plan.ContentHash);
    }

    [Fact]
    public void A_data_file_writes_the_same_plan_whatever_the_order_of_its_properties()
    {
        var a = BlockJson.Parse<RoutineBlock>("""
            { "version": 2, "fallback": { "kind": "roam" }, "windows": [
              { "startMinute": 360, "endMinute": 720, "task": { "kind": "inspect" } },
              { "startMinute": 1320, "endMinute": 360, "task": { "kind": "sleep" }, "priority": 5 },
              { "allDay": true, "task": { "kind": "sit" }, "priority": -1 } ] }
            """);
        var b = BlockJson.Parse<RoutineBlock>("""
            { "windows": [
              { "task": { "kind": "inspect" }, "priority": 0, "endMinute": 720, "allDay": false, "startMinute": 360 },
              { "priority": 5, "endMinute": 360, "task": { "kind": "sleep" }, "startMinute": 1320 },
              { "priority": -1, "endMinute": 0, "startMinute": 0, "task": { "kind": "sit" }, "allDay": true } ],
              "fallback": { "kind": "roam" }, "version": 2 }
            """);
        Assert.Equal(GoldenHash, Plan(a).ContentHash);
        Assert.Equal(GoldenHash, Plan(b).ContentHash);
        Assert.Equal(GoldenWire, Plan(b).Wire);
    }

    [Fact]
    public void The_hash_follows_the_content_and_the_version_and_not_the_name()
    {
        Assert.Equal(GoldenHash, Plan(Day, "other-name").ContentHash);
        Assert.Equal(GoldenHash, Plan(Day with { Was = "earlier" }, "renamed").ContentHash);
        Assert.NotEqual(GoldenHash, Plan(Day with { Version = 3 }).ContentHash);
        Assert.NotEqual(GoldenHash, Plan(Day with { Fallback = new IdleTask() }).ContentHash);
        Assert.NotEqual(GoldenHash, Plan(Day with { Windows = [.. Day.Windows.Reverse()] }).ContentHash);   // the order of the windows decides ties
        Assert.NotEqual(GoldenHash, Plan(Day with { Windows = [Day.Windows[0] with { Priority = 1 }, Day.Windows[1], Day.Windows[2]] }).ContentHash);
    }

    [Fact]
    public void The_wire_document_reads_back_in_any_property_order_and_refuses_what_it_does_not_know()
    {
        var back = RoutinePlan.Parse(GoldenWire);
        Assert.Equal(GoldenWire, back.Wire);
        Assert.Equal(3, back.Windows.Length);
        Assert.Equal(new PlanWindow(1320, 360, false, 5, RoutineKind.Sleep), back.Windows[1]);

        var shuffled = """{"fallback":{"kind":"roam"},"windows":[{"task":{"kind":"inspect"},"priority":0,"allDay":false,"endMinute":720,"startMinute":360},{"startMinute":1320,"endMinute":360,"allDay":false,"priority":5,"task":{"kind":"sleep"}},{"startMinute":0,"endMinute":0,"allDay":true,"priority":-1,"task":{"kind":"sit"}}],"contentHash":"02dcb72ca219f3a72202cede3638112dda4aa5a71948b89543c43f667c4d3ebb","planVersion":2,"key":"routine-test:day","format":1}""";
        Assert.Equal(GoldenHash, RoutinePlan.Parse(shuffled).ContentHash);
        Assert.Equal(GoldenWire, RoutinePlan.Parse(shuffled).Wire);

        string Bad(string wire) => Assert.Throws<FormatException>(() => RoutinePlan.Parse(wire)).Message;
        Assert.Contains("contentHash", Bad(GoldenWire.Replace("\"priority\":5", "\"priority\":6")));       // content changed under the hash
        Assert.Contains("format 2", Bad(GoldenWire.Replace("\"format\":1", "\"format\":2")));
        Assert.Contains("owner", Bad(GoldenWire.Replace("\"format\":1,", "\"format\":1,\"owner\":\"x\",")));
        Assert.Contains("twice", Bad(GoldenWire.Replace("\"format\":1,", "\"format\":1,\"format\":1,")));
        Assert.Contains("duration", Bad(GoldenWire.Replace("{\"kind\":\"inspect\"}", "{\"kind\":\"inspect\",\"duration\":5}")));
        Assert.Contains("routines.anchors", Bad(GoldenWire.Replace("{\"kind\":\"inspect\"}", "{\"kind\":\"goTo\"}")));
        Assert.Contains("routines.jobs", Bad(GoldenWire.Replace("{\"kind\":\"inspect\"}", "{\"kind\":\"job\"}")));
        Assert.Contains("priority", Bad(GoldenWire.Replace("\"priority\":0,", "")));
        Assert.Contains("whole number", Bad(GoldenWire.Replace("\"startMinute\":360", "\"startMinute\":360.5")));
        Assert.Contains("key", Bad(GoldenWire.Replace("routine-test:day", "day")));
        Assert.Contains("not JSON", Bad("{"));
    }

    // ---- a data file's task ---------------------------------------------------------------------------------------------

    [Theory]
    [InlineData("""{ "kind": "idle", "duration": 5 }""", "duration")]
    [InlineData("""{ "kind": "roam", "radius": 20 }""", "radius")]
    [InlineData("""{ "kind": "nap" }""", "is not a task")]
    [InlineData("""{ "kind": "goTo" }""", "routines.anchors")]
    [InlineData("""{ "kind": "job" }""", "routines.jobs")]
    [InlineData("""{ "kind": "idle", "note": "x" }""", "not part of a task")]
    [InlineData("""{ }""", "needs a kind")]
    [InlineData("\"idle\"", "a task is an object")]
    [InlineData("""{ "kind": 3 }""", "kind is a name")]
    public void A_task_in_a_data_file_takes_a_kind_and_nothing_else(string task, string says)
    {
        var e = Assert.ThrowsAny<JsonException>(() => BlockJson.Parse<RoutineBlock>($$"""{ "windows": [ { "allDay": true, "task": {{task}} } ] }"""));
        Assert.Contains(says, e.Message);
    }

    [Fact]
    public void Every_task_kind_reads_and_writes_its_name()
    {
        foreach (var (kind, type) in new (string, Type)[] { ("vanilla", typeof(VanillaTask)), ("idle", typeof(IdleTask)), ("roam", typeof(RoamTask)), ("sleep", typeof(SleepTask)), ("sit", typeof(SitTask)), ("inspect", typeof(InspectTask)) })
        {
            var block = BlockJson.Parse<RoutineBlock>($$"""{ "windows": [ { "allDay": true, "task": { "kind": "{{kind}}" } } ] }""");
            Assert.IsType(type, block.Windows[0].Task);
            Assert.Equal($$"""{"kind":"{{kind}}"}""", JsonSerializer.Serialize(block.Windows[0].Task, BlockJsonAccess.Options));
        }
        Assert.IsType<VanillaTask>(BlockJson.Parse<RoutineBlock>("{}").Fallback);
        Assert.ThrowsAny<JsonException>(() => BlockJson.Parse<RoutineBlock>("""{ "duration": 3 }"""));   // an unknown field of the plan itself
    }

    // ---- what is wrong with a plan ----------------------------------------------------------------------------------

    public static TheoryData<RoutineBlock, string, string> Wrong() => new()
    {
        { new RoutineBlock { Version = 0 }, "version", "1 or more" },
        { new RoutineBlock { Was = "other-mod:day" }, "was", "local name" },
        { new RoutineBlock { Was = "" }, "was", "local name" },
        { new RoutineBlock { Was = "a/b" }, "was", "local name" },
        { new RoutineBlock { Windows = [new RoutineWindow(-1, 10, new IdleTask())] }, "windows[0].startMinute", "outside 0..1439" },
        { new RoutineBlock { Windows = [new RoutineWindow(0, 1440, new IdleTask())] }, "windows[0].endMinute", "outside 0..1439" },
        { new RoutineBlock { Windows = [new RoutineWindow(0, 60, new IdleTask()), new RoutineWindow(600, 600, new IdleTask())] }, "windows[1]", "starts and ends at 600" },
        { new RoutineBlock { Windows = [new RoutineWindow(0, 0, new IdleTask())] }, "windows[0]", "allDay" },
        { new RoutineBlock { Windows = [new RoutineWindow(0, 60, new IdleTask(), AllDay: true)] }, "windows[0]", "whole day" },
        { new RoutineBlock { Windows = [new RoutineWindow(0, 60, null!)] }, "windows[0].task", "needs a task" },
        { new RoutineBlock { Windows = [.. Enumerable.Range(0, 65).Select(i => new RoutineWindow(i, i + 1, new IdleTask()))] }, "windows", "at most 64" },
        { new RoutineBlock { Fallback = new SleepTask() }, "fallback", "vanilla, idle or roam" },
        { new RoutineBlock { Fallback = new SitTask() }, "fallback", "vanilla, idle or roam" },
        { new RoutineBlock { Fallback = new InspectTask() }, "fallback", "vanilla, idle or roam" },
        { new RoutineBlock { Fallback = null! }, "fallback", "null" },
        { new RoutineBlock { Windows = null! }, "windows", "null" },
    };

    [Theory]
    [MemberData(nameof(Wrong))]
    public void A_wrong_plan_is_refused_by_block_and_field(RoutineBlock block, string field, string says)
    {
        var e = Assert.Throws<BlockError>(() => Compiler.Lower(Ledger(), "day", block));
        Assert.Equal("routine day", e.Block);
        Assert.Equal(field, e.Field);
        Assert.Contains(says, e.Problem);
    }

    [Fact]
    public void The_limits_are_met_exactly()
    {
        var plan = Plan(new RoutineBlock { Windows = [.. Enumerable.Range(0, 64).Select(i => new RoutineWindow(i, i + 1, new IdleTask()))] });
        Assert.Equal(64, plan.Windows.Length);
        Plan(new RoutineBlock { Windows = [new RoutineWindow(1439, 0, new IdleTask()), new RoutineWindow(0, 1439, new RoamTask(), Priority: int.MinValue)], Fallback = new IdleTask() });
        Plan(new RoutineBlock { Version = uint.MaxValue, Was = "a.b-c_d" });
    }

    // ---- which window ---------------------------------------------------------------------------------------------------

    [Fact]
    public void A_window_is_end_exclusive_and_wraps_midnight_when_it_starts_after_it_ends()
    {
        var plan = Plan(new RoutineBlock { Windows = [new RoutineWindow(360, 720, new InspectTask()), new RoutineWindow(1320, 360, new SleepTask())] });
        Assert.Equal(-1, plan.Select(1319));         // one minute before 22:00
        Assert.Equal(1, plan.Select(1320));
        Assert.Equal(1, plan.Select(1439));
        Assert.Equal(1, plan.Select(0));
        Assert.Equal(1, plan.Select(359));
        Assert.Equal(0, plan.Select(360));           // the sleep window ends where the inspect window starts
        Assert.Equal(0, plan.Select(719));
        Assert.Equal(-1, plan.Select(720));
        Assert.Equal(RoutineKind.Vanilla, plan.TaskAt(plan.Select(720)));
        Assert.Equal(RoutineKind.Sleep, plan.TaskAt(plan.Select(0)));
    }

    [Fact]
    public void The_highest_priority_wins_and_equal_priorities_go_to_the_window_written_first()
    {
        var plan = Plan(Day);
        Assert.Equal(0, plan.Select(400));     // inspect (0) over the all-day sit (-1)
        Assert.Equal(2, plan.Select(800));     // only the all-day window
        Assert.Equal(1, plan.Select(100));     // sleep (5)
        var tie = Plan(new RoutineBlock { Windows = [new RoutineWindow(0, 600, new IdleTask()), new RoutineWindow(300, 900, new RoamTask()), new RoutineWindow(300, 900, new SitTask(), Priority: 1)] });
        Assert.Equal(0, tie.Select(200));
        Assert.Equal(2, tie.Select(300));      // the higher priority, though written last
        Assert.Equal(2, tie.Select(700));
        var equal = Plan(new RoutineBlock { Windows = [new RoutineWindow(300, 900, new RoamTask()), new RoutineWindow(0, 600, new IdleTask())] });
        Assert.Equal(0, equal.Select(400));    // both cover it, equal priority: the first written
        Assert.Equal(1, equal.Select(100));
        // the fallback is below every window, whatever its priority
        var low = Plan(new RoutineBlock { Windows = [new RoutineWindow(0, 0, new IdleTask(), Priority: int.MinValue, AllDay: true)], Fallback = new RoamTask() });
        Assert.Equal(0, low.Select(777));
    }

    [Fact]
    public void The_minute_of_the_day_rolls_over_with_the_game_time()
    {
        const long minute = 60_000_000_000, day = 1440 * minute;
        Assert.Equal(0, RoutinePlan.MinuteOf(0));
        Assert.Equal(0, RoutinePlan.MinuteOf(minute - 1));
        Assert.Equal(1, RoutinePlan.MinuteOf(minute));
        Assert.Equal(1439, RoutinePlan.MinuteOf(day - 1));
        Assert.Equal(0, RoutinePlan.MinuteOf(day));
        Assert.Equal(1320, RoutinePlan.MinuteOf(7 * day + 22 * 60 * minute + 59_999_999_999));
        Assert.Equal(1439, RoutinePlan.MinuteOf(-1));
        Assert.Equal(0, RoutinePlan.MinuteOf(-day));
        // a plan over midnight keeps its window through the rollover
        var plan = Plan(Day);
        Assert.Equal(1, plan.Select(RoutinePlan.MinuteOf(3 * day - 1)));
        Assert.Equal(1, plan.Select(RoutinePlan.MinuteOf(3 * day)));
    }

    // ---- identity ---------------------------------------------------------------------------------------------------------

    [Fact]
    public void A_plans_identity_is_its_mod_and_its_Was_or_name()
    {
        var mod = Ledger();
        var day = Define(mod, "day", Day);
        Assert.Equal("routine-test:day", RoutineRegistry.Find(day)!.Key);
        Assert.Equal(day.Guid, RoutineIdentity.Guid(Mod, "day"));

        // a rename with Was: another block name, the same identity, GUID and hash
        RoutineRegistry.Forget(Mod);
        var renamed = Define(Ledger(), "workday", Day with { Was = "day" });
        Assert.Equal("workday", renamed.Name);
        Assert.Equal(day.Guid, renamed.Guid);
        Assert.Equal(day.Id, renamed.Id);
        var def = RoutineRegistry.Find(renamed)!;
        Assert.Equal("routine-test:day", def.Key);
        Assert.Equal("workday", def.BlockName);
        Assert.Equal(GoldenHash, def.Plan.ContentHash);
        // the handle of before the rename still finds the definition: by its id, not by its name
        Assert.Same(def, RoutineRegistry.Find(day));
    }

    [Fact]
    public void Two_plans_of_a_mod_cannot_share_an_identity()
    {
        var mod = Ledger();
        Define(mod, "day", Day);
        var e = Assert.Throws<BlockError>(() => Compiler.Lower(mod, "night", new RoutineBlock { Was = "day" }));
        Assert.Equal("was", e.Field);
        Assert.Contains("routine-test:day", e.Problem);
        // and the other way round: a block named as another plan's Was
        Define(mod, "evening", new RoutineBlock { Was = "dusk" });
        Assert.Throws<BlockError>(() => Compiler.Lower(mod, "dusk", new RoutineBlock()));
        // the same block defined again replaces itself
        Define(mod, "day", Day with { Version = 3 });
        Assert.Equal(3u, RoutineRegistry.Find("routine-test:day")!.Plan.Version);
        // a block whose Was changes leaves its old identity free
        Define(mod, "evening", new RoutineBlock { Was = "sunset" });
        Assert.Null(RoutineRegistry.Find("routine-test:dusk"));
        Compiler.Lower(mod, "dusk", new RoutineBlock());
    }

    [Fact]
    public void A_handle_of_another_definition_or_a_forgotten_mod_finds_nothing()
    {
        var day = Define(Ledger(), "day", Day);
        var foreign = Define(new ModBlocks("routine-other", 1) { ManifestClients = "none" }, "day", Day);
        try
        {
            Assert.Equal("routine-other:day", RoutineRegistry.Find(foreign)!.Key);
            Assert.NotEqual(day.Guid, foreign.Guid);
            RoutineRegistry.Forget(Mod);
            Assert.Null(RoutineRegistry.Find(day));
            Assert.Null(RoutineRegistry.Find("routine-test:day"));
            Assert.NotNull(RoutineRegistry.Find(foreign));
        }
        finally
        {
            RoutineRegistry.Forget("routine-other");
        }
    }

    [Fact]
    public void A_mods_reset_forgets_its_plans()
    {
        var mod = Ledger();
        Define(mod, "day", Day);
        mod.Reset();
        Assert.Null(RoutineRegistry.Find("routine-test:day"));
    }

    // ---- an NPC block's routine --------------------------------------------------------------------------------------------

    private static ModBlocks NpcLedger() => new(Mod, 1) { ManifestClients = "mod" };

    [Fact]
    public void An_npcs_inline_routine_is_the_block_named_after_it_and_keeps_its_identity_through_a_rename()
    {
        var mod = NpcLedger();
        var (_, low, _) = Compiler.Lower(mod, "tobin", new NpcBlock { Name = "Tobin", Body = Bodies.Fisher, Routine = Day });
        Assert.Equal(GoldenWire.Replace("routine-test:day", "routine-test:tobin.routine"), low.Json["npcRoutine"]!.ToJsonString());
        Assert.NotNull(low.Json["npc"]);
        Assert.Null(RoutineRegistry.Find("routine-test:tobin.routine"));                 // nothing before the commit
        // a renamed NPC keeps the plan's identity; the block is named after the NPC's name of now
        var renamed = Compiler.Lower(mod, "tobin-the-fisher", new NpcBlock { Name = "Tobin", Was = "tobin", Body = Bodies.Fisher, Routine = Day }).Low.Json["npcRoutine"]!;
        Assert.Equal("routine-test:tobin.routine", (string?)renamed["key"]);
        // a Was of the plan's own wins
        var own = Compiler.Lower(mod, "tobin", new NpcBlock { Name = "Tobin", Body = Bodies.Fisher, Routine = Day with { Was = "tobins-day" } }).Low.Json["npcRoutine"]!;
        Assert.Equal("routine-test:tobins-day", (string?)own["key"]);
        // what is wrong with the inline plan names the NPC's field
        var e = Assert.Throws<BlockError>(() => Compiler.Lower(mod, "tobin", new NpcBlock { Name = "Tobin", Body = Bodies.Fisher, Routine = new RoutineBlock { Fallback = new SleepTask() } }));
        Assert.Equal(("npc tobin", "routine.fallback"), (e.Block, e.Field));
        // an NPC without a routine lowers as before
        Assert.Null(Compiler.Lower(mod, "tobin", new NpcBlock { Name = "Tobin", Body = Bodies.Fisher }).Low.Json["npcRoutine"]);
    }

    [Fact]
    public void An_npc_names_a_routine_of_its_own_mod_by_name_or_handle()
    {
        var mod = NpcLedger();
        var day = Define(mod, "workday", Day with { Was = "day" });
        foreach (var routine in new PartRef<RoutineBlock>[] { "workday", "routine-test:workday", day })
            Assert.Equal("routine-test:day", (string?)Compiler.Lower(mod, "tobin", new NpcBlock { Name = "Tobin", Body = Bodies.Fisher, Routine = routine }).Low.Json["npcRoutine"]);
        var e = Assert.Throws<BlockError>(() => Compiler.Lower(mod, "tobin", new NpcBlock { Name = "Tobin", Body = Bodies.Fisher, Routine = "other-mod:day" }));
        Assert.Equal("routine", e.Field);
        Assert.Contains("belongs to the mod that assigns it", e.Problem);
        e = Assert.Throws<BlockError>(() => Compiler.Lower(mod, "tobin", new NpcBlock { Name = "Tobin", Body = Bodies.Fisher, Routine = "nights" }));
        Assert.Contains("no routine \"nights\"", e.Problem);
        Assert.Contains("workday", e.Problem);
        mod.Handles["ilse"] = Compiler.Lower(mod, "ilse", new NpcBlock { Name = "Ilse", Body = Bodies.Villager }).Handle;
        e = Assert.Throws<BlockError>(() => Compiler.Lower(mod, "tobin", new NpcBlock { Name = "Tobin", Body = Bodies.Fisher, Routine = "ilse" }));
        Assert.Contains("is a npc, not a routine", e.Problem);
        // in a data file: a name, or the plan written there
        var named = BlockJson.Parse<NpcBlock>("""{ "name": "Tobin", "body": "fisher", "routine": "workday" }""");
        Assert.Equal("workday", named.Routine!.Name);
        var inline = BlockJson.Parse<NpcBlock>("""{ "name": "Tobin", "body": "fisher", "routine": { "windows": [ { "allDay": true, "task": { "kind": "sit" } } ] } }""");
        Assert.IsType<SitTask>(inline.Routine!.Inline!.Windows[0].Task);
    }

    [Fact]
    public void An_npcs_commit_publishes_its_inline_plan_and_sets_or_clears_its_link()
    {
        var mod = NpcLedger();
        var made = RoutineCompiler.Define(new BlockContext(mod, "npc tobin"), new Lowered(mod), "tobin.routine", Day, "tobin.routine", "routine");
        RoutineNpc.Bind(mod, "tobin", made, made.GameName);
        Assert.Same(made, mod.Handles["tobin.routine"]);
        Assert.Equal("routine-test:tobin.routine", RoutineRegistry.OfNpc(Mod, "tobin"));
        RoutineNpc.Bind(mod, "tobin", null, "routine-test:day");
        Assert.Equal("routine-test:day", RoutineRegistry.OfNpc(Mod, "tobin"));
        RoutineNpc.Bind(mod, "tobin", null, null);
        Assert.Null(RoutineRegistry.OfNpc(Mod, "tobin"));
    }

    // ---- what a world keeps -------------------------------------------------------------------------------------------------

    [Fact]
    public void The_book_keeps_what_an_assignment_needs_and_reads_back()
    {
        var plan = Plan(Day);
        var book = new RoutineBook { World = "world-1" };
        Assert.True(book.Record(0x1234, Mod, plan, manualSuspended: true, revision: book.NextRevision()));
        var text = JsonSerializer.Serialize(book);
        // never an entity, a pointer, a ticket or a task
        var row = JsonNode.Parse(text)!["Assignments"]![0]!.AsObject();
        Assert.Equal(["ContentHash", "Key", "ManualSuspended", "NpcId", "Owner", "PlanVersion", "Revision"], row.Select(p => p.Key).Order(StringComparer.Ordinal).ToArray());
        var back = JsonSerializer.Deserialize<RoutineBook>(text)!;
        Assert.Equal("world-1", back.World);
        Assert.Equal(1ul, back.LastRevision);
        var saved = Assert.Single(back.Assignments);
        Assert.Equal((0x1234u, Mod, "routine-test:day", 2u, GoldenHash, true, 1ul), (saved.NpcId, saved.Owner, saved.Key, saved.PlanVersion, saved.ContentHash, saved.ManualSuspended, saved.Revision));
    }

    [Fact]
    public void A_stale_result_cannot_put_an_earlier_state_back()
    {
        var (v2, v3) = (Plan(Day), Plan(Day with { Version = 3 }));
        var book = new RoutineBook();
        Assert.True(book.Record(7, Mod, v2, false, 1));
        Assert.True(book.Record(7, Mod, v3, false, 3));
        Assert.False(book.Record(7, Mod, v2, true, 2));           // the callback of revision 2 arrives late
        Assert.Equal((3u, false, 3ul), (book.Find(7)!.PlanVersion, book.Find(7)!.ManualSuspended, book.Find(7)!.Revision));
        Assert.False(book.Remove(7, 2));
        Assert.NotNull(book.Find(7));
        Assert.True(book.Remove(7, 4));
        Assert.Null(book.Find(7));
        Assert.False(book.Record(7, Mod, v3, false, 3));          // older than the removal: it stays removed
        Assert.False(book.Remove(7, 9));                          // nothing to remove
        Assert.True(book.Record(7, Mod, v2, false, 5));           // assigned again later
        Assert.Equal(5ul, book.LastRevision);
        Assert.Equal(6ul, book.NextRevision());
    }

    private static SavedRoutine Saved(RoutinePlan plan, bool suspended = false) =>
        new() { NpcId = 7, Owner = Mod, Key = plan.Key, PlanVersion = plan.Version, ContentHash = plan.ContentHash, ManualSuspended = suspended, Revision = 4 };

    [Fact]
    public void A_restart_decides_each_saved_assignment_by_version_and_hash()
    {
        var saved = Saved(Plan(Day));
        // same version, same content: bound again, no new revision, nothing announced
        var rebind = RoutineStore.Decide(saved, true, Plan(Day));
        Assert.Equal((RoutineRestore.Rebind, RoutineAssignmentState.PendingSpawn, false, false, null), (rebind.Action, rebind.State, rebind.NewRevision, rebind.Announced, rebind.Reason));
        Assert.Equal(GoldenHash, rebind.Plan!.ContentHash);
        // a higher version: follows it
        var newer = Plan(Day with { Version = 3, Fallback = new IdleTask() });
        var migrate = RoutineStore.Decide(saved, true, newer);
        Assert.Equal((RoutineRestore.Migrate, true, true, "Migrated"), (migrate.Action, migrate.NewRevision, migrate.Announced, migrate.Reason));
        Assert.Same(newer, migrate.Plan);
        // same version, other content: refused, stays saved and suspended, and no plan runs
        var changed = RoutineStore.Decide(saved, true, Plan(Day with { Fallback = new IdleTask() }));
        Assert.Equal((RoutineRestore.VersionNotBumped, RoutineAssignmentState.VersionMismatch, false, true, "VersionNotBumped"), (changed.Action, changed.State, changed.NewRevision, changed.Announced, changed.Reason));
        Assert.Null(changed.Plan);
        // a lower version: refused the same way
        var down = RoutineStore.Decide(saved, true, Plan(Day with { Version = 1 }));
        Assert.Equal((RoutineRestore.Downgrade, RoutineAssignmentState.VersionMismatch, false, "Downgrade"), (down.Action, down.State, down.NewRevision, down.Reason));
        Assert.Null(down.Plan);
        // the owner is not loaded, or the plan is not defined: removed
        var gone = RoutineStore.Decide(saved, false, Plan(Day));
        Assert.Equal((RoutineRestore.Remove, RoutineAssignmentState.Removed, true, "OwnerMissing"), (gone.Action, gone.State, gone.NewRevision, gone.Reason));
        Assert.Null(gone.Plan);
        Assert.Equal("DefinitionMissing", RoutineStore.Decide(saved, true, null).Reason);
        // a plan of another key cannot stand in
        Assert.Equal(RoutineRestore.Remove, RoutineStore.Decide(saved, true, Plan(Day, "night")).Action);
    }

    [Fact]
    public void A_restart_applies_its_decisions_to_the_book()
    {
        var day = Plan(Day);
        var night = Plan(new RoutineBlock { Fallback = new IdleTask() }, "night");
        var book = new RoutineBook { World = "world-1" };
        book.Record(1, Mod, day, false, book.NextRevision());                  // rebinds
        book.Record(2, Mod, night, true, book.NextRevision());                 // migrates to version 2
        book.Record(3, "gone-mod", RoutinePlan.Make("gone-mod:day", 1, [], RoutineKind.Vanilla), false, book.NextRevision());   // its owner is gone
        book.Record(4, Mod, Plan(new RoutineBlock(), "old"), false, book.NextRevision());   // no longer defined
        book.Record(5, Mod, Plan(new RoutineBlock(), "edited"), false, book.NextRevision()); // edited without a version
        var now = new Dictionary<string, RoutinePlan>
        {
            [day.Key] = day,
            [night.Key] = Plan(new RoutineBlock { Version = 2, Fallback = new RoamTask() }, "night"),
            ["routine-test:edited"] = Plan(new RoutineBlock { Fallback = new RoamTask() }, "edited"),
        };
        var results = RoutineStore.Restore(book, "world-1", owner => owner == Mod, key => now.GetValueOrDefault(key));
        Assert.Equal([RoutineRestore.Rebind, RoutineRestore.Migrate, RoutineRestore.Remove, RoutineRestore.Remove, RoutineRestore.VersionNotBumped], results.Select(r => r.Decision.Action));
        Assert.Equal(["OwnerMissing", "DefinitionMissing"], results.Where(r => r.Decision.Action == RoutineRestore.Remove).Select(r => r.Decision.Reason));
        Assert.Equal([1u, 2u, 5u], book.Assignments.Select(a => a.NpcId));
        Assert.Equal(1ul, book.Find(1)!.Revision);                              // the same revision
        var migrated = book.Find(2)!;
        Assert.Equal((2u, now[night.Key].ContentHash, true), (migrated.PlanVersion, migrated.ContentHash, migrated.ManualSuspended));
        Assert.True(migrated.Revision > 5);                                     // a new one, above every saved one
        Assert.Equal(1u, results[1].Saved.PlanVersion);                         // what was saved, for the event's Before
        var kept = book.Find(5)!;
        Assert.Equal((1u, 5ul), (kept.PlanVersion, kept.Revision));             // refused: never silently changed
        Assert.Equal(8ul, book.LastRevision);                                   // 5 saved, one migration, two removals

        // a second restart with the same definitions changes nothing
        var again = RoutineStore.Restore(book, "world-1", owner => owner == Mod, key => now.GetValueOrDefault(key));
        Assert.Equal([RoutineRestore.Rebind, RoutineRestore.Rebind, RoutineRestore.VersionNotBumped], again.Select(r => r.Decision.Action));
        Assert.Equal(8ul, book.LastRevision);

        // a book of another world keeps nothing
        var other = RoutineStore.Restore(book, "world-2", _ => true, key => now.GetValueOrDefault(key));
        Assert.All(other, r => Assert.Equal("OtherWorld", r.Decision.Reason));
        Assert.Empty(book.Assignments);
        Assert.Equal("world-2", book.World);
    }

    // ---- capability rows ------------------------------------------------------------------------------------------------------

    [Fact]
    public void The_rows_of_later_versions_are_off_and_decisions_need_the_service_and_the_feature()
    {
        var later = BlockCapabilities.RoutineLaterRow("x");
        Assert.Equal((CapabilityState.Unavailable, CapabilityReason.Policy), (later.State, later.Reason));
        Assert.True(BlockCapabilities.RoutinesOn(service: true, feature: true));
        Assert.False(BlockCapabilities.RoutinesOn(service: false, feature: true));
        Assert.False(BlockCapabilities.RoutinesOn(service: true, feature: false));
        Assert.Equal(("routines.decisions", "routines.anchors", "routines.jobs"), (BlockCapabilities.RoutineDecisions, BlockCapabilities.RoutineAnchors, BlockCapabilities.RoutineJobs));
    }
}
