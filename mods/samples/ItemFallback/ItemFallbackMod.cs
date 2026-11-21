// SPDX-License-Identifier: MIT
using Kiln;

namespace ItemFallback;

/// <summary>
/// Sample: what players without a mod see of its items. Kiln never sends a player an item id their game does not know:
/// to a vanilla client, and to a Kiln client without this mod's content, each Kiln-only item becomes its declared
/// look-alike (<see cref="ItemDefinition.VanillaLook"/>, a game item of the same kind) or, without one, an empty slot.
/// <list type="bullet">
/// <item>Frost Can: a watering can of its own (a clone of the game's), declaring the game's can as its look-alike.</item>
/// <item>Frost Charm: a clone of the can that declares nothing: players without this mod see an empty slot.</item>
/// </list>
/// On the server, /fallback shows what the remap did (items, sites, ids replaced).
/// </summary>
public sealed class ItemFallbackMod : Mod
{
    private const string Can = "Tool_T1_WateringCan";
    public static readonly Guid FrostCan = Guid.Parse("3c7e9a14-58b2-4d61-9f0a-2b6c8e4d1f75");
    public static readonly Guid FrostCharm = Guid.Parse("a91d4f60-27c3-48e5-b1a8-6e0f3d9c5b22");

    private IDisposable? _command;

    public override void OnLoad()
    {
        if (!Content.Available || !Capabilities.Is("content.new") || !NewContent.Available)
        {
            Log.Warn("kiln.content cannot add items on this game build; ItemFallback does nothing");
            return;
        }
        Content.Items.Register(new ItemDefinition
        {
            Guid = FrostCan,
            Name = "Kiln_Sample_FrostCan",
            BaseOn = Can,
            DisplayName = "Frost Can",
            Description = "A watering can that never freezes. Players without this mod see the game's own watering can.",
            VanillaLook = Can,
        });
        Content.Items.Register(new ItemDefinition
        {
            Guid = FrostCharm,
            Name = "Kiln_Sample_FrostCharm",
            BaseOn = Can,
            DisplayName = "Frost Charm",
            Description = "It declares no look-alike: players without this mod see an empty slot where it is.",
        });
        // "/fallback" (admins, where the server world runs): what the remap did
        _command = Commands.Register("fallback", c =>
        {
            var s = Replication.RemapStats();
            c.Reply($"item remap {(s.Active ? "on" : "off")}: {s.KilnOnlyItems} Kiln-only items ({s.WithoutLookAlike} without a look-alike), " +
                    $"{s.Sites} sites; {s.Remapped} ids shown as their look-alike, {s.Zeroed} as an empty slot, {s.Faults} faults");
        }, CommandPermission.Admin, description: "what players without this mod are sent of its items");
    }
}
