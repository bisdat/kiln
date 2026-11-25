// SPDX-License-Identifier: MIT
// ClientStations (since 0.2.0-dev.31.13, mod API 0.2.22): the production stations a game client knows from its own world,
// read-only, for a player who joined someone else's world. Native: kiln.stations.client (native/core/abi/services/
// kiln_stations_client.h, in kiln-items.dll); guide: docs/kiln/services/stations.md ("On a joined client: the client's own
// station view"); evidence: docs/re/stations.md section 13.
using System.Runtime.InteropServices;

namespace Kiln;

/// <summary>The blittable mirror of kiln.stations.client v1: the view 232 bytes, the status 192, the table 32 (x64).</summary>
internal static unsafe class ClientStationsNative
{
    public const string Service = "kiln.stations.client";
    public const uint Version = 1;
    public const int MaxOutputs = 8;
    public const uint FlagRecipe = 0x1, FlagTime = 0x2, FlagOutputs = 0x4, FlagEnergy = 0x8;
    public const int Ok = 0, InvalidArgument = 1, NotFound = 2, Unavailable = 3, BufferTooSmall = 5;

    [StructLayout(LayoutKind.Sequential)]
    public struct View
    {
        public uint Size, Entity, Workshop, SelectedRecipe;
        public StationsNative.BaseId Base;
        public float X, Y, Z;
        public byte State, Needs, UseAllOptions, Reserved0;
        public StationsNative.Choice C0, C1, C2, C3;
        public uint CraftsLoaded;
        public int Water, WaterMax;
        public uint Flags;
        public long RecipeStartNs, NowNs, CraftNs, CraftRemainingNs, RemainingNs;
        public float Progress;
        public uint OutputCount;
        public fixed uint Outputs[MaxOutputs * 2];  // {item, count} x 8
        public ulong Step;
        public fixed uint Reserved[4];
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct Status
    {
        public uint Size;
        public byte On, World, Copying, Reserved0;
        public uint Rows, Reserved1;
        public ulong Step;
        public long AgeMs;
        public fixed byte Detail[160];
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct Table
    {
        public ServiceHeader Header;
        public delegate* unmanaged[Cdecl]<Status*, int> GetStatus;
        public delegate* unmanaged[Cdecl]<View*, uint, uint*, int> Stations;
        public delegate* unmanaged[Cdecl]<uint, View*, int> Station;
    }

    private static Table* _table;
    private static Table* _forTests;
    private static bool _testSeam;
    private static readonly object Lock = new();

    /// <summary>The layout this assembly was built with is the header's (232 / 192 / 32 bytes): checked before the first
    /// lookup, so a drift refuses the table instead of misreading it.</summary>
    internal static bool LayoutOk => sizeof(View) == 232 && sizeof(Status) == 192 && sizeof(Table) == 32;

    /// <summary>The table, or null when kiln-items.dll does not serve it (a dedicated server serves it too, answering
    /// unavailable; an older kiln-items.dll, the test kit without its fake have none). Looked up until found.</summary>
    public static Table* T
    {
        get
        {
            if (_testSeam) return _forTests;
            if (_table != null) return _table;
            if (!LayoutOk) return null;
            lock (Lock)
            {
                if (_table != null) return _table;
                var t = Services.Get<Table>(Service, Version);
                return _table = t != null && t->Header.Size >= (uint)sizeof(Table) && t->Stations != null ? t : null;
            }
        }
    }

    internal static void UseForTests(Table* table)
    {
        lock (Lock)
        {
            _forTests = table;
            _testSeam = true;
            _table = null;
        }
    }

    internal static void ResetForTests()
    {
        lock (Lock)
        {
            _forTests = null;
            _testSeam = false;
            _table = null;
        }
    }

    public static string Text(byte* p, int cap)
    {
        var n = 0;
        while (n < cap && p[n] != 0) n++;
        return System.Text.Encoding.UTF8.GetString(p, n);
    }
}

/// <summary>
/// The production stations (kilns, smelters, looms, seed beds: every station with the game's <c>FactoryStation</c>) as
/// the game client itself knows them, read-only: for a player who joined someone else's world, where
/// <see cref="Stations"/> has no server world and throws, and also on a hosting player's own client. Since
/// 0.2.0-dev.31.13, mod API 0.2.22. Thanks XHL.
/// </summary>
/// <remarks>
/// <para><b>Where it comes from.</b> The server sends each player the stations of the bases around them
/// (<c>ServerFactoryInfo</c>), and the client keeps them in its world's station cache (<c>ClientFactoryInfo</c>), the one
/// the game's own station HUD and station screen read. Kiln copies that cache after each client step while a mod reads
/// it. Only a station standing in a base (a flame altar's zone) the client knows is in it; which stations the server
/// sends to which player, and from how far, was not traced: read <see cref="All"/> and filter by distance
/// (<see cref="Near"/>).</para>
/// <para><b>What may be stale or missing.</b> A row is as fresh as the server's last update of it (it sends one when the
/// station's state changes or a player acts on it). Not in the cache: the running recipe (when the selection changed during
/// a cycle, the cycle in progress is of the earlier recipe), the pause time, and a "stop after this craft" that waits. The
/// times (<see cref="ClientStationInfo.Remaining"/>) are the game's HUD arithmetic on the client's clock and its own copy of
/// the production speed setting; the finished outputs are read from the client world's copy of the station's inventory,
/// null when the client has none.</para>
/// <para><b>Threads.</b> Any thread: every read answers from the copy of the client world's last step. The first read
/// after a pause of 5 seconds starts the copying and answers from the previous copy (or none yet: empty, null): poll it every
/// frame or every second, as a HUD does. Nothing here writes or sends anything: acting on a station is still your server
/// half's (<see cref="Stations"/>, through <c>Kiln.Network</c> messages).</para>
/// <para><b>Off.</b> <see cref="Capability"/> (<c>stations.client_view</c>) is off on a dedicated server, on a game build
/// whose cache layout sigdb does not qualify, and after a fault; then every member but <see cref="Available"/> and
/// <see cref="Status"/> throws <see cref="KilnUnavailableException"/> naming it.</para>
/// </remarks>
public static unsafe class ClientStations
{
    /// <summary>The capability row: the client's station cache can be read here (sigdb feature, since
    /// 0.2.0-dev.31.13, mod API 0.2.22).</summary>
    public const string Capability = "stations.client_view";

