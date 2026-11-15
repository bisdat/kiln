// SPDX-License-Identifier: MIT
using System.Text.Json;
using System.Text.Json.Nodes;
using Xunit;
namespace Kiln;
[Collection("entity identity")]
public sealed class StrictEntityIdentityTests
{
    private static readonly Guid Chest = Guid.Parse("00000002-0000-0000-0000-000000000002");
    private static EntityId Place(uint id, double x = 42, long? generation = null) {
        var e = new EntityId(id);
        TestWorld.World[id] = new SpawnLink.Ent(e, Chest, null, new WorldPosition(x,0,0),0,generation);
        return e;
    }
    private static EntityTable Load() {
        var t = new EntityTable(true);
        t.Absorb("kiln:world", JsonNode.Parse("""{"entities":[{"template":"CHEST","pos":[42,0,0],"keys":{"transform-scale":{"mod":"m","scale":{"x":2,"y":2,"z":2},"base":{"x":1,"y":1,"z":1}}}}] }""".Replace("CHEST", Chest.ToString()))!.AsObject());
        t.StartMatching(); return t;
    }
    public StrictEntityIdentityTests() { TestWorld.Reset(); TestWorld.Templates[Chest] = new(3,9,"Chest"); }
    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public void Ambiguity_is_sticky_across_removal_and_save(bool periodic) {
        var a=Place(1); Place(2,42.2); var t=Load(); if (periodic) t.Tick();
        Assert.Null(t.FindOrMatch(a)); Assert.Equal(1,t.UnresolvedCount);
        TestWorld.World.Remove(2); Assert.Null(t.FindOrMatch(a));
        var next=new EntityTable(true);
        foreach(var (scope,data) in t.Stage(new Dictionary<string,JsonSerializerOptions>())) next.Absorb(scope,JsonNode.Parse(data)!.AsObject());
        next.StartMatching(); next.Tick(); Assert.Null(next.FindOrMatch(a));
    }
    [Fact] public void Unique_restart_still_matches() { var a=Place(1); var t=Load(); t.Tick(); Assert.NotNull(t.FindOrMatch(a)); }
    [Fact] public void Strict_replacement_is_dropped_instead_of_claiming_the_id() {
        var a=Place(1); var t=new EntityTable(true); var r=t.Attach(a);
        r.ByMod["kiln:world"]=new(){["transform-scale"]=new WorldStore.Slot{Json="{}"}};
        TestWorld.World.Remove(1); Place(1, generation: 2); t.Tick();
        Assert.Null(t.FindOrMatch(a)); Assert.Empty(t.Stage(new Dictionary<string,JsonSerializerOptions>()));
    }
    [Fact]
    public void Strict_scope_does_not_change_the_other_attachments_on_the_same_saved_entity()
    {
        const string scope = "kiln:strict-test";
        ServiceEntityData.UseStrict(scope);
        var parent = new EntityTable();
        var doc = JsonNode.Parse("""{"entities":[{"template":"CHEST","pos":[42,0,0],"keys":{"note":1}}]}""".Replace("CHEST", Chest.ToString()))!.AsObject();
        parent.Absorb(scope, doc);
        parent.Absorb("ordinary-mod", doc);
        var a = Place(2001);
        Place(2002, 42.2);
        parent.StartMatching();
        parent.Tick();
        Assert.NotNull(parent.FindOrMatch(a));
        Assert.Null(parent.Strict(scope).FindOrMatch(a));
        Assert.Single(parent.Find(a)!.ByMod);
        Assert.True(parent.Find(a)!.ByMod.ContainsKey("ordinary-mod"));
        Assert.Equal(2, parent.Stage(new Dictionary<string, JsonSerializerOptions>()).Count());
    }

    [Fact]
    public void Unique_strict_restart_hands_over_the_value_once_and_binds_the_generation()
    {
        const string scope = "kiln:strict-single-test";
        ServiceEntityData.UseStrict(scope);
        var parent = new EntityTable();
        var doc = JsonNode.Parse("""{"entities":[{"template":"CHEST","pos":[42,0,0],"keys":{"note":1}}]}""".Replace("CHEST", Chest.ToString()))!.AsObject();
        var delivered = new List<EntityId>();
        ServiceEntityData.Register(scope, new ServiceEntityData.Consumer { Loaded = (e, _) => delivered.Add(e) });
        parent.Absorb(scope, doc);
        var a = Place(2001);
        parent.StartMatching(); parent.Tick(); parent.HandOverServiceValues(); parent.HandOverServiceValues();
        Assert.Equal(a, Assert.Single(delivered));
        var strict = parent.Strict(scope);
        Assert.True(strict.SameGeneration(strict.Find(a)!, a));
        Place(2001, generation: 2);
        Assert.False(strict.SameGeneration(strict.Find(a)!, a));
    }

