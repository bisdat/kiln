// SPDX-License-Identifier: MIT
// A Kiln service's own values on a saved entity (Kiln.Api's ServiceEntityData; Kiln.World keeps transform scales there,
// 0.2.0-dev.28 fix round): saved as the bundle entity/kiln:<service> beside the mods' ones, matched to the loaded entity
// after a restart like a mod's EntityData, and handed to the service once, at the pump after the match.
using System.Reflection;
using System.Text.Json.Nodes;
using Xunit;

namespace Kiln;

[Collection("entity identity")]
public sealed class ServiceEntityValuesTests : IDisposable
{
    private static readonly Guid Chest = Guid.Parse("00000002-0000-0000-0000-000000000002");
    private const string Scope = "kiln:unit-test";
    private const BindingFlags PrivateInstance = BindingFlags.Instance | BindingFlags.NonPublic;
    private readonly string _root = Path.Combine(AppContext.BaseDirectory, "service-entity-tests", Guid.NewGuid().ToString("N"));

    public ServiceEntityValuesTests()
    {
        TestWorld.Reset();
        TestWorld.ClearMetadataCache();
        TestWorld.Available = true;
        TestWorld.RegistryReady = true;
        TestWorld.RegistryResult = 0;
        TestWorld.Templates[Chest] = new(3, 9, "PropChest"); // KNOWN | TRANSFORM
        Directory.CreateDirectory(Path.Combine(_root, "manifests"));
        Directory.CreateDirectory(Path.Combine(_root, "blobs"));
    }

    public void Dispose()
    {
        TestWorld.Reset();
        Directory.Delete(_root, recursive: true);
    }

    private static EntityId Place(uint id)
    {
        var entity = new EntityId(id);
        TestWorld.World[id] = new SpawnLink.Ent(entity, Chest, null, new WorldPosition(42, 0, 0), 0);
        return entity;
    }

    private WorldStore Store()
    {
        var store = new WorldStore();
        typeof(WorldStore).GetProperty("Root")!.SetValue(store, _root);
        typeof(WorldStore).GetProperty("WorldId")!.SetValue(store, "service-values-unit");
        typeof(WorldStore).GetField("_opened", PrivateInstance)!.SetValue(store, true);
        return store;
    }

    [Fact]
    public void A_service_value_is_saved_in_its_own_bundle_and_handed_over_once_after_the_restart()
    {
        Assert.True(ServiceEntityData.IsScope("kiln:world"));
        Assert.False(ServiceEntityData.IsScope("kiln.world"));  // a mod id may be kiln.world; it can never hold a ':'
        var handed = new List<(EntityId Entity, IReadOnlyDictionary<string, string> Values)>();
        var worldChanges = 0;
        ServiceEntityData.Register(Scope, new ServiceEntityData.Consumer { Loaded = (e, v) => handed.Add((e, v)), WorldChanged = () => worldChanges++ });

        var store = Store();
        var rec = store.Entities.Attach(Place(2001));
        rec.ByMod[Scope] = new() { ["transform-scale"] = new WorldStore.Slot { Json = "{\"v\":1}" } };
        rec.ByMod["some-mod"] = new() { ["note"] = new WorldStore.Slot { Json = "\"mod data\"" } };
        store.Stage(1, SaveReason.Request, late: false);
        store.Commit(1, 1001, SaveReason.Request);
        var bundles = JsonNode.Parse(File.ReadAllText(Path.Combine(_root, "manifests", "0000000001-1001.json")))!["bundles"]!.AsObject();
        Assert.NotNull(bundles["entity/" + Scope]);
        Assert.NotNull(bundles["entity/some-mod"]);
        TestWorld.World.Clear();

        // the restart: the chest is entity 3001 now
        var loaded = Store();
        typeof(WorldStore).GetMethod("Load", PrivateInstance)!.Invoke(loaded, [1u, 1001u]);
        loaded.Entities.StartMatching();
        var now = Place(3001);
        loaded.Entities.HandOverServiceValues();
        Assert.Empty(handed);  // nothing matched yet
        loaded.Entities.Tick();
        loaded.Entities.HandOverServiceValues();
        var (entity, values) = Assert.Single(handed);
        Assert.Equal(now, entity);
        Assert.Equal("{\"v\":1}", values["transform-scale"]);
        loaded.Entities.HandOverServiceValues();
        Assert.Single(handed);  // once
        Assert.Equal("\"mod data\"", loaded.Entities.Find(now)!.ByMod["some-mod"]["note"].Json);  // the mod's data is its own

        // the world closes: the service is told to let go of what it keeps per entity id (ids mean nothing in the next world)
        var changes = worldChanges;
        loaded.CloseWorld();
        Assert.Equal(changes + 1, worldChanges);
        Assert.Null(loaded.Entities.Find(now));
    }
}
