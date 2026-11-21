// SPDX-License-Identifier: MIT
// The facade of Kiln.ModValues (dossier chapter 05b section 5). Usage: docs/kiln/services/mod-values.md.
namespace Kiln;

/// <summary>
/// A mod's own per-character values: counters, currencies, and experience with levels. The server owns them and saves
/// them with the character (CharacterData, in this world's sidecar); the owning player's Kiln client gets a read-only
/// copy (<see cref="Synced"/>). A vanilla client sees nothing of them.
/// </summary>
/// <remarks>
/// <para>Define a value once per load (C# here, or <c>mod-values/*.json</c>): <see cref="Describe"/> on both sides
/// makes it known (for UI), <see cref="Define"/> on the server activates it. A character gets every active value at
/// its initial when it is first in game; offline characters keep theirs.</para>
/// <para>Only the defining mod writes its values, only on the server's tick thread, and only for a character that is in
/// game. Players change a value only through your server code (a command, an action you handle); there is no client
/// write. Results follow Kiln's error rule: a refusal is <c>Ok = false</c> with a <see cref="ModValueReasons"/> reason
/// and changes nothing; a bad argument throws <see cref="ArgumentException"/>; an off capability throws
/// <see cref="KilnUnavailableException"/>.</para>
/// </remarks>
public static class ModValues
{
    /// <summary>"id" names the calling mod's value; "other-mod:id" names another mod's (to read it, where its read
    /// policy allows). A key is a name, not a permission.</summary>
    /// <exception cref="ArgumentException">Not an id or a qualified key.</exception>
    /// <exception cref="InvalidOperationException">An unqualified id outside a mod's code.</exception>
    [AnyThread]
    public static ModValueKey Key(string localOrQualified)
    {
        ArgumentNullException.ThrowIfNull(localOrQualified);
        if (localOrQualified.Contains(':'))
            return ModValueKey.TryParse(localOrQualified, out var k) ? k : throw new ArgumentException($"\"{localOrQualified}\" is not a key: mod:id", nameof(localOrQualified));
        if (!ModValueRules.IsId(localOrQualified)) throw new ArgumentException($"\"{localOrQualified}\" is not a mod value id: [a-z][a-z0-9._-], at most 64 characters", nameof(localOrQualified));
        return new ModValueKey(ModValuesService.CallerOwner("ModValues.Key"), localOrQualified);
    }

    /// <summary>Makes the calling mod's value <paramref name="id"/> known on this side, without activating it: its
    /// label, format and kind for UI before any server answers. Both sides; the same options again are accepted,
    /// other options for the same id are refused (<see cref="ModValueReasons.DefinitionConflict"/>).</summary>
    /// <exception cref="ArgumentException">The options break a rule (bounds, thresholds, carried kinds, formats, limits).</exception>
    [AnyThread]
    [RequiresCapability("stats.definitions")]
    public static ModValueDefinitionResult Describe(string id, ModValueOptions options)
    {
        var owner = ModValuesService.CallerOwner("ModValues.Describe");
        return ModValuesService.Describe(Def.FromOptions(owner, id, options), ModContext.CurrentMod);
    }

    /// <summary>Activates the calling mod's value <paramref name="id"/> on the server (describes it too). Characters in
    /// game get it at its initial value now, others when they are next in game. Call it in OnLoad: on a game client that
    /// runs no world yet (a both-side mod in the main menu) the value is described now and activated when the client
    /// hosts a world, so a single-player or co-op host gets it without a hook of its own. Its saved values come back with it: a
    /// higher <see cref="ModValueOptions.Version"/> converts them (scale, bounds, levels), a changed definition at the
    /// same version holds them (<see cref="ModValueReasons.MigrationRequired"/>).</summary>
    /// <exception cref="ArgumentException">The options break a rule.</exception>
    [ServerOnly]
    [TickThreadOnly]
    [RequiresCapability("stats.own")]
    public static ModValueDefinitionResult Define(string id, ModValueOptions options)
    {
        ModValuesService.EnsureStarted();
        Services.RequireTickThread("ModValues.Define");
        var owner = ModValuesService.CallerOwner("ModValues.Define");
        var def = Def.FromOptions(owner, id, options);
        // a game client in its main menu (a both-side mod's OnLoad): kept, and active when this client hosts a world
        if (!ModValuesCaps.ServerRole && Mods.Side == ModSide.Client) return ModValuesService.DeferDefine(def, ModContext.CurrentMod);
        ModValuesService.NeedLedger("ModValues.Define");
        return ModValuesService.Define(def, ModContext.CurrentMod);
    }

