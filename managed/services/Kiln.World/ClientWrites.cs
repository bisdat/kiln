// SPDX-License-Identifier: MIT
// ClientComponentWrittenEvent (chapter 14 section 9.3, since 0.2.0-dev.29): a write a client or prediction program of a
// mod made through EcsProgramContext.TryWrite / TryWriteOther (Kiln.Api's EcsPrograms.TryBind) in a client's world. Kiln.Api
// cannot name this assembly's types, so this assembly hands it two delegates when it loads: one says (lock-free) whether
// anything listens, the other queues the event from the worker that made the write to the tick thread's next step.
using System.Runtime.CompilerServices;

namespace Kiln;

/// <summary>A mod's client or prediction program (<see cref="EcsPrograms.TryBind(string, EcsProgramCallback, WorldKind)"/>)
/// changed a component of an entity of this player's client world (or prediction storage) through
/// <see cref="EcsProgramContext.TryWrite{T}(string, T)"/> or <see cref="EcsProgramContext.TryWriteOther{T}(EntityId, string, T)"/>.
/// The client world is this player's process only: the write is local presentation state, never saved or sent to anyone,
/// and the server world's <see cref="ComponentWrittenEvent"/> is not raised for it. Queued by the program's thread (a game
/// worker or the main thread) and raised at the start of the next step on the tick thread (a joined client's: the main
/// thread; a host's: its server thread): act on the client world from a handler through <c>Client.Post</c>. Built only
/// while the event has subscribers; one event per write. Since 0.2.0-dev.29.</summary>
[ClientOnly]
public sealed class ClientComponentWrittenEvent : KilnEvent, IEntityEvent
{
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
    private static long _dropped;

#pragma warning disable CA2255  // a service assembly: the hook must be there as soon as anything of this assembly loads
    [ModuleInitializer]
#pragma warning restore CA2255
    internal static void Install()
    {
        EcsPrograms.ClientWriteWanted = static () => Events.HasSubscribers<ClientComponentWrittenEvent>();
        EcsPrograms.ClientWritten = static (entity, component, before, after, writer, world) =>
        {
            try
            {
                Events.Enqueue(new ClientComponentWrittenEvent(entity, component, before, after, writer, world));
            }
            catch (InvalidOperationException)
            {
                // the tick thread's queue is full (65536 events waiting): this write's event is dropped, the write stands
                if (Interlocked.Increment(ref _dropped) == 1)
                    KilnLog.Warn("ClientComponentWrittenEvent: the event queue is full; events of client-world writes are dropped");
            }
        };
    }
}
