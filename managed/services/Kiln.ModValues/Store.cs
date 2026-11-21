// SPDX-License-Identifier: MIT
// The ledger's records in Kiln.Saves (CharacterStore's explicit-owner overloads) and the ledger's events on Kiln's bus.
namespace Kiln;

internal sealed class SavesValueStore : IValueStore
{
    internal const string WorldKey = "mod-values.world.v1", AuthorityKey = "mod-values.carried-authority.v1", ProjectionKey = "mod-values.carried.v1";

    private static (string Prefix, string Key) Where(Partition p) => p switch
    {
        Partition.World => (CharacterData.WorldPrefix, WorldKey),
        Partition.CarriedAuthority => (CharacterData.WorldPrefix, AuthorityKey),
        _ => (CharacterData.CarriedPrefix, ProjectionKey),
    };

    public Guid World => CharacterStore.OwnedWorld;

    public string? Read(Partition p, CharacterKey c, string owner)
    {
        var (prefix, key) = Where(p);
        return CharacterStore.ReadOwned(prefix, c, owner, key);
    }

    public void Write(Partition p, CharacterKey c, string owner, string json)
    {
        var (prefix, key) = Where(p);
        CharacterStore.WriteOwned(prefix, c, owner, key, json);
    }

    public IReadOnlyList<(CharacterKey Character, string Owner)> Find(Partition p)
    {
        var (prefix, key) = Where(p);
        return CharacterStore.FindOwned(prefix, key);
    }
}

/// <summary>Records in memory (offline tests, the test kit).</summary>
internal sealed class MemoryValueStore : IValueStore
{
    public readonly Dictionary<(Partition, CharacterKey, string), string> Records = new();
    public Guid World { get; set; } = Guid.NewGuid();

    public string? Read(Partition p, CharacterKey c, string owner) => Records.GetValueOrDefault((p, c, owner));

    public void Write(Partition p, CharacterKey c, string owner, string json) => Records[(p, c, owner)] = json;

    public IReadOnlyList<(CharacterKey Character, string Owner)> Find(Partition p) =>
        Records.Keys.Where(k => k.Item1 == p).Select(k => (k.Item2, k.Item3)).ToList();
}

internal sealed class BusValueEvents : IValueEvents
{
    public void Changed(ModValueChangedEvent e)
    {
        if (Events.HasSubscribers<ModValueChangedEvent>()) Events.Post(e);
    }

    public void Imported(ModValueImportedEvent e)
    {
        if (Events.HasSubscribers<ModValueImportedEvent>()) Events.Post(e);
    }

    public string? Importing(ModValueImportingEvent e)
    {
        if (!Events.HasSubscribers<ModValueImportingEvent>()) return null;
        Events.Post(e);
        return e.Canceled ? e.Reason : null;
    }
}

/// <summary>Events kept in a list (offline tests).</summary>
internal sealed class ListValueEvents : IValueEvents
{
    public readonly List<KilnEvent> All = [];
    public Func<ModValueImportingEvent, string?>? OnImporting;

    public void Changed(ModValueChangedEvent e) => All.Add(e);

    public void Imported(ModValueImportedEvent e) => All.Add(e);

    public string? Importing(ModValueImportingEvent e)
    {
        All.Add(e);
        return OnImporting?.Invoke(e);
    }
}