    [Fact]
    public void Absent_generations_use_unique_positional_identity()
    {
        var a = Place(2001, generation: null);
        var t = new EntityTable(true);
        var record = t.Attach(a);
        Assert.True(t.SameGeneration(record, a));
        Assert.Null(record.WaitingReason);
    }

    [Theory]
    [InlineData(null, 2L)]
    [InlineData(1L, 2L)]
    [InlineData(1L, null)]
    public void Generation_change_before_periodic_check_drops_record(long? before, long? after)
    {
        var parent = new EntityTable();
        var store = new ServiceEntities(parent);
        const string scope = "kiln:strict-generation";
        ServiceEntityData.UseStrict(scope);
        var e = Place(2001, generation: before);
        Assert.True(store.Set(e, scope, "scale", "1"));
        Place(2001, generation: after);
        Assert.False(store.Holds(e, scope, "scale"));
        Assert.Empty(store.Replays(scope, "scale"));
        Assert.Empty(parent.Stage(new Dictionary<string, JsonSerializerOptions>()));
    }

    [Fact]
    public void Missing_generations_recheck_uniqueness_at_use()
    {
        var parent = new EntityTable(); var store = new ServiceEntities(parent);
        const string scope = "kiln:strict-position"; ServiceEntityData.UseStrict(scope);
        var e = Place(2001);
        Assert.True(store.Set(e, scope, "scale", "1"));
        Assert.True(store.Holds(e, scope, "scale"));
        Place(2002, 42.2);
        Assert.False(store.Holds(e, scope, "scale"));
        Assert.Contains("two entities", Assert.Single(store.Replays(scope, "scale")).Reason);
    }

    [Fact]
    public void Destroyed_scaled_entities_leave_no_records_or_warnings()
    {
        var parent = new EntityTable(); var store = new ServiceEntities(parent);
        const string scope = "kiln:strict-dead"; ServiceEntityData.UseStrict(scope);
        for (uint id = 2001; id < 2101; id++) Assert.True(store.Set(Place(id, id, id), scope, "scale", "1"));
        TestWorld.World.Clear(); parent.Tick();
        Assert.Equal(0, parent.LiveCount); Assert.Equal(0, parent.UnresolvedCount);
        Assert.Empty(store.Replays(scope, "scale"));
        Assert.Empty(parent.Stage(new Dictionary<string, JsonSerializerOptions>()));
    }

    [Fact]
    public void Ambiguity_expires_after_three_missed_sessions()
    {
        var a = Place(2001); Place(2002, 42.2); var t = Load();
        for (var session = 1; session <= 3; session++)
        {
            t.Tick(); Assert.Null(t.FindOrMatch(a));
            typeof(EntityTable).GetField("_matchUntil", System.Reflection.BindingFlags.Instance | System.Reflection.BindingFlags.NonPublic)!.SetValue(t, DateTime.UtcNow.AddSeconds(-1));
            t.Tick();
            var staged = t.Stage(new Dictionary<string, JsonSerializerOptions>()).ToList();
            if (session == 3) { Assert.Empty(staged); Assert.Equal(0, t.UnresolvedCount); }
            else {
                t = new EntityTable(true);
                foreach (var (scope, data) in staged) t.Absorb(scope, JsonNode.Parse(data)!.AsObject());
                t.StartMatching();
            }
        }
    }

    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public void Removal_does_not_create_records_or_throw_for_absent_entities(bool exists)
    {
        const string scope = "kiln:strict-remove"; ServiceEntityData.UseStrict(scope);
        var parent = new EntityTable(); var store = new ServiceEntities(parent);
        var e = exists ? Place(2001) : new EntityId(2001);
        Assert.Equal(exists, store.Set(e, scope, "scale", null));
        Assert.Equal(0, parent.LiveCount); Assert.Equal(0, parent.UnresolvedCount);
    }