    internal static readonly ServiceCapRows Rows = new(ClientStationsNative.Service, LowerWithoutService);

    static ClientStations() => Rows.Ensure();

    private static void LowerWithoutService()
    {
        if (ClientStationsNative.T == null) ServiceCaps.LowerFeatures(ClientStationsNative.Service, Capability);
    }

    /// <summary>The view works here now: a game client whose build qualifies the station cache, nothing faulted. Since
    /// 0.2.0-dev.31.13, mod API 0.2.22.</summary>
    [AnyThread]
    public static bool Available => Status is { On: true };

    /// <summary>What works and the copy's age; null when kiln.stations.client is not here. Since 0.2.0-dev.31.13, mod API
    /// 0.2.22.</summary>
    [AnyThread]
    public static ClientStationsStatus? Status
    {
        get
        {
            var t = ClientStationsNative.T;
            if (t == null) return null;
            ClientStationsNative.Status s = default;
            s.Size = (uint)sizeof(ClientStationsNative.Status);
            if (t->GetStatus(&s) != ClientStationsNative.Ok) return null;
            return new ClientStationsStatus(s.On != 0, s.World != 0, s.Copying != 0, (int)s.Rows, s.Step,
                s.AgeMs >= 0 ? TimeSpan.FromMilliseconds(s.AgeMs) : null, ClientStationsNative.Text(s.Detail, 160));
        }
    }

    /// <summary>Every station in the client's cache, in the cache's order (grouped by base). Empty before the first copy.
    /// Since 0.2.0-dev.31.13, mod API 0.2.22.</summary>
    /// <exception cref="KilnUnavailableException"><see cref="Capability"/> is off.</exception>
    [AnyThread, ClientOnly, RequiresCapability(Capability)]
    public static IReadOnlyList<ClientStationInfo> All()
    {
        var t = Require("ClientStations.All");
        var buf = new ClientStationsNative.View[64];
        while (true)
        {
            for (var i = 0; i < buf.Length; i++) buf[i].Size = (uint)sizeof(ClientStationsNative.View);
            uint n = 0;
            int rc;
            fixed (ClientStationsNative.View* p = buf) rc = t->Stations(p, (uint)buf.Length, &n);
            if (rc == ClientStationsNative.BufferTooSmall && n > buf.Length && n <= 4096)
            {
                buf = new ClientStationsNative.View[n];
                continue;
            }
            Check(rc == ClientStationsNative.BufferTooSmall ? ClientStationsNative.Ok : rc, "ClientStations.All");
            var list = new List<ClientStationInfo>((int)Math.Min(n, (uint)buf.Length));
            for (var i = 0; i < Math.Min(n, (uint)buf.Length); i++) list.Add(Convert(in buf[i]));
            return list;
        }
    }

