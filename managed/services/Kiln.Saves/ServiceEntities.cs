// SPDX-License-Identifier: MIT
// Kiln.Saves' side of Kiln.Api's ServiceEntityData: a Kiln service's own values on saved entities (Kiln.World's
// transform scale), kept in the entity attachments under the service's scope ("kiln:world") as if it were one more mod:
// saved as the bundle entity/kiln:world, matched to the loaded entity after a restart like a mod's EntityData, dropped
// with the entity. Tick thread.
namespace Kiln;

internal sealed class ServiceEntities : ServiceEntityData.IStore
{
    public bool Set(EntityId entity, string scope, string key, string? json)
    {
        Check(scope, key);
        if (entity.IsNone || entity.IsPlayer || !SavesRuntime.Ensure() || !SpawnLink.Available) return false;
        var store = SavesRuntime.Store;
        store.EnsureOpen();
        var table = store.Entities;
        if (json is null)
        {
            if (table.FindOrMatch(entity) is { } r && r.ByMod.TryGetValue(scope, out var keys))
            {
                keys.Remove(key);
                if (keys.Count == 0) r.ByMod.Remove(scope);
            }
            return EntityData.IsPersistent(entity);
        }
        if (!EntityData.IsPersistent(entity)) return false;  // no positional identity: nothing a restart could find again
        var rec = table.Attach(entity);
        if (!rec.ByMod.TryGetValue(scope, out var k)) rec.ByMod[scope] = k = new SortedDictionary<string, WorldStore.Slot>(StringComparer.Ordinal);
        k[key] = new WorldStore.Slot { Json = json };
        return true;
    }

    public string? Get(EntityId entity, string scope, string key)
    {
        Check(scope, key);
        if (entity.IsNone || entity.IsPlayer || !SavesRuntime.Ensure() || !SpawnLink.Available) return null;
        return SavesRuntime.Store.Entities.FindOrMatch(entity) is { } r && r.ByMod.TryGetValue(scope, out var keys) &&
               keys.TryGetValue(key, out var s) ? s.Json : null;
    }

    public IReadOnlyList<EntityId> Entities(string scope) =>
        SavesRuntime.Store.Entities.LiveRecs.Where(r => r.ByMod.TryGetValue(scope, out var k) && k.Count > 0).Select(r => new EntityId(r.Entity)).ToList();

    public IReadOnlyDictionary<string, string> Values(EntityId entity, string scope) =>
        SavesRuntime.Store.Entities.Find(entity) is { } r && r.ByMod.TryGetValue(scope, out var keys)
            ? keys.Where(p => p.Value.Json is not null).ToDictionary(p => p.Key, p => p.Value.Json!, StringComparer.Ordinal)
            : new Dictionary<string, string>();

    private static void Check(string scope, string key)
    {
        if (!ServiceEntityData.IsScope(scope)) throw new ArgumentException("a service scope is kiln:<service>", nameof(scope));
        WorldStore.CheckKey(key);
    }
}
