// SPDX-License-Identifier: MIT
namespace Kiln;

/// <summary>Enshrouded's gems for mods: the catalog, the gems a player's equipment runs, sockets, new gems with their
/// own effects (<c>Register</c>), the forge's actions and gem events. docs/kiln/services/gems.md.</summary>
public static partial class Gems
{
    /// <summary>Every gem item: the game's and those mods added.</summary>
    public static IReadOnlyList<GemInfo> Catalog => throw new NotImplementedException("gem wave G1");

    /// <summary>One gem item's definition, or null when the item is not a gem.</summary>
    public static GemInfo? Get(ItemId gem) => throw new NotImplementedException("gem wave G1");

    /// <summary>The gems whose effects run for a player now, one per equipment impact slot with a socketed gem.</summary>
    public static IReadOnlyList<EquippedGem> Equipped(EntityId player) => throw new NotImplementedException("gem wave G1");

    /// <summary>The gem in an item's socket, or null when it has no socket or the socket is empty.</summary>
    public static SocketedGem? Socket(ItemInstanceRef carrier) => throw new NotImplementedException("gem wave G1");
}
