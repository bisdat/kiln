// SPDX-License-Identifier: MIT
// The seam between a Kiln service that keeps a value of its own on a saved entity across a restart (Kiln.World's
// transform scale: the game's save does not keep it) and Kiln.Saves, which owns entity attachments (an entity is known
// across a restart by its template and its position at the save; docs/kiln/services/saves.md "Entities"). The two
// services do not reference each other: Kiln.Saves sets the store when it starts, the service registers its scope here.
// A scope is "kiln:<service>": a ':' is never in a mod id, so a scope never meets a mod's own EntityData. Tick thread.
using System.Runtime.CompilerServices;

namespace Kiln;

internal static class ServiceEntityData
{
    /// <summary>Kiln.Saves' side: one JSON value per (entity, scope, key), saved and matched like a mod's EntityData.</summary>
    internal interface IStore
    {
        /// <summary>Records <paramref name="json"/> (null: removes the key). True when the entity has a positional identity
        /// that a save keeps (a known, saved world template), so the value comes back after a restart; false (nothing
        /// recorded) for a session-only entity or when kiln.saves cannot keep data now.</summary>
        bool Set(EntityId entity, string scope, string key, string? json);

        /// <summary>The value recorded for the entity, or null.</summary>
        string? Get(EntityId entity, string scope, string key);

        /// <summary>Live entities that have values of <paramref name="scope"/> (matched to their entity).</summary>
        IReadOnlyList<EntityId> Entities(string scope);

        /// <summary>The keys and values of <paramref name="scope"/> on a live entity (empty when none).</summary>
        IReadOnlyDictionary<string, string> Values(EntityId entity, string scope);
    }

    /// <summary>What a service is told for its scope.</summary>
    internal sealed class Consumer
    {
        /// <summary>A loaded entity got the values saved for it (after a restart, once kiln.saves matched it).</summary>
        public Action<EntityId, IReadOnlyDictionary<string, string>>? Loaded;

        /// <summary>Right before kiln.saves writes its data for a save (refresh what is recorded).</summary>
        public Action? Staging;
    }

    private static readonly Dictionary<string, Consumer> Consumers = new(StringComparer.Ordinal);
    private static bool _tried;

    /// <summary>Set by Kiln.Saves when it starts; null without it.</summary>
    internal static IStore? Store;

    /// <summary>True for a scope name of this seam (<c>kiln:&lt;service&gt;</c>), never a mod id.</summary>
    internal static bool IsScope(string name) => name.StartsWith("kiln:", StringComparison.Ordinal);

    /// <summary>A service registers its scope (once). Values already matched to live entities are handed to it now.</summary>
    internal static void Register(string scope, Consumer consumer)
    {
        if (!IsScope(scope)) throw new ArgumentException("a service scope is kiln:<service>", nameof(scope));
        Consumers[scope] = consumer;
        if (Store is not { } s || consumer.Loaded is not { } loaded) return;
        foreach (var e in s.Entities(scope))
            Run(scope, "loaded", () => loaded(e, s.Values(e, scope)));
    }

    /// <summary>The store, starting Kiln.Saves once if nothing has (a service whose values must be found again at the next
    /// start needs it running from the start). Null without the assembly (unit tests, an install without it).</summary>
    internal static IStore? Ensure()
    {
        if (Store is null && !_tried)
        {
            _tried = true;
            try
            {
                if (Type.GetType("Kiln.SavesRuntime, Kiln.Saves") is { } t) RuntimeHelpers.RunModuleConstructor(t.Module.ModuleHandle);
            }
            catch (Exception e)
            {
                Backend.Current.Log(0, LogLevel.Warn, $"kiln: Kiln.Saves could not be started for the services' entity data: {e.Message}");
            }
        }
        return Store;
    }

    // ---- Kiln.Saves calls these (tick thread, service context) ----

    internal static void RaiseLoaded(string scope, EntityId entity, IReadOnlyDictionary<string, string> values)
    {
        if (Consumers.TryGetValue(scope, out var c) && c.Loaded is { } loaded) Run(scope, "loaded", () => loaded(entity, values));
    }

    internal static void RaiseStaging()
    {
        foreach (var (scope, c) in Consumers.ToList())
            if (c.Staging is { } staging) Run(scope, "staging", staging);
    }

    private static void Run(string scope, string what, Action action)
    {
        try
        {
            action();
        }
        catch (Exception e)
        {
            Backend.Current.Log(0, LogLevel.Error, $"kiln: {scope} entity data ({what}) threw: {e}");
        }
    }
}
