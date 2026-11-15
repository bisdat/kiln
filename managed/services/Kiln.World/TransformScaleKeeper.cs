// SPDX-License-Identifier: MIT
// A transform scale set with World.SetTransformScale survives a save and restart (0.2.0-dev.28 fix round). The game's
// save does not keep it: a placed prop is saved without the scale of its CurrentTransform and comes back at its
// template's (lab: docs/re/world.md "Transform scale and the save"). So Kiln keeps it beside the save, in kiln.saves'
// entity attachments under the scope "kiln:world" (template + position at the save, as a mod's EntityData), and sets it
// again through the same tracked write when kiln.saves matches the loaded entity. Timing and visibility to a joined
// player have not been verified. The value is set again in the context of the mod that set it,
// and only while that mod is loaded and lists CurrentTransform under "writes": removing the mod prevents replay on
// the next load, but does not restore a scale already written in this session (the record stays for reinstall).
// Setting the scale the entity had before Kiln's first write (its
// game scale) forgets the record. Guide: docs/kiln/services/world.md "Transform scale".
||||||| 1224a4ff
// A transform scale set with World.SetTransformScale survives a save and restart (0.2.0-dev.28 fix round). The game's
// save does not keep it: a placed prop is saved without the scale of its CurrentTransform and comes back at its
// template's (lab: docs/re/world.md "Transform scale and the save"). So Kiln keeps it beside the save, in kiln.saves'
// entity attachments under the scope "kiln:world" (template + position at the save, as a mod's EntityData), and sets it
// again through the same tracked write when kiln.saves matches the loaded entity: on a dedicated server in the first
// second the world runs, before a player can be in it. The value is set again in the context of the mod that set it,
// and only while that mod is loaded and lists CurrentTransform under "writes": a removed mod's scales go with it (the
// record stays, so the scale comes back with the mod). Setting the scale the entity had before Kiln's first write (its
// game scale) forgets the record. Guide: docs/kiln/services/world.md "Transform scale".
using System.Runtime.InteropServices;
using System.Text.Json;
using System.Text.Json.Serialization;

namespace Kiln;

internal static class TransformScaleKeeper
{
    internal const string Scope = "kiln:world";
    internal const string Key = "transform-scale";
    private const ulong CheckEverySteps = 25;

    /// <summary>One entity's kept scale: the mod that set it, the scale, and the game's own scale before Kiln's first write.</summary>
    internal sealed record Saved(
        [property: JsonPropertyName("mod")] string Mod,
        [property: JsonPropertyName("scale")] Scale3 Scale,
        [property: JsonPropertyName("base")] Scale3 Base);

    private sealed class Waiting
    {
        public required Saved Saved;
        public string? Noted;  // the last reason logged for it (logged once per reason)
    }

    private static readonly Dictionary<uint, Waiting> ToApply = new();
    private static readonly HashSet<uint> Applied = new();  // entities whose kept scale this session wrote or set again
    private static bool _registered, _ticking, _recording;
    private static int _notTheEntity;  // waiting scales dropped because their id named another entity (tests, diagnostics)

    internal static string Json(Saved s) => JsonSerializer.Serialize(s);

    internal static Saved? Parse(string? json)
    {
        if (json is null) return null;
        try
        {
            return JsonSerializer.Deserialize<Saved>(json) is { Mod: not null } s && s.Scale.IsValid && s.Base.IsValid ? s : null;
        }
        catch (JsonException)
        {
            return null;
        }
    }

    /// <summary>Kiln.World's start (first service tick): watches for the moment kiln.saves is needed. Kiln.Saves is
    /// started only when a scale could be set at all (the row is on and a loaded mod lists CurrentTransform), so a server
    /// that never scales anything does not get a sidecar for it.</summary>
    internal static void Start()
    {
        if (_ticking) return;
        _ticking = true;
        ServiceTick.Add("kiln.world transform scale keeper", static (in TickInfo t) =>
        {
            if (t.Step % CheckEverySteps != 0) return;
            if (!_registered && CanSetAny()) Register();
            if (ToApply.Count > 0) ApplyWaiting();
        });
    }

    private static bool CanSetAny() =>
        Capabilities.Is(TransformScale.Capability) && Capabilities.Is(World.TrackedWritesCapability) &&
        WorldWrites.Info(TransformScale.Component, null).State is WorldWriteState.Ready or WorldWriteState.Pending;

    private static ServiceEntityData.IStore? Register()
    {
        var store = ServiceEntityData.Ensure();
        if (store is null || _registered) return store;
        _registered = true;
        ServiceEntityData.Register(Scope, new ServiceEntityData.Consumer { Loaded = OnLoaded, Staging = OnStaging, WorldChanged = OnWorldChanged });
        return store;
    }

    /// <summary>After a committed <see cref="World.SetTransformScale"/> (tick thread, the mod's context): records the scale
    /// for the next start, or forgets it when it is the game's own. True when replay was recorded or the original scale restored,
    /// without guaranteeing future identity or size; false when it is for this session only (Kiln's own tools, an entity the save
    /// does not keep, kiln.saves off).</summary>
    internal static bool Record(EntityId entity, Scale3? before, Scale3 now, string? modId)
    {
        if (modId is null) return false;
        ServiceEntityData.IStore? store;
        _recording = true;  // a first registration hands over kept scales: queued, never set over this write
        try
        {
            store = Register();
        }
        finally
        {
            _recording = false;
        }
        if (store is null) return false;
        ToApply.Remove(entity.Value);
        var baseScale = Parse(store.Get(entity, Scope, Key))?.Base ?? before ?? now;
        if (now == baseScale)
        {
            Applied.Remove(entity.Value);
            return store.Set(entity, Scope, Key, null);
        }
        var kept = store.Set(entity, Scope, Key, Json(new Saved(modId, now, baseScale)));
        if (kept) Applied.Add(entity.Value);
        else Applied.Remove(entity.Value);
        return kept;
    }

