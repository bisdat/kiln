// SPDX-License-Identifier: MIT
namespace Kiln.TestKit;

public sealed partial class KilnHarness
{
    private HarnessGameTexts? _gameTexts;
    private HarnessVoxelMaterials? _voxelMaterials;

    /// <summary>The game's texts per language and the items' text tags this harness's game has (the kit's content lookups,
    /// since 0.2.0-dev.31.13): what <c>Content.TextIn</c>, <c>Content.TryGetTexts</c>, <c>Content.LoadTextsAsync</c>,
    /// <c>Items.TextTags</c>, <c>Items.Description</c> and <c>Items.DisplayNameIn(item, language)</c> answer from. A
    /// dedicated-server harness (the default) has none, as a dedicated server: use <see cref="HarnessSide.HostingClient"/>
    /// or <see cref="HarnessSide.JoinedClient"/>.</summary>
    public HarnessGameTexts GameTexts => _gameTexts ??= new HarnessGameTexts(this);

    /// <summary>The game's voxel materials of this harness's game (the kit's content lookups, since 0.2.0-dev.31.13): what
    /// <c>Content.TryGetVoxelMaterials</c> and <c>Content.LoadVoxelMaterialsAsync</c> answer from, on every side.</summary>
    public HarnessVoxelMaterials VoxelMaterials => _voxelMaterials ??= new HarnessVoxelMaterials(this);
}

/// <summary>
/// The game's text tables and the items' text tags of a harness (<see cref="KilnHarness.GameTexts"/>). A change applies to
/// the next question (tables read before are dropped). Since 0.2.0-dev.31.13, mod API 0.2.22.
/// </summary>
public sealed class HarnessGameTexts
{
    private readonly KilnHarness _harness;

    internal HarnessGameTexts(KilnHarness harness) => _harness = harness;

    private GameLookupsState State => GameLookupsFake.StateOf(_harness);

    /// <summary>Adds (or replaces) a text of <paramref name="language"/> (<c>"de-de"</c>; <c>"De_De"</c> works too), with the
    /// game's markup if you like. The first text of a language makes the game ship it (<c>Content.TextLanguages</c>, in the
    /// order added).</summary>
    /// <exception cref="ArgumentException">An empty language, or tag id 0.</exception>
    public HarnessGameTexts Add(string language, uint locaId, string text)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(language);
        ArgumentNullException.ThrowIfNull(text);
        if (locaId == 0) throw new ArgumentException("a loca tag id is never 0", nameof(locaId));
        var key = ModLocale.Normalize(language);
        var s = State;
        lock (s.Gate)
        {
            if (!s.Texts.TryGetValue(key, out var t))
            {
                s.Texts[key] = t = new Dictionary<uint, string>();
                s.Languages.Add(key);
            }
            t[locaId] = text;
        }
        GameTextStore.Invalidate();
        return this;
    }

    /// <summary>Makes <paramref name="item"/> a game item with these text tags (<c>Items.TextTags</c>); its texts are the
    /// tables' texts of the tags (<see cref="Add"/>).</summary>
    public HarnessGameTexts AddItem(ItemId item, ItemTextTags tags)
    {
        if (item.Value == 0) throw new ArgumentException("item id 0 is no item", nameof(item));
        var s = State;
        lock (s.Gate) s.ItemTags[item.Value] = tags;
        GameTextStore.Invalidate();  // labels made from the old tags are made again
        return this;
    }

    /// <summary>The languages added so far, in order.</summary>
    public IReadOnlyList<string> Languages
    {
        get { var s = State; lock (s.Gate) return [.. s.Languages]; }
    }

    /// <summary>How many times Kiln read a table of this harness (a table is read once, then kept).</summary>
    public int Reads
    {
        get { var s = State; lock (s.Gate) return s.TextReads; }
    }
}

/// <summary>
/// The voxel materials of a harness (<see cref="KilnHarness.VoxelMaterials"/>): rows as the game's terraforming registry,
/// world table and items would give them. A change applies to the next question. Since 0.2.0-dev.31.13, mod API 0.2.22.
/// </summary>
public sealed class HarnessVoxelMaterials
{
    private readonly KilnHarness _harness;

    internal HarnessVoxelMaterials(KilnHarness harness) => _harness = harness;

    private GameLookupsState State => GameLookupsFake.StateOf(_harness);

    /// <summary>Adds (or replaces) the material of <paramref name="cellId"/> (1..127 terrain, 128..255 building). Its label
    /// comes from <c>Items.DisplayName</c> of its first placing item, else of its loot item, as in the game.</summary>
    /// <param name="cellId">The cell id.</param>
    /// <param name="name">Kiln's stable name (<c>"TerrainStone"</c>, <c>"Cell26"</c>).</param>
    /// <param name="lootItem">What digging it gives; <c>default</c> for none.</param>
    /// <param name="exchangeRate">Cells per loot item; null for none.</param>
    /// <param name="placingItems">The items that place it, the label's first.</param>
    /// <param name="hardness">The hardness category's name, or null.</param>
    /// <param name="healthPoints">Its health points.</param>
    /// <param name="guid">Its GUID in the world's table; <c>default</c> for none.</param>
    /// <exception cref="ArgumentOutOfRangeException"><paramref name="cellId"/> is not 1..255.</exception>
    public HarnessVoxelMaterials Add(int cellId, string name, ItemId lootItem = default, int? exchangeRate = null,
        IReadOnlyList<ItemId>? placingItems = null, string? hardness = null, int healthPoints = 0, Guid guid = default)
    {
        ArgumentOutOfRangeException.ThrowIfLessThan(cellId, 1);
        ArgumentOutOfRangeException.ThrowIfGreaterThan(cellId, 255);
        ArgumentException.ThrowIfNullOrWhiteSpace(name);
        var m = new VoxelMaterial(cellId, cellId >= 128 ? VoxelMaterialKind.Building : VoxelMaterialKind.Terrain, guid, name, lootItem,
            exchangeRate is > 0 ? exchangeRate : null, [.. placingItems ?? []], hardness, healthPoints, null, VoxelMaterialLabelSource.None);
        var s = State;
        lock (s.Gate) s.Materials[cellId] = m;
        VoxelMaterialData.Invalidate();
        return this;
    }

    /// <summary>How many times Kiln read this harness's materials (once, then kept).</summary>
    public int Reads
    {
        get { var s = State; lock (s.Gate) return s.MaterialReads; }
    }
}
