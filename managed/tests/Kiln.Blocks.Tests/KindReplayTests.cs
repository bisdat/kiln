// SPDX-License-Identifier: MIT
// An applied plan in a later content generation (16d review R2; chapter 16d: "on a new world, the host replays the already
// frozen products using the built-in compilers against the new world's snapshot, never calling the provider; it publishes a
// new application generation only after success. Old-world callbacks carry the old world/request token and are discarded").
// The frozen products are lowered again, the plan's parts at content ready run again, and a failure refuses the plan and
// revokes its owner as a failure at the first content ready does. Through the fake ready host.
using Xunit;

namespace Kiln.BlockTests;

[Collection("block kinds")]
public sealed class KindReplayTests
{
    private const string Missing = "0xdeadbeef";

    private static KindApplication Submit(ModBlocks target, KindOwner consumer, BlockPlan plan) =>
        KindApply.Submit(target, consumer, plan, stage => KindDefine.HandleOf(target.Id, plan, stage));

    private static (KindApplication App, LanternKind Kind, ModBlocks Target, KindOwner Consumer) Camp(string mod)
    {
        var (entry, kind, _) = KindKit.Lantern();
        var consumer = KindKit.Owner(mod);
        var target = KindKit.Mod(mod);
        var plan = KindCompiler.Compile(consumer, entry, "camp", new LanternBlock { Name = "Camp", Seconds = 600 }, KindApply.Provisional(consumer));
        return (Submit(target, consumer, plan), kind, target, consumer);
    }

    /// <summary>A consumable whose effect applies a game status by name: a late program, finished with the game's value.</summary>
    private static (KindApplication App, ModBlocks Target) Potion(string mod)
    {
        var (entry, kind, _) = KindKit.Lantern();
        kind.Body = (c, b) => c.Plan("p", [c.Item("p", new ItemBlock
        {
            Name = b.Name, Kind = ItemKind.Consumable,
            OnUse = [new EffectBlock { On = EffectOn.Caster, Applies = (StatusRef)Missing }],
        })]);
        var consumer = KindKit.Owner(mod);
        var target = KindKit.Mod(mod);
        var plan = KindCompiler.Compile(consumer, entry, "potion", new LanternBlock { Name = "Potion" }, KindApply.Provisional(consumer));
        return (Submit(target, consumer, plan), target);
    }

    [Fact]
    public void A_new_content_generation_lowers_the_frozen_plan_again_and_publishes_a_new_generation_after_its_replay()
    {
        using var w = new KindWorld();
        var (app, kind, target, _) = Camp("replay-ok");
        Assert.Equal(BlockApplyState.Applied, app.State);
        var first = app.Result;
        var (calls, writes) = (kind.Calls, w.Sink.Writes.Count);

        w.Generation = 2;
        KindApply.PumpAll();
        // lowered again and waiting for its replay: what it answers stays as it was until the replay is done
        Assert.Equal(first, app.Result);
        Assert.Single(w.Changes);
        w.Ready.Tick();
        var after = app.Result;
        Assert.Equal((BlockApplyState.Applied, true, 2ul), (after.State, after.Ok, after.ContentGeneration));
        Assert.True(after.Generation > first.Generation);
        var change = w.Changes.Last();
        Assert.Equal((first, after), (change.Before, change.After));
        // the provider is never asked, and the same lowering registers nothing again
        Assert.Equal(calls, kind.Calls);
        Assert.Equal(writes, w.Sink.Writes.Count);
        Assert.Single(w.Created);
        Assert.Empty(w.Revoked);
        Assert.Same(app.Handle, target.Handles["camp"]);
        // nothing more for the same generation
        KindApply.PumpAll();
        w.Ready.Tick();
        Assert.Equal(after, app.Result);
        Assert.Equal(2, w.Changes.Count());
    }

    [Fact]
    public void A_rule_the_new_content_refuses_refuses_the_plan_revokes_its_owner_and_unpublishes_it()
    {
        using var w = new KindWorld();
        var (app, _, target, _) = Camp("replay-rule");
        var owner = app.Owner;
        Assert.Equal(BlockApplyState.Applied, app.State);
        w.Ready.RuleErrors = json => json.Contains("replay-rule.kind-camp.light", StringComparison.Ordinal)
            ? ["items replay-rule.kind-camp.light: the game has no field 'maxStack'"] : [];
        w.Generation = 2;
        w.Ready.Apply();       // the game replays the rules of the new content first: the error alone changes nothing yet
        KindApply.PumpAll();
        w.Ready.Tick();
        Assert.Equal(BlockApplyState.Refused, app.State);
        Assert.Equal("ContentReadyFailed: items replay-rule.kind-camp.light: the game has no field 'maxStack'", app.Result.Reason);
        Assert.Equal([owner], w.Revoked);
        Assert.True(app.Stage.Ledger.Revoked);
        Assert.False(target.Handles.ContainsKey("camp"));
        Assert.False(target.Lowered.ContainsKey("camp"));
        var change = w.Changes.Last();
        Assert.Equal((BlockApplyState.Applied, BlockApplyState.Refused), (change.Before.State, change.After.State));
        Assert.Equal(change.After, KindApply.Of(app.Plan));
        // a refused plan is not lowered again by a later generation
        w.Generation = 3;
        KindApply.PumpAll();
        Assert.Single(w.Revoked);
    }

