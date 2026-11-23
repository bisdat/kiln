// SPDX-License-Identifier: MIT
using System.Numerics;

namespace Kiln;

/// <summary>The kinds of world a process runs (kiln_api.h's KILN_WORLD_* values). A dedicated server runs the server
/// world; a game client runs the client world and its prediction world while in game, plus the server world while it
/// hosts (single player or co-op host).</summary>
public enum WorldKind
{
    /// <summary>The authoritative gameplay world: a dedicated server's, or a host's.</summary>
    Server = 0,
    /// <summary>The client's prediction storage; it runs the client world's programs.</summary>
    Prediction = 1,
    /// <summary>The client's own world: local player, camera, presentation.</summary>
    Client = 2,
    /// <summary>Not a world: the game client's main menu (and the stretch between two worlds) as a target of a Kiln
    /// task's hops, <c>ctx.ToTick(WorldKind.Menu)</c> (since 0.2.0-dev.30). Its hops run on the game's UI frame while no
    /// world runs; world APIs refuse there and do not take this value.</summary>
    Menu = 3,
}

/// <summary>A server-world entity id. Ids can be reused after an entity is destroyed. On a dedicated server player
/// entities are slot + 1 (1..16 with 16 slots); <see cref="IsPlayer"/> tests the engine's reserved range 1..1023.</summary>
/// <param name="Value">The id; 0 is none.</param>
public readonly record struct EntityId(uint Value)
{
    /// <summary>No entity (0).</summary>
    public static readonly EntityId None = default;
    /// <summary>True for <see cref="None"/>.</summary>
    public bool IsNone => Value == 0;
    /// <summary>True for ids 1 to 1023, the engine's range for player entities.</summary>
    public bool IsPlayer => Value is >= 1 and <= 1023;
    /// <summary>The id as a number.</summary>
    public override string ToString() => Value.ToString();
}

/// <summary>A world position in metres (x east, y up, z north). The engine stores 32.32 fixed point.</summary>
/// <param name="X">Metres east.</param>
/// <param name="Y">Metres up.</param>
/// <param name="Z">Metres north.</param>
public readonly record struct WorldPosition(double X, double Y, double Z)
{
    /// <summary>A position from a <see cref="Vector3"/> (metres).</summary>
    public static WorldPosition From(Vector3 v) => new(v.X, v.Y, v.Z);
    /// <summary>This position as a <see cref="Vector3"/> (single precision).</summary>
    public Vector3 ToVector3() => new((float)X, (float)Y, (float)Z);
    /// <summary>The straight-line distance to <paramref name="o"/>, in metres.</summary>
    public double DistanceTo(WorldPosition o) => Math.Sqrt((X - o.X) * (X - o.X) + (Y - o.Y) * (Y - o.Y) + (Z - o.Z) * (Z - o.Z));
    /// <summary><c>(x, y, z)</c> with one decimal each.</summary>
    public override string ToString() => $"({X:F1}, {Y:F1}, {Z:F1})";
}
