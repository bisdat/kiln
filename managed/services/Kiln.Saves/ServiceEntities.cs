// SPDX-License-Identifier: MIT
// Kiln.Saves' side of Kiln.Api's ServiceEntityData: a Kiln service's own values on saved entities (Kiln.World's
// transform scale), kept in the entity attachments under the service's scope ("kiln:world") as if it were one more mod:
// saved as the bundle entity/kiln:world, matched to the loaded entity after a restart like a mod's EntityData, dropped
// with the entity. Tick thread.
namespace Kiln;

internal sealed class ServiceEntities(EntityTable? testTable = null) : ServiceEntityData.IStore
{
    /// <inheritdoc />
    public bool Set(EntityId entity, string scope, string key, string? json)
    {
        Check(scope, key);
        if (entity.IsNone || entity.IsPlayer || (testTable is null && !SavesRuntime.Ensure()) || !SpawnLink.Available) return false;
        var store = SavesRuntime.Store;
        if (testTable is null) store.EnsureOpen();
        var table = Table(scope);
        if (json is null)
        {
            if ((ServiceEntityData.IsStrict(scope) ? table.FindForRemoval(entity) : table.FindOrMatch(entity)) is { } r && r.ByMod.TryGetValue(scope, out var keys))
            {
                keys.Remove(key);
                if (keys.Count == 0) r.ByMod.Remove(scope);
            }
            return Persistent(entity);
        }
        if (!Persistent(entity)) return false;  // no positional identity: nothing a restart could find again
        var rec = table.Attach(entity);
        if (!rec.ByMod.TryGetValue(scope, out var k)) rec.ByMod[scope] = k = new SortedDictionary<string, WorldStore.Slot>(StringComparer.Ordinal);
        k[key] = new WorldStore.Slot { Json = json };
        return true;
    }

    /// <inheritdoc />
    public string? Get(EntityId entity, string scope, string key)
    {
        Check(scope, key);
        if (entity.IsNone || entity.IsPlayer || (testTable is null && !SavesRuntime.Ensure()) || !SpawnLink.Available) return null;
        return Table(scope).FindOrMatch(entity) is { } r && r.ByMod.TryGetValue(scope, out var keys) &&
               keys.TryGetValue(key, out var s) ? s.Json : null;
    }

    /// <inheritdoc />
    public IReadOnlyList<EntityId> Entities(string scope) =>
        Table(scope).LiveRecs.Where(r => r.ByMod.TryGetValue(scope, out var k) && k.Count > 0).Select(r => new EntityId(r.Entity)).ToList();

    /// <inheritdoc />
    public IReadOnlyDictionary<string, string> Values(EntityId entity, string scope) =>
        Table(scope).Find(entity) is { } r && r.ByMod.TryGetValue(scope, out var keys)
            ? keys.Where(p => p.Value.Json is not null).ToDictionary(p => p.Key, p => p.Value.Json!, StringComparer.Ordinal)
            : new Dictionary<string, string>();

    /// <inheritdoc />
    public bool Holds(EntityId entity, string scope, string key)
    {
        Check(scope, key);
        if (entity.IsNone || entity.IsPlayer || (testTable is null && !SavesRuntime.Ensure()) || !SpawnLink.Available) return false;
        return Table(scope).Find(entity) is { } r && r.ByMod.TryGetValue(scope, out var keys) && keys.ContainsKey(key) &&
               (!ServiceEntityData.IsStrict(scope) || Table(scope).HoldsStrict(r, entity)) &&
               SpawnLink.Inspect(entity) is { } info && info.Template is { } t && t == r.Template;
    }

    private bool Persistent(EntityId entity) => testTable is null ? EntityData.IsPersistent(entity) :
        SpawnLink.Inspect(entity)?.Template is { } g && SpawnLink.Template(g).canPersist;

    private EntityTable Table(string scope) => ServiceEntityData.IsStrict(scope)
        ? (testTable ?? SavesRuntime.Store.Entities).Strict(scope) : testTable ?? SavesRuntime.Store.Entities;

    /// <inheritdoc />
    public IReadOnlyList<ServiceEntityData.Replay> Replays(string scope, string key)
    {
        Check(scope, key);
        var table = Table(scope);
        return table.Records.Where(r => r.ByMod.TryGetValue(scope, out var keys) && keys.ContainsKey(key))
            .Select(r => new ServiceEntityData.Replay(new EntityId(r.Entity), r.Template, (r.WaitingReason is not null ? r.MatchPosition : null) ?? new WorldPosition(r.X, r.Y, r.Z),
                r.ByMod[scope][key].Json ?? "null", r.WaitingReason ?? (r.Entity == 0 ? "its entity has not been found at the recorded place yet" : null),
                r.WaitingReason is not null)).ToList();
    }

    private static void Check(string scope, string key)
    {
        if (!ServiceEntityData.IsScope(scope)) throw new ArgumentException("a service scope is kiln:<service>", nameof(scope));
        WorldStore.CheckKey(key);
    }
}
