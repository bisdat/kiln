// SPDX-License-Identifier: MIT
// ClientComponentWrittenEvent (chapter 14 section 9.3, since 0.2.0-dev.29): a write a client or prediction program of a
// mod made through EcsProgramContext.TryWrite (Kiln.Api's EcsPrograms.TryBind) in a client's world. Kiln.Api cannot name
// this assembly's types, so this assembly hands it two delegates when it loads: one says (lock-free) whether anything
// listens, the other queues the event from the worker that made the write for the client world's next Pre on the main
// thread (Kiln.Api's ClientEvents).
using System.Runtime.CompilerServices;

namespace Kiln;

/// <summary>A mod's client or prediction program (<see cref="EcsPrograms.TryBind(string, EcsProgramCallback, WorldKind)"/>)
/// changed a component of an entity of this player's client world (or prediction storage) through
/// <see cref="EcsProgramContext.TryWrite{T}(string, T)"/>. Such a write is not marked for the game's save or replication
/// (TryWrite marks only in the server world; TryWriteOther is not available there): it is this player's local
/// presentation state, and the server world's <see cref="ComponentWrittenEvent"/> is not raised for it. Queued by the
/// program's thread (a game worker or the main thread) and raised on the game's main thread in the client world's next
/// Pre (on a host too, never on the server's tick), where a handler may touch the client world; an event of a client
/// world that ended before that Pre is not raised. Built only while the event has subscribers; one event per write.
/// Only Kiln raises it. Since 0.2.0-dev.29.</summary>
[ClientOnly]
public sealed class ClientComponentWrittenEvent : KilnEvent, IEntityEvent
{
    internal override bool TickDispatchAllowed => false;

    internal ClientComponentWrittenEvent(EntityId entity, string component, byte[] before, byte[] after, string writer, WorldKind world)
    {
        Entity = entity;
        Component = component;
        Before = before;
        After = after;
        Writer = writer;
        World = world;
    }

    /// <summary>The entity (an id of that client world: the same number may name another entity in the server world).</summary>
    public EntityId Entity { get; }

    /// <summary>The component's short name, as the program wrote it.</summary>
    public string Component { get; }

    /// <summary>The component's bytes before the write.</summary>
    public ReadOnlyMemory<byte> Before { get; }

    /// <summary>The bytes written.</summary>
    public ReadOnlyMemory<byte> After { get; }

    /// <summary>Who wrote: <c>"&lt;modid&gt;/program:&lt;key&gt;"</c> (the program's key in the mod's kiln.programs.json).</summary>
    public string Writer { get; }

    /// <summary><see cref="WorldKind.Client"/>, or <see cref="WorldKind.Prediction"/> for a prediction program.</summary>
    public WorldKind World { get; }

    /// <inheritdoc/>
    public bool Involves(EntityId entity) => entity == Entity;
}

internal static class ClientWrites
{
#pragma warning disable CA2255  // a service assembly: the hook must be there as soon as anything of this assembly loads
    [ModuleInitializer]
#pragma warning restore CA2255
    internal static void Install()
    {
        EcsPrograms.ClientWriteWanted = static () => Events.HasSubscribers<ClientComponentWrittenEvent>();
        // the main thread's queue of the client world's events: full (65536 waiting) or no client world session drops it
        // (counted by ClientEvents, logged once); the write stands
        EcsPrograms.ClientWritten = static (entity, component, before, after, writer, world) =>
            ClientEvents.Enqueue(new ClientComponentWrittenEvent(entity, component, before, after, writer, world));
    }
}