    [Fact]
    public void A_game_status_the_new_content_does_not_have_refuses_the_plan()
    {
        using var w = new KindWorld();
        var (app, target) = Potion("replay-missing");
        Assert.Equal(BlockApplyState.Applied, app.State);
        var owner = app.Owner;
        w.Ready.Missing.Add(Missing);
        w.Generation = 2;
        KindApply.PumpAll();
        Assert.Equal(BlockApplyState.Refused, app.State);
        Assert.StartsWith("ContentReadyFailed: block ", app.Result.Reason);
        Assert.Contains($"the game has no status \"{Missing}\"", app.Result.Reason);
        Assert.Equal([owner], w.Revoked);
        Assert.False(target.Handles.ContainsKey("potion"));
        Assert.Empty(target.Programs);
    }

    [Fact]
    public void A_plan_that_lowers_differently_against_the_new_content_is_registered_again_under_an_owner_of_its_own()
    {
        using var w = new KindWorld();
        var (app, target) = Potion("replay-differs");
        var (old, program) = Assert.Single(w.Ready.Programs);
        var oldBlob = target.Programs[program].Blob;
        var first = app.Result;
        // the game's status is another GUID in the new content: the finished program differs
        w.Ready.Resolved = new Guid("0c0c0c0c-0000-4000-8000-00000000cafe");
        w.Generation = 2;
        KindApply.PumpAll();
        Assert.Equal([old], w.Revoked);              // the old registrations came off first ...
        Assert.Equal(2, w.Created.Count);            // ... and the new lowering went in under a new owner
        var owner = w.Created[1];
        Assert.Equal(owner, app.Owner);
        Assert.Equal((owner, program), w.Ready.Programs[^1]);
        Assert.Contains(w.Sink.Writes, x => x.StartsWith($"item {owner} replay-differs.kind-potion.p"));
        var after = app.Result;
        Assert.Equal((BlockApplyState.Applied, 2ul), (after.State, after.ContentGeneration));
        Assert.True(after.Generation > first.Generation);
        Assert.NotEqual(oldBlob, target.Programs[program].Blob);
        Assert.Same(app.Handle, target.Handles["potion"]);
    }

    [Fact]
    public void The_check_of_an_older_pass_or_an_overtaken_generation_is_discarded()
    {
        using var w = new KindWorld();
        w.Ready.Deferred = true;
        var (app, _, _, _) = Camp("replay-token");
        w.Ready.RunReady();
        w.Ready.RunLater();
        Assert.Equal((BlockApplyState.Applied, 1ul), (app.State, app.Result.ContentGeneration));

        w.Generation = 2;
        w.Ready.Apply();       // the game's replay of generation 2 starts before Kiln's tick looked at the plan
        KindApply.PumpAll();
        w.Ready.RunLater();    // that pass's check began before the plan was lowered again: not its replay
        Assert.Equal(1ul, app.Result.ContentGeneration);
        w.Generation = 3;      // and a newer generation comes before the replay of 2 was done
        KindApply.PumpAll();
        w.Ready.Tick();
        w.Ready.RunLater();
        Assert.Equal((BlockApplyState.Applied, 3ul), (app.State, app.Result.ContentGeneration));
        Assert.Equal(2, w.Changes.Count());   // PendingContent -> Applied, then one new generation: none for the overtaken one
        Assert.Single(w.Created);
    }

    [Fact]
    public void A_frozen_product_that_no_longer_lowers_refuses_the_plan()
    {
        using var w = new KindWorld();
        var (app, _, target, _) = Camp("replay-relower");
        // a data file of the consumer declares a block under a product's generated name: the plan cannot be lowered again
        target.Pending["kind-camp.light"] = () => throw new Exception("must not compile the authored block");
        w.Generation = 2;
        KindApply.PumpAll();
        Assert.Equal(BlockApplyState.Refused, app.State);
        Assert.StartsWith("RelowerFailed: ", app.Result.Reason);
        Assert.Equal(w.Created, w.Revoked);
        Assert.False(target.Handles.ContainsKey("camp"));
    }

    [Fact]
    public void A_mod_that_unloads_while_its_plan_replays_takes_it_with_it()
    {
        using var w = new KindWorld();
        w.Ready.Deferred = true;
        var (app, _, target, consumer) = Camp("replay-retire");
        w.Ready.RunReady();
        w.Ready.RunLater();
        w.Generation = 2;
        KindApply.PumpAll();
        KindApply.RetireConsumer(consumer.Token);
        Assert.Equal(BlockApplyState.Removed, app.State);
        w.Ready.Tick();
        w.Ready.Apply();
        w.Ready.RunLater();
        Assert.Equal(BlockApplyState.Removed, app.State);
        Assert.False(target.Handles.ContainsKey("camp"));
        Assert.Equal(w.Created, w.Revoked);
    }
}
