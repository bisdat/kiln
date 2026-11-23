// SPDX-License-Identifier: MIT
// The public shapes of Kiln.ModValues (dossier chapter 05b section 5): kinds and policies, keys, the author's options,
// snapshots and results by Kiln's one error rule. Usage: docs/kiln/services/mod-values.md.
namespace Kiln;

/// <summary>What a mod value is. Same numbers in C#, JSON (lowercase names) and the native facade (KMV_*).</summary>
public enum ModValueKind
{
    /// <summary>A plain number; may go down. A Carried counter must stay nonnegative.</summary>
    Counter = 0,
    /// <summary>Money: never below zero, spent with <see cref="ModValues.Spend"/>, never carried between worlds.</summary>
    Currency = 1,
    /// <summary>Experience with levels from <see cref="ModValueOptions.Thresholds"/>; never below zero, scale 0.</summary>
    Experience = 2,
}

/// <summary>Where a value lives.</summary>
public enum ModValuePersistence
{
    /// <summary>This world's record of the character (CharacterData): the safe default.</summary>
    World = 0,
    /// <summary>Also travels with the character to other worlds (merged there by maximum). Monotonic progress only:
    /// experience or a nonnegative counter with <see cref="ModValueOptions.TrustClientImports"/>.</summary>
    Carried = 1,
}

/// <summary>Which mods may read a value (cooperative protection between mods, not a sandbox).</summary>
public enum ModValueReadPolicy
{
    /// <summary>Only the defining mod.</summary>
    OwnerOnly = 0,
    /// <summary>Every loaded mod.</summary>
    Public = 1,
    /// <summary>The defining mod and the mods in <see cref="ModValueOptions.ReadMods"/>.</summary>
    AllowList = 2,
}

/// <summary>What the owning player's Kiln client receives.</summary>
public enum ModValueSyncPolicy
{
    /// <summary>Never sent: for values that must stay on the server.</summary>
    None = 0,
    /// <summary>Sent to the character's own client; there only the defining mod reads it.</summary>
    OwnerMod = 1,
    /// <summary>Sent to the character's own client; there the read policy decides who reads it.</summary>
    Readers = 2,
}

/// <summary>A value's name: the defining mod's id and the value's id, written "mod:id". A key is a name, never a
/// permission: every call checks the caller again.</summary>
/// <param name="Mod">The defining mod's id (lowercase), or a native owner's namespace ("native." and 32 hex digits).</param>
/// <param name="Id">The value's id: <c>[a-z][a-z0-9._-]{0,63}</c>.</param>
public readonly record struct ModValueKey(string Mod, string Id)
{
    /// <summary>"mod:id".</summary>
    public override string ToString() => Mod + ":" + Id;

    /// <summary>Reads "mod:id" (exactly one colon, no spaces). False for anything else.</summary>
    public static bool TryParse(string? text, out ModValueKey key)
    {
        key = default;
        if (text is null) return false;
        var colon = text.IndexOf(':');
        if (colon <= 0 || colon != text.LastIndexOf(':')) return false;
        var mod = text[..colon];
        var id = text[(colon + 1)..];
        if (!ModValueRules.IsOwner(mod) || !ModValueRules.IsId(id)) return false;
        key = new ModValueKey(mod, id);
        return true;
    }
}

/// <summary>How a mod value behaves: the same fields as a <c>mod-values/*.json</c> entry.</summary>
/// <param name="Kind">Counter, currency or experience.</param>
/// <param name="Label">The name players see, in the game's languages.</param>
/// <param name="Min">Lowest raw value (currency and experience: 0 or more).</param>
/// <param name="Max">Highest raw value.</param>
/// <param name="Initial">Where a character starts; null: <paramref name="Min"/>. Must be within the bounds.</param>
/// <param name="Scale">Fixed decimal places, 0 to 6: the raw value 1234 with scale 2 shows as 12.34.</param>
/// <param name="Thresholds">Experience only (required there): raw values where levels start, from 0, strictly
/// increasing, at most 256. The level is one plus the index of the greatest threshold reached.</param>
/// <param name="NumberFormat">How the number shows: N0-N6, F0-F6, "0" or "0." with 1 to 6 zeros; null: N0, or
/// N&lt;scale&gt;. Per language like a label.</param>
/// <param name="Persistence">World (default) or Carried.</param>
/// <param name="TrustClientImports">Required for Carried: the value may come in from a client's copy (by maximum).</param>
/// <param name="Version">Raise it when you change kind, bounds, scale, thresholds, initial or persistence.</param>
/// <param name="CarryEpoch">Carried only: raise it to reset carried progress everywhere.</param>
/// <param name="ReadPolicy">Which mods may read the value.</param>
/// <param name="ReadMods">With <see cref="ModValueReadPolicy.AllowList"/>: the mods that may read it (at most 32).</param>
/// <param name="Sync">What the owning player's client receives.</param>
/// <param name="AllowPartitionMove">With a new version: move a value between World and Carried instead of holding it.</param>
/// <param name="ResetOnKindChange">With a new version: a kind change resets to the new initial value instead of
/// holding it.</param>
public sealed record ModValueOptions(ModValueKind Kind, UiText Label,
    long Min = 0, long Max = long.MaxValue, long? Initial = null, int Scale = 0,
    IReadOnlyList<long>? Thresholds = null, UiText? NumberFormat = null,
    ModValuePersistence Persistence = ModValuePersistence.World, bool TrustClientImports = false,
    uint Version = 1, uint CarryEpoch = 1, ModValueReadPolicy ReadPolicy = ModValueReadPolicy.OwnerOnly,
    IReadOnlyList<string>? ReadMods = null, ModValueSyncPolicy Sync = ModValueSyncPolicy.OwnerMod,
    bool AllowPartitionMove = false, bool ResetOnKindChange = false);