    /// <summary>The stations of the cache within <paramref name="radius"/> metres of <paramref name="center"/> (your
    /// player's or the camera's position), nearest first. Since 0.2.0-dev.31.13, mod API 0.2.22.</summary>
    /// <exception cref="ArgumentOutOfRangeException"><paramref name="radius"/> is negative or not a number.</exception>
    /// <exception cref="KilnUnavailableException"><see cref="Capability"/> is off.</exception>
    [AnyThread, ClientOnly, RequiresCapability(Capability)]
    public static IReadOnlyList<ClientStationInfo> Near(WorldPosition center, double radius = 16)
    {
        if (!(radius >= 0)) throw new ArgumentOutOfRangeException(nameof(radius), radius, "a distance in metres, 0 or more");
        return All().Where(s => s.Position.DistanceTo(center) <= radius).OrderBy(s => s.Position.DistanceTo(center)).ToList();
    }

    /// <summary>One station of the cache; null when the client's cache does not have it (not in a known base, not sent,
    /// not copied yet). Since 0.2.0-dev.31.13, mod API 0.2.22.</summary>
    /// <exception cref="KilnUnavailableException"><see cref="Capability"/> is off.</exception>
    [AnyThread, ClientOnly, RequiresCapability(Capability)]
    public static ClientStationInfo? Read(EntityId station)
    {
        var t = Require("ClientStations.Read");
        ClientStationsNative.View v = default;
        v.Size = (uint)sizeof(ClientStationsNative.View);
        var rc = t->Station(station.Value, &v);
        if (rc == ClientStationsNative.NotFound) return null;
        Check(rc, "ClientStations.Read");
        return Convert(in v);
    }

    /// <summary>The station whose screen the local player has open (the game's production-station screen, the entity of
    /// <c>Ui.Screens.Current.Target</c>), read from the cache; null when no station screen is open or the cache does not
    /// have it. Since 0.2.0-dev.31.13, mod API 0.2.22.</summary>
    /// <exception cref="KilnUnavailableException"><see cref="Capability"/> is off.</exception>
    [AnyThread, ClientOnly, RequiresCapability(Capability)]
    public static ClientStationInfo? Open
    {
        get
        {
            Require("ClientStations.Open");
            var screen = Ui.Screens.Current;
            if (screen.Hud != HudScreen.FactoryCrafting || screen.Target == EntityId.None) return null;
            return Read(screen.Target);
        }
    }

    private static ClientStationsNative.Table* Require(string api)
    {
        var t = ClientStationsNative.T;
        if (t == null) throw ServiceCaps.Unavailable(Capability, $"{api}: kiln.stations.client is not loaded");
        if (!Capabilities.Is(Capability)) throw ServiceCaps.Unavailable(Capability, $"{api}: the client's station view is off");
        return t;
    }

    private static void Check(int rc, string api)
    {
        switch (rc)
        {
            case ClientStationsNative.Ok: return;
            case ClientStationsNative.Unavailable:
                var detail = Status?.Detail is { Length: > 0 } d ? d : "the client's station view is off here";
                throw ServiceCaps.Unavailable(Capability, $"{api}: {detail}");
            default: throw new InvalidOperationException($"{api}: kiln.stations.client answered {rc}");
        }
    }

