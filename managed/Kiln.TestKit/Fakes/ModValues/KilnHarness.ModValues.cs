// SPDX-License-Identifier: MIT
// The test kit's Kiln.ModValues: the real ledger (definitions, revisions, refusals, migration, events) on an in-memory
// store instead of the world's sidecar, with characters joined and left by the test (a write needs the character in
// game). ModValues.Define/Read/Add/Spend/Set/Reset work in kiln.Run as they do on a server.
namespace Kiln.TestKit;

public sealed partial class KilnHarness
{
    private HarnessModValues? _modValues;

    /// <summary>The fake of Kiln.ModValues' world store and live characters (created and installed on first use).</summary>
    public HarnessModValues ModValues => _modValues ??= new HarnessModValues();
}

/// <summary>Kiln.ModValues offline: which characters are in game, the world id, and the records as JSON.</summary>
public sealed class HarnessModValues
{
    private readonly MemoryValueStore _store = new();
    private readonly HashSet<CharacterKey> _live = [];

    internal HarnessModValues() => ModValuesService.UseForTests(_store, _live.Contains);

    /// <summary>A character id for tests: account 0x1111..., id n.</summary>
    public static CharacterKey Character(int n) => new(0x1111222233334444, new Guid(n, 0, 0x4000, 0x80, 0, 0, 0, 0, 0, 0, (byte)n));

    /// <summary>The character is in game: its values are initialized now, and writes to it are allowed.</summary>
    public void Join(CharacterKey character)
    {
        _live.Add(character);
        ModValuesService.Ledger?.Touch(character);
    }

    /// <summary>The character left: its values stay (readable), writes are refused (CharacterUnavailable).</summary>
    public void Leave(CharacterKey character) => _live.Remove(character);

    /// <summary>The world's id (revisions and compare-and-set are per world). Set a new one for "another world".</summary>
    public Guid World
    {
        get => _store.World;
        set => _store.World = value;
    }

    /// <summary>The records of a character and owning mod as stored ("world", "carried-authority", "carried"), or null.</summary>
    public string? Record(CharacterKey character, string mod, string partition = "world") =>
        _store.Read(partition switch { "world" => Partition.World, "carried-authority" => Partition.CarriedAuthority, "carried" => Partition.Projection,
            _ => throw new ArgumentException("world, carried-authority or carried", nameof(partition)) }, character, mod);
}
