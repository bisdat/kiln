// SPDX-License-Identifier: MIT
// A placed station on the server: what it is doing, and starting a recipe the way a player's choice does (player_crafting sets nextRecipe and clears isStoppedByPlayer).
namespace Kiln;

/// <summary>What a station is doing: the game's <c>FactoryStationState</c>.</summary>
public enum StationState { NoRecipe, MissingMaterials, Paused, Stopped, Running, MissingOutputSpace }

/// <summary>What keeps a station from running: the game's <c>FactoryStationRequirementMask</c>.</summary>
[Flags]
public enum StationNeeds { None = 0, Recipe = 1, EnergySourceInRange = 2, EnergyAmount = 4, InputMaterials = 8, OutputSpace = 16, AboveWater = 32 }

/// <param name="Running">The recipe in progress, or 0.</param>
/// <param name="Next">The recipe it was told to make, which it goes back to as long as there are inputs. 0: none.</param>
/// <param name="Started">Its current cycle's start, on the game's clock.</param>
public sealed record StationStatus(StationState State, uint Running, uint Next, TimeSpan Started, StationNeeds Needs, bool StoppedByPlayer);

/// <summary>
/// Placed stations that work on their own (kilns, smelters, looms, grinders, seed beds: every entity with the game's
/// <c>FactoryStation</c>), on the server, on the tick thread. Since 0.2.0-dev.29.14 also the game's own actions on them for
/// code (kiln.stations): fill from the base's crafting-stock chests (<see cref="Fill"/>, the code of the game's "fill from
/// chests" button, with no player), stop and resume (<see cref="Stop"/>, <see cref="Resume"/>: the stop button), the recipe
/// a station repeats (<see cref="Select"/>, the game's own write of a player's choice: prefer it to <see cref="Start"/>),
/// and moving its finished items into a chest (<see cref="Collect"/>). Everything is the server's own state, so other
/// players see the changes as they see their own. On the server world: a dedicated server or a hosting client; a joined
/// client has none (<see cref="Available"/> false). docs/kiln/services/stations.md.
/// </summary>
public static partial class Stations
{
    // keen::ecs::FactoryStation on the builds Kiln knows. The size stands in for a layout check: a game update that moves these changes it, and these calls then decline.
    private const string Component = "FactoryStation";
    private const int Size = 104, NextRecipe = 0x28, RecipeStart = 0x50, StoppedByPlayer = 0x61, Requirements = 0x62;

    /// <summary>What the station is doing. Null: it is no station that works on its own, or it is gone.</summary>
    public static StationStatus? Status(EntityId station)
    {
        if (World.ReadBytes(station, Component) is not { Length: Size } b) return null;
        return new StationStatus((StationState)b[0], BitConverter.ToUInt32(b, 4), BitConverter.ToUInt32(b, NextRecipe), TimeSpan.FromTicks(BitConverter.ToInt64(b, RecipeStart) / 100),
            (StationNeeds)b[Requirements], b[StoppedByPlayer] != 0);
    }

    /// <summary>Tells the station to make a recipe, as a player's choice in its menu does. The game then checks the rest: a recipe of another workshop shows as missing materials.</summary>
    /// <returns>False: it is no station that works on its own.</returns>
    public static bool Start(EntityId station, uint recipe)
    {
        if (World.ComponentSize(station, Component) != Size) return false;
        return World.TryWrite(station, Component, NextRecipe, recipe) && World.TryWrite(station, Component, StoppedByPlayer, (byte)0);
    }
}
