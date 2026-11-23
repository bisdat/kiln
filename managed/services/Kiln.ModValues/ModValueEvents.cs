// SPDX-License-Identifier: MIT
// Events of Kiln.ModValues: one for every initialization, write, migration, import and reset on the server (tick
// thread), and for what a client's synced copy received (the client's main thread).
namespace Kiln;

/// <summary>Why a <see cref="ModValueChangedEvent"/> was posted (its <see cref="ModValueChangedEvent.Cause"/>).</summary>
public static class ModValueCauses
{
    /// <summary>A character got the value for the first time (Before is null).</summary>
    public const string Initialized = "initialized";
    /// <summary>Add, Spend or Set.</summary>
    public const string Changed = "changed";
    /// <summary>Reset to the initial value.</summary>
    public const string Reset = "reset";
    /// <summary>A higher definition version converted the saved value (scale, bounds, levels, kind, partition).</summary>
    public const string Migrated = "migrated";
    /// <summary>A client's carried copy raised the value (maximum merge).</summary>
    public const string Imported = "imported";
    /// <summary>A higher carry epoch reset carried progress.</summary>
    public const string EpochReset = "epoch-reset";
}

/// <summary>A value of a character changed on the server (tick thread, after the change). Every initialization,
/// write, migration, import and reset posts one; a no-op (adding 0) posts none.</summary>
public sealed class ModValueChangedEvent(CharacterKey character, ModValueKey key,
    ModValueSnapshot? before, ModValueSnapshot? after, string cause, ulong requestId) : KilnEvent
{
    /// <summary>The character whose value changed.</summary>
    public CharacterKey Character { get; } = character;
    /// <summary>Which value changed: the defining mod and the value's id.</summary>
    public ModValueKey Key { get; } = key;
    /// <summary>Null when the value did not exist before (<see cref="ModValueCauses.Initialized"/>).</summary>
    public ModValueSnapshot? Before { get; } = before;
    /// <summary>The value after the change.</summary>
    public ModValueSnapshot? After { get; } = after;
    /// <summary>One of <see cref="ModValueCauses"/>.</summary>
    public string Cause { get; } = cause;
    /// <summary>The native caller's request id; 0 for managed calls.</summary>
    public ulong RequestId { get; } = requestId;
}

/// <summary>The client's synced copy of a value changed (main thread, after the whole update is in place). Before is
/// null for a value that is new; After is null for one that was removed.</summary>
public sealed class ModValueSyncedEvent(ModValueKey key, ModValueSnapshot? before,
    ModValueSnapshot? after, string nonce, ulong sequence) : KilnEvent
{
    /// <summary>Which value changed in the copy: the defining mod and the value's id.</summary>
    public ModValueKey Key { get; } = key;
    /// <summary>The copy's value before this update; null for a value that is new.</summary>
    public ModValueSnapshot? Before { get; } = before;
    /// <summary>The copy's value after this update; null for a value that was removed.</summary>
    public ModValueSnapshot? After { get; } = after;
    /// <summary>The stream's id (new for each server, world and character).</summary>
    public string Nonce { get; } = nonce;
    /// <summary>The update's sequence number in its stream.</summary>
    public ulong Sequence { get; } = sequence;
}

/// <summary>A whole update of the client's synced copy is in place (after its <see cref="ModValueSyncedEvent"/>s).</summary>
public sealed class ModValuesSnapshotPublishedEvent(string nonce, ulong sequence, bool full) : KilnEvent
{
    /// <summary>The stream's id (new for each server, world and character).</summary>
    public string Nonce { get; } = nonce;
    /// <summary>The number of the update now in place, in its stream (as on its <see cref="ModValueSyncedEvent"/>s).</summary>
    public ulong Sequence { get; } = sequence;
    /// <summary>A full replacement (a new stream, a resync) rather than a change.</summary>
    public bool Full { get; } = full;
}

/// <summary>A client offered its carried copy of values (progress from other worlds). Tick thread, before anything is
/// applied; cancel refuses the whole offer. Not posted when the server refuses imports (characters.json).</summary>
public sealed class ModValueImportingEvent(CharacterKey character, IReadOnlyList<ModValueKey> offered) : CancelableEvent
{
    /// <summary>The character whose carried copy is offered.</summary>
    public CharacterKey Character { get; } = character;
    /// <summary>The keys in the offer (also ones that will be rejected).</summary>
    public IReadOnlyList<ModValueKey> Offered { get; } = offered;
    /// <summary>Logged and counted when canceled.</summary>
    public string Reason { get; set; } = "canceled by a handler";
}

/// <summary>A client's carried offer was applied (tick thread): how many values went up, stayed, were rejected.</summary>
public sealed class ModValueImportedEvent(CharacterKey character, int applied, int unchanged, int rejected) : KilnEvent
{
    /// <summary>The character whose carried copy was applied.</summary>
    public CharacterKey Character { get; } = character;
    /// <summary>How many offered values raised the server's value.</summary>
    public int Applied { get; } = applied;
    /// <summary>How many offered values were not above the server's value, which stayed as it was.</summary>
    public int Unchanged { get; } = unchanged;
    /// <summary>How many offered values were refused (unknown, not carried, or not allowed to come in).</summary>
    public int Rejected { get; } = rejected;
}

/// <summary>A definition was described, activated or retired (its mod unloaded). Posted on the thread that made the
/// change (tick thread for activation and retirement).</summary>
public sealed class ModValueDefinitionChangedEvent(ModValueKey key, ModValueDefinition? definition, string change) : KilnEvent
{
    /// <summary>Which definition changed: the defining mod and the value's id.</summary>
    public ModValueKey Key { get; } = key;
    /// <summary>Null when retired.</summary>
    public ModValueDefinition? Definition { get; } = definition;
    /// <summary>"described", "activated" or "retired".</summary>
    public string Change { get; } = change;
}