    /// <summary>A character's value, also of a character that is offline. Refused: <see cref="ModValueReasons.NotReady"/>
    /// (never in game since the value came), <see cref="ModValueReasons.AccessDenied"/> (another mod's value its read
    /// policy keeps from you), <see cref="ModValueReasons.CharacterUnavailable"/> (an account-only key), Undefined.</summary>
    [ServerOnly]
    [TickThreadOnly]
    [RequiresCapability("stats.own")]
    public static ModValueReadResult Read(CharacterKey character, ModValueKey key)
    {
        var ledger = ModValuesService.NeedLedger("ModValues.Read");
        return ledger.Read(ModValuesService.CallerOwner("ModValues.Read"), character, key);
    }

    /// <summary>Adds <paramref name="amount"/> (an award; a counter also takes a negative amount). Adding 0 is accepted
    /// and changes nothing. Refused: <see cref="ModValueReasons.BoundExceeded"/>, NonMonotonic (carried), StaleRevision
    /// (<paramref name="expected"/>), CharacterUnavailable (not in game).</summary>
    [ServerOnly]
    [TickThreadOnly]
    [RequiresCapability("stats.own")]
    public static ModValueResult Add(CharacterKey character, ModValueKey key, long amount, ModValueVersion? expected = null) =>
        Change("ModValues.Add", character, key, ChangeOp.Add, amount, expected);

    /// <summary>Spends a positive <paramref name="amount"/> of a currency. Refused with
    /// <see cref="ModValueReasons.InsufficientFunds"/> when it would go below the minimum. Not a transaction with
    /// anything else: when what you sell then fails, give the amount back with <see cref="Add"/>.</summary>
    [ServerOnly]
    [TickThreadOnly]
    [RequiresCapability("stats.own")]
    public static ModValueResult Spend(CharacterKey character, ModValueKey key, long amount, ModValueVersion? expected = null) =>
        Change("ModValues.Spend", character, key, ChangeOp.Spend, amount, expected);

    /// <summary>Sets the raw value (within the bounds; a carried value only upwards).</summary>
    [ServerOnly]
    [TickThreadOnly]
    [RequiresCapability("stats.own")]
    public static ModValueResult Set(CharacterKey character, ModValueKey key, long value, ModValueVersion? expected = null) =>
        Change("ModValues.Set", character, key, ChangeOp.Set, value, expected);

    /// <summary>Back to the initial value. A carried value is refused (<see cref="ModValueReasons.EpochRequired"/>):
    /// raise its carry epoch instead.</summary>
    [ServerOnly]
    [TickThreadOnly]
    [RequiresCapability("stats.own")]
    public static ModValueResult Reset(CharacterKey character, ModValueKey key, ModValueVersion? expected = null) =>
        Change("ModValues.Reset", character, key, ChangeOp.Reset, 0, expected);

    private static ModValueResult Change(string api, CharacterKey character, ModValueKey key, ChangeOp op, long amount, ModValueVersion? expected)
    {
        var ledger = ModValuesService.NeedLedger(api);
        return ledger.Change(ModValuesService.CallerOwner(api), character, key, op, amount, expected, 0);
    }

    /// <summary>Client: this player's copy of a value, as the server last sent it (any thread). The caller is the mod
    /// whose code runs on this thread: the defining mod reads its own; others where the value's read and sync policy
    /// allow. Refused with <see cref="ModValueReasons.NotSynced"/> before the copy arrived, for a value that is not
    /// sent, and after leaving the world.</summary>
    [ClientOnly]
    [AnyThread]
    [RequiresCapability("stats.sync")]
    public static ModValueReadResult Synced(ModValueKey key)
    {
        ModValuesCaps.Require(ModValuesCaps.Sync);
        var mod = ModContext.CurrentMod;
        return ModValuesClient.Read(mod is null ? null : ModValueRules.OwnerOf(mod), key);
    }

    /// <summary>A known definition (described here, active on a server, or received from the server): label, format,
    /// kind, levels. Null when nothing is known of <paramref name="key"/>.</summary>
    [AnyThread]
    [RequiresCapability("stats.definitions")]
    public static ModValueDefinition? Definition(ModValueKey key)
    {
        if (ModValuesService.Registry.Active(key) is { } a) return a.ToPublic(true);
        if (ModValuesService.Registry.Described(key) is { } d) return d.ToPublic(false);
        return ModValuesClient.Definition(key);
    }

    /// <summary>The value as players see it: the exact decimal (raw / 10^scale, never through a double) in the
    /// definition's number format and the game's language ("12,345.67"; en-US on a dedicated server). Any thread, both
    /// sides.</summary>
    /// <exception cref="ArgumentException">No definition of the snapshot's key is known here.</exception>
    [AnyThread]
    public static string Format(ModValueSnapshot value)
    {
        ArgumentNullException.ThrowIfNull(value);
        var def = Definition(value.Key) ?? throw new ArgumentException($"{value.Key}: no definition is known here", nameof(value));
        return ModValueFormat.Format(value.Value, value.Scale, def.NumberFormat.ToString(), GameLanguage.Current);
    }
}