/// <summary>A value's revision in one world: for compare-and-set. Revision 0 is a real revision (a fresh value), so
/// pass the world too: revision 0 of another world never matches.</summary>
/// <param name="World">The world the revision belongs to (a provisional session id until a new world's first save).</param>
/// <param name="ValueRevision">The value's revision in that world; it rises with every accepted change.</param>
public readonly record struct ModValueVersion(Guid World, ulong ValueRevision);

/// <summary>One value of one character, as of one moment (a copy: changing nothing).</summary>
/// <param name="World">The world the value is from (a provisional session id until a new world's first save).</param>
/// <param name="Character">The character the value belongs to.</param>
/// <param name="Key">Which value: the defining mod and the value's id.</param>
/// <param name="Value">The raw value; with <paramref name="Scale"/> 2, 1234 means 12.34.</param>
/// <param name="Level">Experience: how many thresholds the value reached; 0 for counters and currencies.</param>
/// <param name="Scale">Fixed decimal places of <paramref name="Value"/>, 0 to 6.</param>
/// <param name="Kind">Counter, currency or experience.</param>
/// <param name="Persistence">World, or Carried to other worlds with the character.</param>
/// <param name="DefinitionVersion">The definition version the saved value was last converted to.</param>
/// <param name="CarryEpoch">Carried values: the carry epoch the value belongs to.</param>
/// <param name="ValueRevision">This value's revision in the world, for compare-and-set (see <see cref="Version"/>).</param>
/// <param name="PartitionRevision">The revision of the character's whole record for this mod and persistence (World
/// or Carried): it rises with a change to any value in it.</param>
public sealed record ModValueSnapshot(Guid World, CharacterKey Character, ModValueKey Key, long Value, int Level, int Scale,
    ModValueKind Kind, ModValuePersistence Persistence, uint DefinitionVersion, uint CarryEpoch,
    ulong ValueRevision, ulong PartitionRevision)
{
    /// <summary>The version to pass back for compare-and-set.</summary>
    public ModValueVersion Version => new(World, ValueRevision);
}

/// <summary>A definition as Kiln knows it (described or active): what a UI needs to show a value.</summary>
/// <param name="Key">The value's name: the defining mod and the value's id.</param>
/// <param name="Kind">Counter, currency or experience.</param>
/// <param name="Label">The name players see, in the game's languages.</param>
/// <param name="NumberFormat">How the number shows (N0-N6, F0-F6, "0" or "0." with zeros), per language.</param>
/// <param name="Min">Lowest raw value.</param>
/// <param name="Max">Highest raw value.</param>
/// <param name="Initial">The raw value a character starts with.</param>
/// <param name="Scale">Fixed decimal places, 0 to 6.</param>
/// <param name="Thresholds">Experience: the raw values where levels start; empty for other kinds.</param>
/// <param name="Persistence">World, or Carried to other worlds with the character.</param>
/// <param name="Version">The definition's version: saved values convert when it rises.</param>
/// <param name="CarryEpoch">Carried values: raising it resets carried progress everywhere.</param>
/// <param name="ReadPolicy">Which mods may read the value.</param>
/// <param name="ReadMods">With <see cref="ModValueReadPolicy.AllowList"/>: the mods that may read it; else empty.</param>
/// <param name="Sync">What the owning player's client receives.</param>
/// <param name="Active">True when the defining mod activated it in this load; false when it is only described.</param>
public sealed record ModValueDefinition(ModValueKey Key, ModValueKind Kind, UiText Label, UiText NumberFormat, long Min, long Max,
    long Initial, int Scale, IReadOnlyList<long> Thresholds, ModValuePersistence Persistence, uint Version, uint CarryEpoch,
    ModValueReadPolicy ReadPolicy, IReadOnlyList<string> ReadMods, ModValueSyncPolicy Sync, bool Active);

