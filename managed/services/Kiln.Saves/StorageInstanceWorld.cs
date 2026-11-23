// SPDX-License-Identifier: MIT
namespace Kiln;

/// <summary>Storage holders are part of their character's inventory witness, in persisted, non-overlapping slot bands.
/// Normalize reads AND tracker notices, so moving an item between a bag and a storage never changes its owner.</summary>
internal sealed class StorageInstanceWorld : IInstanceWorld
{
    private readonly IInstanceWorld _inner;
    private readonly Func<InstancePlace, InstancePlace> _normalize;
    private readonly Func<EntityId, IReadOnlyList<EntityId>> _parts;
    private readonly Func<EntityId, bool> _ready;
    internal StorageInstanceWorld(IInstanceWorld inner, Func<InstancePlace, InstancePlace> normalize,
        Func<EntityId, IReadOnlyList<EntityId>> parts, Func<EntityId, bool> ready)
    {
        (_inner, _normalize, _parts, _ready) = (inner, normalize, parts, ready);
        inner.Moved += (item, from, to) => Moved?.Invoke(item, _normalize(from), _normalize(to));
        inner.Gone += (item, place, step) => Gone?.Invoke(item, _normalize(place), step);
    }
    public void Require() => _inner.Require();
    public ulong Step => _inner.Step;
    private InstanceItem? Map(InstanceItem? item) => item is { } i ? i with { Place = _normalize(i.Place) } : null;
    public InstanceItem? Read(ItemInstanceRef item) => Map(_inner.Read(item));
    public IReadOnlyList<InstanceItem?> Read(IReadOnlyList<ItemInstanceRef> items) => _inner.Read(items).Select(Map).ToArray();
    public IReadOnlyList<InstanceItem> ReadHolder(EntityId holder, out bool initialized)
    {
        initialized = _ready(holder);
        if (!initialized) return [];
        var items = new List<InstanceItem>();
        foreach (var part in _parts(holder))
        {
            var rows = _inner.ReadHolder(part, out var ready);
            if (!ready) { initialized = false; return []; }
            items.AddRange(rows.Select(i => i with { Place = _normalize(i.Place) }));
        }
        return items.OrderBy(i => i.Place.FlatSlot).ToArray();
    }
    public IDisposable Follow(ItemInstanceRef item) => _inner.Follow(item);
    public event Action<ItemInstanceRef, InstancePlace, InstancePlace>? Moved;
    public event Action<ItemInstanceRef, InstancePlace, ulong>? Gone;
    public HolderIdentity Holder(EntityId holder) => _inner.Holder(_normalize(new InstancePlace(holder, 0)).Holder);
    public IReadOnlyList<EntityId> FindHolders(Guid template, WorldPosition near, double radius) => _inner.FindHolders(template, near, radius);
    public IReadOnlyList<(EntityId Entity, CharacterKey Character, ulong Visit)> PlayersInGame() => _inner.PlayersInGame();
}
