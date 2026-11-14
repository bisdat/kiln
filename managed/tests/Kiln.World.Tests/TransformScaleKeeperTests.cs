// SPDX-License-Identifier: MIT
// A transform scale survives a save and restart (0.2.0-dev.28 fix round): what Kiln.World keeps beside the save
// (kiln.saves' entity attachments, scope kiln:world) for each committed SetTransformScale. A fake store stands in for
// kiln.saves; the restart itself is proven in the lab (world-writes-test, PASS world.scale-persist).
using Xunit;

namespace Kiln.WorldTests;

[Collection("kiln.world backend")]
public sealed class TransformScaleKeeperTests : IDisposable
{
    private sealed class FakeStore : ServiceEntityData.IStore
    {
        public readonly Dictionary<(uint, string, string), string> Data = new();
        public readonly HashSet<uint> SessionOnly = [];

        public bool Set(EntityId entity, string scope, string key, string? json)
        {
            if (SessionOnly.Contains(entity.Value)) return false;
            if (json is null) Data.Remove((entity.Value, scope, key));
            else Data[(entity.Value, scope, key)] = json;
            return true;
        }

        public string? Get(EntityId entity, string scope, string key) => Data.GetValueOrDefault((entity.Value, scope, key));
        public IReadOnlyList<EntityId> Entities(string scope) => [.. Data.Keys.Where(k => k.Item2 == scope).Select(k => new EntityId(k.Item1)).Distinct()];
        public IReadOnlyDictionary<string, string> Values(EntityId entity, string scope) =>
            Data.Where(p => p.Key.Item1 == entity.Value && p.Key.Item2 == scope).ToDictionary(p => p.Key.Item3, p => p.Value);
    }

    private readonly FakeStore _store = new();
    private readonly ServiceEntityData.IStore? _before = ServiceEntityData.Store;

    public TransformScaleKeeperTests() => ServiceEntityData.Store = _store;

    public void Dispose() => ServiceEntityData.Store = _before;

    private TransformScaleKeeper.Saved? Kept(uint e) =>
        TransformScaleKeeper.Parse(_store.Get(new EntityId(e), TransformScaleKeeper.Scope, TransformScaleKeeper.Key));

    [Fact]
    public void A_set_scale_is_kept_with_the_game_scale_from_before_the_first_write_and_forgotten_when_set_back()
    {
        var e = new EntityId(1311);
        var asked = new Scale3(1.25f, 0.75f, 1.5f);
        Assert.True(TransformScaleKeeper.Record(e, Scale3.One, asked, "scale-mod"));
        Assert.Equal(new TransformScaleKeeper.Saved("scale-mod", asked, Scale3.One), Kept(1311));
        // a second write (another mod): the game scale stays the one from before Kiln's first write
        Assert.True(TransformScaleKeeper.Record(e, asked, Scale3.Uniform(2f), "other-mod"));
        Assert.Equal(new TransformScaleKeeper.Saved("other-mod", Scale3.Uniform(2f), Scale3.One), Kept(1311));
        // back at the game's own scale: nothing to keep, and the game brings that back itself
        Assert.True(TransformScaleKeeper.Record(e, Scale3.Uniform(2f), Scale3.One, "scale-mod"));
        Assert.Null(Kept(1311));
        // an unchanged write of the game scale keeps nothing
        Assert.True(TransformScaleKeeper.Record(e, Scale3.One, Scale3.One, "scale-mod"));
        Assert.Empty(_store.Data);
    }

    [Fact]
    public void Kilns_own_writes_and_session_only_entities_are_not_kept()
    {
        Assert.False(TransformScaleKeeper.Record(new EntityId(1400), Scale3.One, Scale3.Uniform(3f), modId: null));
        _store.SessionOnly.Add(1401);  // a DoNotSave template: the save never brings the entity back
        Assert.False(TransformScaleKeeper.Record(new EntityId(1401), Scale3.One, Scale3.Uniform(3f), "scale-mod"));
        Assert.Empty(_store.Data);
    }

    [Fact]
    public void The_record_is_json_and_a_bad_one_reads_as_none()
    {
        var json = TransformScaleKeeper.Json(new TransformScaleKeeper.Saved("m", new Scale3(1.25f, 0.75f, 1.5f), Scale3.One));
        Assert.Equal("{\"mod\":\"m\",\"scale\":{\"x\":1.25,\"y\":0.75,\"z\":1.5},\"base\":{\"x\":1,\"y\":1,\"z\":1}}", json);
        Assert.Null(TransformScaleKeeper.Parse("{\"mod\":\"m\",\"scale\":{\"x\":0,\"y\":1,\"z\":1},\"base\":{\"x\":1,\"y\":1,\"z\":1}}"));
        Assert.Null(TransformScaleKeeper.Parse("not json"));
        Assert.Null(TransformScaleKeeper.Parse("{\"scale\":{\"x\":1,\"y\":1,\"z\":1}}"));
        Assert.Null(TransformScaleKeeper.Parse(null));
    }
}
