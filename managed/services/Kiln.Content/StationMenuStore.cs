// SPDX-License-Identifier: MIT
// Station menus with tabs of their own (chapter 16a; dev31 out/16a-station-tabs/DESIGN.md): the managed facade over
// kiln.content's table kiln.stationmenus (kiln_station_menus.h). Kiln.Blocks lowers a menu to the normalized record and
// calls Define/Remove here on the tick thread; this file stores it natively, decides publication on a server (content sync
// on, every connected Kiln client able to follow: StationMenuSync), and posts StationMenuDefinitionChangedEvent once per
// accepted change. docs/kiln/modding/world-blocks.md "Menus with tabs of their own".
using System.Runtime.InteropServices;
using System.Text;
using System.Text.Json;

namespace Kiln;

/// <summary>What a store call answered: <see cref="Ok"/>, or why not (a stable reason word first).</summary>
internal readonly record struct MenuStoreResult(bool Ok, string Reason, ulong Handle, ulong Generation);

/// <summary>The native side of the store; tests replace it (<see cref="StationMenuStore.TestBackend"/>).</summary>
internal interface IStationMenuBackend
{
    /// <summary>The table is here (stationmenus.store).</summary>
    bool Available { get; }
    /// <summary>KilnResult, the layer's handle, the local generation after the call, and the report's error text (or "").</summary>
    (int Rc, ulong Handle, ulong Generation, string State, string Error) Define(ulong owner, string record);
    (int Rc, ulong Generation) Remove(ulong owner, ulong handle);
    /// <summary>The effective local layouts: {"format":1,"generation":N,"menus":[...]} or null.</summary>
    string? Export();
    (int Rc, string Error) RemoteReplace(ulong owner, string snapshot);
    int RemoteClear(ulong owner);
    int SetPending(bool on);
    /// <summary>JSON array of materializations since the last call, or null.</summary>
    string? DrainApplied();
    StationMenuNative.Info? Info();
}

internal static class StationMenuStore
{
    internal const string StoreRow = "stationmenus.store", RenderRow = "stationmenus.render", SyncRow = "stationmenus.sync.v1";

    // stable refusal words (MenuDefinitionResult.Reason starts with one)
    internal const string PendingContent = "PendingContent", ContentSyncDisabled = "ContentSyncDisabled",
        PeersLack = "PeersLackStationMenus", NoHoldGate = "NoHoldGate", Refused = "Refused";

    internal static IStationMenuBackend? TestBackend { get; set; }
    internal static IStationMenuBackend Backend => TestBackend ?? StationMenuNative.Instance;

    /// <summary>The store is here: kiln.content's kiln.stationmenus table (or a test backend).</summary>
    internal static bool Available => Backend.Available;

    /// <summary>Throws <see cref="KilnUnavailableException"/> naming stationmenus.store when the table is missing.</summary>
    internal static void RequireStore()
    {
        if (!Available) throw new KilnUnavailableException(StoreRow, "kiln.content has no station-menu store on this build (kiln.stationmenus; update Kiln)");
    }

    private sealed record Layer(ulong Owner, ulong Handle, Guid Definition, uint Workshop, string Mod, string Record);

    private static readonly object Lock = new();
    private static readonly Dictionary<ulong, Layer> Layers = new();   // handle -> layer
    private static ulong _generation;

    /// <summary>The local collection's generation (0: nothing defined yet).</summary>
    internal static ulong Generation
    {
        get { lock (Lock) return _generation; }
    }

    /// <summary>Layers now (status, the sync's "is there any menu").</summary>
    internal static int Count
    {
        get { lock (Lock) return Layers.Count; }
    }

    /// <summary>Stores <paramref name="record"/> (the normalized format-1 record) as <paramref name="owner"/>'s layer for
    /// its definition. Tick thread. A bad record throws <see cref="ArgumentException"/>; a refusal (a server whose
    /// clients cannot follow, content sync off) answers Ok=false with the reason and changes nothing.</summary>
    internal static MenuStoreResult Define(ulong owner, string modId, string record, Guid definitionId, uint workshop)
    {
        ArgumentException.ThrowIfNullOrEmpty(record);
        RequireStore();
        Services.RequireTickThread("StationMenus.Define");
        bool first;
        lock (Lock) first = !Layers.Values.Any(l => l.Owner == owner && l.Definition == definitionId) && Layers.Count == 0;
        if (StationMenuSync.CheckPublish(first) is { } refusal) return new(false, refusal, 0, Generation);
        var (rc, handle, generation, state, error) = Backend.Define(owner, record);
        if (rc == StationMenuNative.InvalidArgument) throw new ArgumentException($"station menu: {(error.Length > 0 ? error : "the record was refused")}", nameof(record));
        if (rc == StationMenuNative.Revoked) return new(false, $"{Refused}: the owner is gone", 0, Generation);
        if (rc != StationMenuNative.Ok) return new(false, $"{Refused}: {(error.Length > 0 ? error : StationMenuNative.Message(rc))}", 0, Generation);
        bool changed;
        lock (Lock)
        {
            changed = state != "unchanged";
            Layers[handle] = new Layer(owner, handle, definitionId, workshop, modId, record);
            _generation = generation;
        }
        if (changed)
        {
            StationMenuSync.Published();
            Post(definitionId, workshop, generation, modId, removed: false);
        }
        return new(true, "", handle, generation);
    }