    [Fact]
    public void Removal_can_clear_ambiguous_waiting_record_without_claiming_it()
    {
        const string scope = "kiln:world"; ServiceEntityData.UseStrict(scope);
        var parent = new EntityTable(); var store = new ServiceEntities(parent);
        var e = Place(2001); Place(2002, 42.2);
        parent.Absorb(scope, JsonNode.Parse("""{"entities":[{"template":"CHEST","pos":[42,0,0],"keys":{"transform-scale":1}}]}""".Replace("CHEST", Chest.ToString()))!.AsObject());
        parent.StartMatching(); parent.Tick();
        Assert.True(store.Set(e, scope, "transform-scale", null));
        Assert.Equal(0, parent.LiveCount);
        Assert.Empty(store.Replays(scope, "transform-scale"));
        Assert.Empty(parent.Stage(new Dictionary<string, JsonSerializerOptions>()));
    }

    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public void Late_opt_in_verifies_previously_lenient_claim_at_its_saved_place(bool neighbour)
    {
        var parent = new EntityTable();
        const string scope = "kiln:late-claim";
        parent.Absorb(scope, JsonNode.Parse("""{"entities":[{"template":"CHEST","pos":[42,0,0],"keys":{"scale":1}}]}""".Replace("CHEST", Chest.ToString()))!.AsObject());
        var e = Place(2001, 42.3);
        parent.StartMatching(); parent.Tick(); Assert.NotNull(parent.Find(e));
        // Within the recorded radius, outside the radius centred on the chosen candidate.
        if (neighbour) Place(2002, 41.6);
        var strict = parent.Strict(scope);
        if (neighbour) { Assert.Null(strict.FindOrMatch(e)); Assert.Contains("two entities", Assert.Single(strict.Records).WaitingReason); }
        else { Assert.NotNull(strict.FindOrMatch(e)); Assert.True(strict.SameGeneration(strict.Find(e)!, e)); }
    }

    [Fact]
    public void Late_registration_delivers_a_revalidated_legacy_claim_once()
    {
        const string scope = "kiln:late-delivery";
        var parent = new EntityTable();
        parent.Absorb(scope, JsonNode.Parse("""{"entities":[{"template":"CHEST","pos":[42,0,0],"keys":{"scale":1}}]}""".Replace("CHEST", Chest.ToString()))!.AsObject());
        var e = Place(2001); parent.StartMatching(); parent.Tick();
        var previous = ServiceEntityData.Store;
        var delivered = new List<EntityId>();
        try
        {
            ServiceEntityData.Store = new ServiceEntities(parent);
            ServiceEntityData.UseStrict(scope);
            ServiceEntityData.Register(scope, new ServiceEntityData.Consumer { Loaded = (id, _) => delivered.Add(id) });
            parent.HandOverServiceValues();
            Assert.Equal(e, Assert.Single(delivered));
        }
        finally { ServiceEntityData.Store = previous; }
    }

    [Fact]
    public void Changed_template_drops_the_live_record()
    {
        const string scope = "kiln:strict-template";
        ServiceEntityData.UseStrict(scope);
        var parent = new EntityTable(); var store = new ServiceEntities(parent);
        var e = Place(2001);
        Assert.True(store.Set(e, scope, "scale", "1"));
        TestWorld.World[e.Value] = TestWorld.World[e.Value] with { Template = Guid.NewGuid() };
        Assert.False(store.Holds(e, scope, "scale"));
        Assert.Empty(store.Replays(scope, "scale"));
        Assert.Empty(parent.Stage(new Dictionary<string, JsonSerializerOptions>()));
    }

    [Fact]
    public void Late_opt_in_failed_search_retains_the_claim_for_retry()
    {
        const string scope = "kiln:late-unavailable";
        var parent = new EntityTable();
        parent.Absorb(scope, JsonNode.Parse("""{"entities":[{"template":"CHEST","pos":[42,0,0],"keys":{"scale":1}}]}""".Replace("CHEST", Chest.ToString()))!.AsObject());
        var e = Place(2001); parent.StartMatching(); parent.Tick();
        TestWorld.FindResult = 6;
        parent.Strict(scope);
        TestWorld.FindResult = 0;
        var strict = parent.Strict(scope);
        var r = Assert.Single(strict.Records);
        Assert.True(strict.HoldsStrict(r, e));
        Assert.Equal("1", r.ByMod[scope]["scale"].Json);
    }

}