    // kiln.saves matched a loaded entity to its kept scale (tick thread, between steps)
    private static void OnLoaded(EntityId entity, IReadOnlyDictionary<string, string> values)
    {
        if (!values.TryGetValue(Key, out var json)) return;
        if (Parse(json) is not { } saved)
        {
            Backend.Current.Log(0, LogLevel.Warn, $"kiln.world: the transform scale kept for entity {entity} does not read ({json}); left as the game loaded it");
            return;
        }
        Queue(entity, saved);
        if (!_recording) ApplyWaiting();
    }

    internal static void Queue(EntityId entity, Saved saved) => ToApply[entity.Value] = new Waiting { Saved = saved };

    /// <summary>Tests: kept scales waiting to be set again, and scales written this session.</summary>
    internal static (int Waiting, int Applied, int NotTheEntity) Counts => (ToApply.Count, Applied.Count, _notTheEntity);

    // kiln.saves let the world's data go (a hosted world closed, another world loads): ids from before name nothing now
    internal static void OnWorldChanged()
    {
        ToApply.Clear();
        Applied.Clear();
    }

    internal static void ApplyWaiting()
    {
        foreach (var (id, w) in ToApply.ToList())
        {
            var entity = new EntityId(id);
            // the id may have been handed out again since the match: only the entity the kept record is on gets it
            if (ServiceEntityData.Store is not { } store || !store.Holds(entity, Scope, Key))
            {
                ToApply.Remove(id);
                _notTheEntity++;
                Backend.Current.Log(0, LogLevel.Info, $"kiln.world: the transform scale {w.Saved.Scale} kept for entity {entity} is dropped: " +
                                                      "the entity is no longer the one it was kept for");
                continue;
            }
            var mod = Mods.Get(w.Saved.Mod)?.Info is { } info ? ModContext.FindLive(info) : null;
            string? wait = null, stop = null;
            if (mod is null) stop = $"mod '{w.Saved.Mod}' is not loaded (the scale comes back with it)";
            else if (!Capabilities.Is(TransformScale.Capability)) wait = $"{TransformScale.Capability} is off: {Capabilities.Why(TransformScale.Capability)}";
            else if (!Capabilities.Is(World.TrackedWritesCapability)) wait = $"{World.TrackedWritesCapability} is off: {Capabilities.Why(World.TrackedWritesCapability)}";
            else
            {
                var state = WorldWrites.Info(TransformScale.Component, mod.Info.Id);
                if (state.State == WorldWriteState.Pending) wait = "the write program waits for the server world";
                else if (state.State != WorldWriteState.Ready) stop = state.Reason ?? state.State.ToString();
            }
            if (stop is not null || wait is not null)
            {
                var why = stop ?? wait!;
                if (w.Noted != why)
                {
                    w.Noted = why;
                    Backend.Current.Log(0, LogLevel.Info, $"kiln.world: the transform scale {w.Saved.Scale} kept for entity {entity} is not set again " +
                                                          $"{(stop is not null ? "this session" : "yet")}: {why}");
                }
                if (stop is not null) ToApply.Remove(id);
                continue;
            }
            ToApply.Remove(id);
            ModInvoke.Run(mod, () => _ = SetAgain(entity, w.Saved), "kiln.world: setting a kept transform scale again");
        }
    }

    private static async Task SetAgain(EntityId entity, Saved saved)
    {
        EntityScaleResult r;
        try
        {
            r = await World.SetTransformScale(entity, saved.Scale);
        }
        catch (Exception e) when (e is KilnUnavailableException or ArgumentException or InvalidOperationException)
        {
            Backend.Current.Log(0, LogLevel.Warn, $"kiln.world: the transform scale {saved.Scale} kept for entity {entity} could not be set again: {e.Message}");
            return;
        }
        Backend.Current.Log(0, r.Ok ? LogLevel.Info : LogLevel.Warn,
            $"kiln.world: transform scale of entity {entity} set again after the load for mod '{saved.Mod}': {r.WorldWriteStatus}, " +
            $"{r.Before?.ToString() ?? "?"} -> {r.Now?.ToString() ?? "?"} (kept {saved.Scale}, game scale {saved.Base}){(r.Ok ? "" : ": " + r.Reason)}");
    }

    // Right before kiln.saves writes its data: a kept scale that this session wrote follows what the entity has now (the
    // game or another tool may have changed it since); back at the game's scale, the record goes.
    private static void OnStaging()
    {
        if (ServiceEntityData.Store is not { } store || Applied.Count == 0 || TransformScale.CurrentLayout is not { } layout) return;
        var live = store.Entities(Scope).Select(e => e.Value).ToHashSet();
        Applied.IntersectWith(live);
        foreach (var id in Applied.ToList())
        {
            var e = new EntityId(id);
            if (Parse(store.Get(e, Scope, Key)) is not { } saved) continue;
            if (World.ReadBytes(e, TransformScale.Component) is not { } bytes || bytes.Length < layout.Offset + 12) continue;
            var now = MemoryMarshal.Read<Scale3>(bytes.AsSpan(layout.Offset));
            if (now == saved.Scale || !now.IsValid) continue;
            if (now == saved.Base)
            {
                store.Set(e, Scope, Key, null);
                Applied.Remove(id);
            }
            else store.Set(e, Scope, Key, Json(saved with { Scale = now }));
        }
    }
}