    /// <summary>Takes a layer off. Tick thread. The wrong owner throws <see cref="ArgumentException"/>; a second call is a no-op.</summary>
    internal static MenuStoreResult Remove(ulong owner, ulong handle)
    {
        Layer? layer;
        lock (Lock) Layers.TryGetValue(handle, out layer);
        if (layer is null) return new(true, "", handle, Generation);
        if (layer.Owner != owner) throw new ArgumentException("this station menu belongs to another mod", nameof(owner));
        if (Available)
        {
            Services.RequireTickThread("StationMenuRegistration.Dispose");
            var (rc, generation) = Backend.Remove(owner, handle);
            if (rc is not (StationMenuNative.Ok or StationMenuNative.NotFound)) return new(false, $"{Refused}: {StationMenuNative.Message(rc)}", handle, Generation);
            lock (Lock)
            {
                Layers.Remove(handle);
                if (generation != 0) _generation = generation;
            }
        }
        else lock (Lock) Layers.Remove(handle);
        StationMenuSync.Published();
        Post(layer.Definition, layer.Workshop, Generation, layer.Mod, removed: true);
        return new(true, "", handle, Generation);
    }

    /// <summary>Every layer of <paramref name="owner"/> goes (its mod retired). Tick thread.</summary>
    internal static int RemoveOwner(ulong owner)
    {
        Layer[] mine;
        lock (Lock) mine = Layers.Values.Where(l => l.Owner == owner).ToArray();
        foreach (var l in mine) Remove(owner, l.Handle);
        return mine.Length;
    }

    private static void Post(Guid definition, uint workshop, ulong generation, string mod, bool removed)
    {
        if (Events.HasSubscribers<StationMenuDefinitionChangedEvent>())
            Events.Post(new StationMenuDefinitionChangedEvent(definition, workshop, generation, mod, removed));
    }

    /// <summary>Tests: forget every layer.</summary>
    internal static void ResetForTests()
    {
        lock (Lock)
        {
            Layers.Clear();
            _generation = 0;
        }
    }
}

/// <summary>A station's menu layout was accepted, replaced or taken off (another layer, or the game's own layout, shows
/// again). On the tick thread, once per change; an identical definition posts nothing.</summary>
public sealed class StationMenuDefinitionChangedEvent : KilnEvent
{
    internal StationMenuDefinitionChangedEvent(Guid definitionId, uint workshop, ulong generation, string modId, bool removed)
    {
        DefinitionId = definitionId;
        Workshop = workshop;
        Generation = generation;
        ModId = modId;
        Removed = removed;
    }

    /// <summary>The definition (the layer): <c>BlockIds.Derive(mod, "menu-layout", key)</c>.</summary>
    public Guid DefinitionId { get; }
    /// <summary>The station's own workshop id.</summary>
    public uint Workshop { get; }
    /// <summary>The local collection's generation after the change.</summary>
    public ulong Generation { get; }
    /// <summary>The mod that defined the layer.</summary>
    public string ModId { get; }
    /// <summary>True: the layer was taken off.</summary>
    public bool Removed { get; }
}

/// <summary>Client: a station's menu was built from its layout, or refused (<see cref="Ok"/> false, <see cref="Reason"/>:
/// <c>IconSourceMissing</c>, <c>RestartRequired</c>, ...). Posted on the tick thread after the game built the menu, never
/// from inside the game's UI code.</summary>
[ClientOnly]
public sealed class StationMenuAppliedEvent : KilnEvent
{
    internal StationMenuAppliedEvent(Guid definitionId, uint workshop, Guid session, ulong generation, bool fromServer, bool ok, string reason)
    {
        DefinitionId = definitionId;
        Workshop = workshop;
        Session = session;
        Generation = generation;
        FromServer = fromServer;
        Ok = ok;
        Reason = reason;
    }

    public Guid DefinitionId { get; }
    public uint Workshop { get; }
    /// <summary>The server's session of the layout (<see cref="Guid.Empty"/> for a local one).</summary>
    public Guid Session { get; }
    public ulong Generation { get; }
    /// <summary>The layout came from the server (a joined game).</summary>
    public bool FromServer { get; }
    public bool Ok { get; }
    public string Reason { get; }
}