/// <summary>Result of <see cref="ModValues.Read"/> and <see cref="ModValues.Synced"/>.</summary>
/// <param name="Ok">True when the value was read.</param>
/// <param name="Reason">Why not: one of <see cref="ModValueReasons"/>; null when <see cref="Ok"/>.</param>
/// <param name="Value">The value as of the read; null when refused.</param>
public sealed record ModValueReadResult(bool Ok, string? Reason, ModValueSnapshot? Value) : IKilnResult;

/// <summary>Result of <see cref="ModValues.Describe"/> and <see cref="ModValues.Define"/>.</summary>
/// <param name="Ok">True when the definition was accepted.</param>
/// <param name="Reason">Why not: one of <see cref="ModValueReasons"/>; null when <see cref="Ok"/>.</param>
/// <param name="Key">The definition's key (when refused, null if the options did not name one).</param>
public sealed record ModValueDefinitionResult(bool Ok, string? Reason, ModValueKey? Key) : IKilnResult;

/// <summary>Result of a change. Accepted: <see cref="Before"/> and <see cref="After"/> (equal for a no-op such as adding
/// 0). Refused: <see cref="Reason"/> (one of <see cref="ModValueReasons"/>), and nothing changed.</summary>
/// <param name="Ok">True when the change was accepted.</param>
/// <param name="Reason">Why not: one of <see cref="ModValueReasons"/>; null when <see cref="Ok"/>.</param>
/// <param name="Before">The value before the change; null when refused.</param>
/// <param name="After">The value after the change; null when refused.</param>
public sealed record ModValueResult(bool Ok, string? Reason, ModValueSnapshot? Before, ModValueSnapshot? After) : IKilnResult;

/// <summary>The reasons a mod value call is refused (<see cref="IKilnResult.Reason"/>). The native facade's KMV_*
/// reasons have the same names.</summary>
public static class ModValueReasons
{
    /// <summary>A spend asked for more than the value holds (it would go below the minimum).</summary>
    public const string InsufficientFunds = "InsufficientFunds";
    /// <summary>A compare-and-set gave a version that is no longer current (another change came first, or the world got
    /// its id at its first save).</summary>
    public const string StaleRevision = "StaleRevision";
    /// <summary>No live per-character identity: an account-only key, a character not in game (writes), no world.</summary>
    public const string CharacterUnavailable = "CharacterUnavailable";
    /// <summary>The change would take the value past its minimum or maximum, or overflow: nothing changed.</summary>
    public const string BoundExceeded = "BoundExceeded";
    /// <summary>No definition with this key is known.</summary>
    public const string Undefined = "Undefined";
    /// <summary>The caller may not do this: only the defining mod writes a value, and reads follow its read policy.</summary>
    public const string AccessDenied = "AccessDenied";
    /// <summary>The client has no synced copy of this value (not received, sync none, another world).</summary>
    public const string NotSynced = "NotSynced";
    /// <summary>The definition was active and is gone (its mod unloaded): the data is kept.</summary>
    public const string DefinitionRetired = "DefinitionRetired";
    /// <summary>The saved value needs a higher definition version, or its kind/partition changed without the policy
    /// that allows it: it is held unchanged.</summary>
    public const string MigrationRequired = "MigrationRequired";
    /// <summary>The server refused a carried import (the server takes none, or a handler canceled the offer).</summary>
    public const string ImportRefused = "ImportRefused";
    /// <summary>Carried progress only grows.</summary>
    public const string NonMonotonic = "NonMonotonic";
    /// <summary>A limit: 128 definitions per mod, revision numbers exhausted.</summary>
    public const string Budget = "Budget";
    /// <summary>Carried progress resets only through a higher carry epoch.</summary>
    public const string EpochRequired = "EpochRequired";
    /// <summary>The character's values are not initialized yet (it was never in game since the definition came).</summary>
    public const string NotReady = "NotReady";
    /// <summary>The same id is already defined (or described) with other content.</summary>
    public const string DefinitionConflict = "DefinitionConflict";

    internal static readonly string[] ByCode =
    [
        "", InsufficientFunds, StaleRevision, CharacterUnavailable, BoundExceeded, Undefined, AccessDenied, NotSynced,
        DefinitionRetired, MigrationRequired, ImportRefused, NonMonotonic, Budget, EpochRequired, NotReady, DefinitionConflict,
    ];

    internal static uint Code(string? reason) => reason is null ? 0u : (uint)Math.Max(0, Array.IndexOf(ByCode, reason));
}