    internal static ClientStationInfo Convert(in ClientStationsNative.View v)
    {
        var choices = new List<CategoryChoice>(4);
        foreach (var c in new[] { v.C0, v.C1, v.C2, v.C3 })
            if (c.Category != 0) choices.Add(new CategoryChoice(c.Category, c.Index));
        List<StationStack>? outputs = null;
        if ((v.Flags & ClientStationsNative.FlagOutputs) != 0)
        {
            outputs = new List<StationStack>((int)Math.Min(v.OutputCount, ClientStationsNative.MaxOutputs));
            fixed (uint* o = v.Outputs)
                for (var i = 0; i < Math.Min(v.OutputCount, (uint)ClientStationsNative.MaxOutputs); i++)
                    outputs.Add(new StationStack(new ItemId(o[2 * i]), (int)Math.Min(o[2 * i + 1], int.MaxValue)));
        }
        var recipe = (v.Flags & ClientStationsNative.FlagRecipe) != 0;
        var time = (v.Flags & ClientStationsNative.FlagTime) != 0;
        static TimeSpan Ns(long ns) => TimeSpan.FromTicks(ns / 100);
        return new ClientStationInfo(new EntityId(v.Entity), v.Workshop, v.SelectedRecipe, new BaseId(StationsNative.GuidOf(v.Base)),
            new WorldPosition(v.X, v.Y, v.Z), (StationState)v.State, (StationNeeds)v.Needs, choices, v.UseAllOptions != 0,
            (int)Math.Min(v.CraftsLoaded, int.MaxValue), v.Water, v.WaterMax, (v.Flags & ClientStationsNative.FlagEnergy) != 0,
            recipe ? Ns(v.CraftNs) : null, time ? Ns(v.CraftRemainingNs) : null, time ? Ns(v.RemainingNs) : null,
            time ? v.Progress : null, outputs, v.Step);
    }
}

/// <summary>What <see cref="ClientStations"/> works with now. Since 0.2.0-dev.31.13, mod API 0.2.22.</summary>
/// <param name="On">The client's station cache can be read on this build and nothing faulted.</param>
/// <param name="World">A client world runs now (in game, not in the menu).</param>
/// <param name="Copying">A read in the last 5 seconds keeps the copying on.</param>
/// <param name="Rows">Stations in the copy.</param>
/// <param name="Step">The client world's step of the copy (0: none yet).</param>
/// <param name="Age">Since the copy was taken; null when there is none.</param>
/// <param name="Detail">Why it is off, in words; empty when it works.</param>
public readonly record struct ClientStationsStatus(bool On, bool World, bool Copying, int Rows, ulong Step, TimeSpan? Age, string Detail);

/// <summary>One production station as the game client's own station cache has it (<see cref="ClientStations"/>). Since
/// 0.2.0-dev.31.13, mod API 0.2.22.</summary>
/// <param name="Station">The station (the server's entity id; the client uses the same ids).</param>
/// <param name="Workshop">Its workshop (<c>Crafting.workshopId</c>): which recipes it runs. Name it with
/// <see cref="DisplayName"/>.</param>
/// <param name="SelectedRecipe">The recipe it repeats; 0 when none is chosen. Name it with <see cref="RecipeName"/>.</param>
/// <param name="Base">The base the client grouped it under.</param>
/// <param name="Position">Where it stands, in 0.25 m steps (as sent).</param>
/// <param name="State">Its state as the server last sent it.</param>
/// <param name="Needs">What it lacks (the game's missing requirements).</param>
/// <param name="SelectedChoices">The category choices of the selected recipe ("any wood": which wood).</param>
/// <param name="UseAllOptions">"Use all ingredient options" is on.</param>
/// <param name="CraftsLoaded">The complete crafts its inputs still hold after the cycle in progress (the server's count).</param>
/// <param name="Water">Its water, in the game's Fuel units (64 per charge); 0 without water.</param>
/// <param name="WaterMax">Its water capacity, in Fuel units.</param>
/// <param name="NeedsEnergy">It runs on rotational energy (it needs a power source in range).</param>
/// <param name="CraftDuration">One craft of the selected recipe on this client (the recipe's time and the client's copy of
/// the production speed setting); null when the recipe is not in the client's data.</param>
/// <param name="CraftRemaining">What is left of the cycle in progress, at least zero; null unless <see cref="State"/> is
/// Running and the time is finite (as the game's HUD).</param>
/// <param name="Remaining">Until every loaded craft is done: the time the game's HUD shows ("finishes in"), at least zero;
/// null as <paramref name="CraftRemaining"/>.</param>
/// <param name="Progress">0..1 of the cycle in progress; null as <paramref name="CraftRemaining"/>.</param>
/// <param name="Outputs">Finished stacks of the selected recipe's outputs (an item that is no input of it) in the client
/// world's copy of the station's inventory, at most 8 items; null when the client has no copy of that inventory (or no
/// recipe data).</param>
/// <param name="Step">The client world's step of the copy.</param>
public sealed record ClientStationInfo(EntityId Station, uint Workshop, uint SelectedRecipe, BaseId Base, WorldPosition Position,
    StationState State, StationNeeds Needs, IReadOnlyList<CategoryChoice> SelectedChoices, bool UseAllOptions, int CraftsLoaded,
    int Water, int WaterMax, bool NeedsEnergy, TimeSpan? CraftDuration, TimeSpan? CraftRemaining, TimeSpan? Remaining,
    float? Progress, IReadOnlyList<StationStack>? Outputs, ulong Step)
{
    /// <summary>A cycle is running (<see cref="State"/> is Running).</summary>
    public bool IsRunning => State == StationState.Running;

    /// <summary>The workshop's name in the player's language ("Charcoal Kiln"), from the game's data
    /// (<see cref="Stations.WorkshopName"/>); null until the names are read.</summary>
    public string? DisplayName => Stations.WorkshopName(Workshop);

    /// <summary>The selected recipe's name (<see cref="Stations.RecipeName"/>); null without a recipe.</summary>
    public string? RecipeName => Stations.RecipeName(SelectedRecipe);

    /// <summary>The station's id, workshop, state and time on one line.</summary>
    public override string ToString() =>
        $"{Station} workshop 0x{Workshop:x8} {State} recipe 0x{SelectedRecipe:x8}, {CraftsLoaded} loaded" +
        (Remaining is { } r ? $", done in {r:hh\\:mm\\:ss}" : "");
}
