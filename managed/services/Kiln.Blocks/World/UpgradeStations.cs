// SPDX-License-Identifier: MIT
// The rarity upgrade station (package P4 of the equipment wave, docs/kiln/modding/world-blocks.md "Upgrade stations"): a placed
// thing whose use raises an item's rarity by one step of a RarityUpgradeBlock (InteractBlock.Upgrade), or an offer that does
// the same on any entity of the world (UpgradeStation.Offer). Everything that decides is the server's:
//  - Held (every player): once the game accepted the use (InteractionResultEvent), the server reads the item in the player's
//    hand and runs Items.UpgradeRarity with the block's rule; the outcome goes to the player as a toast (a Kiln client) or a
//    server chat line (any other).
//  - Choose (Kiln clients with the channel kiln:upgrades): the server prices every piece of gear the player carries with a
//    dry run and sends the list; the player's game shows it in a Kiln window and sends back the one picked. The server takes a
//    pick only from the list it sent (a token per player, 2 minutes), from a player still within Range of that station, for an
//    item still in their inventory, and runs the same upgrade: rule, cost and chance are checked again there. A player whose
//    game has no such channel gets Held.
// Channel kiln:upgrades, required: false: a vanilla client never waits on it and is never sent anything on it.
using System.Collections.Concurrent;
using System.Security.Cryptography;

namespace Kiln;

/// <summary>Which item an upgrade interaction raises (<see cref="UpgradeAction.Target"/>).</summary>
public enum UpgradeTarget
{
    /// <summary>The item in the player's hand (a weapon, shield or tool on the action bar), read by the server: works for
    /// every player, also one without Kiln. The default.</summary>
    Held = 0,

    /// <summary>A window lists the gear the player carries with each next step's cost and chance, and the player picks one.
    /// Needs Kiln on the player's game (0.2.0-dev.29.19 or later); for any other player it acts as <see cref="Held"/>.</summary>
    Choose = 1,
}

/// <summary>
/// A rarity upgrade as what happens when a player uses something: on a placed thing (<see cref="InteractBlock.Upgrade"/>) or
/// as an offer on any entity (<see cref="UpgradeStation.Offer"/>). It runs one step of a rarity upgrade rule
/// (<c>RarityUpgradeBlock</c>) on the server: its items, steps, costs and chances, paid by the player.
/// <code>
/// Blocks.Define("rarity-anvil", new PropBlock
/// {
///     Name = "Rarity Anvil", Visual = new VisualBlock { Like = GameItems.Prop_Decoration_Comfort1_T0_Table_Medium },
///     OnInteract = new InteractBlock { Prompt = "Upgrade", Upgrade = new UpgradeAction { Rule = "standard-upgrades" } },
/// });
/// </code>
/// </summary>
public sealed record UpgradeAction
{
    /// <summary>How far, in metres, a player may stand from the station when the pick of a <see cref="UpgradeTarget.Choose"/>
    /// window arrives, unless <see cref="Range"/> says otherwise: 6.</summary>
    public const double DefaultRange = 6;

    /// <summary>The rarity upgrade rule it runs: a <c>RarityUpgradeBlock</c> of this mod by name, or <c>"other-mod:name"</c>.</summary>
    public required string Rule { get; init; }

    /// <summary>The item it raises (default <see cref="UpgradeTarget.Held"/>).</summary>
    public UpgradeTarget Target { get; init; } = UpgradeTarget.Held;

    /// <summary>1 to 64 metres: how far from the station a player may be when they pick an item in the
    /// <see cref="UpgradeTarget.Choose"/> window (default <see cref="DefaultRange"/>). Farther, the pick is refused.</summary>
    public double Range { get; init; } = DefaultRange;
}

/// <summary>
/// Rarity upgrades from code, the way a station's use runs them: <see cref="Use"/> as if a player used a station, and
/// <see cref="Offer"/> to put the use on any entity of the world (an "E" prompt), which players without Kiln can use too.
/// A placed thing of your own takes <see cref="InteractBlock.Upgrade"/> instead. Capability <c>blocks.upgrade_station</c>.
/// </summary>
public static class UpgradeStation
{
    /// <summary>The capability: upgrades as a station's use (needs <c>items.gear.upgrade</c>).</summary>
    public const string Capability = "blocks.upgrade_station";

    /// <summary>True when a station's use can upgrade (capability <c>blocks.upgrade_station</c>).</summary>
    public static bool Available
    {
        get
        {
            UpgradeStations.ReportRow();
            return Capabilities.Is(Capability);
        }
    }

    /// <summary>Runs <paramref name="action"/> for <paramref name="player"/> as if they had used <paramref name="station"/>: the
    /// held item is upgraded, or (<see cref="UpgradeTarget.Choose"/>, a Kiln player) the window opens. The player is told the
    /// outcome. Completes with the upgrade's result, or null when nothing was upgraded yet (no item in the hand, the window
    /// opened, the rule refused before pricing).</summary>
    /// <param name="player">The player who uses it.</param>
    /// <param name="station">What they use (a placed thing, any entity): where a window's pick is measured from.</param>
    /// <param name="action">The rule and the target.</param>
    /// <exception cref="ArgumentException"><paramref name="player"/> is none, or the action is malformed (no rule, a range
    /// outside 1..64, an unknown target).</exception>
    /// <exception cref="InvalidOperationException">Not on the tick thread.</exception>
    /// <exception cref="KilnUnavailableException">The capability <c>blocks.upgrade_station</c> is off.</exception>
    [ServerOnly]
    [TickThreadOnly]
    [RequiresCapability(Capability)]
    public static Task<UpgradeResult?> Use(EntityId player, EntityId station, UpgradeAction action)
    {
        ArgumentNullException.ThrowIfNull(action);
        Services.RequireTickThread("UpgradeStation.Use");
        if (player.IsNone) throw new ArgumentException("no player", nameof(player));
        UpgradeStations.Check(action, nameof(action));
        UpgradeStations.Require();
        var mod = ModContext.CurrentMod;
        return UpgradeStations.Use(player, station, UpgradeStations.StationOf(action, mod?.Info.Id, mod?.Info.Id ?? "code", "Upgrade", mod));
    }

    /// <summary>Puts the use on any entity of the world: an "E" prompt (the game's word Use, which every player's game
    /// shows) whose acceptance runs <paramref name="action"/> for the player, as <see cref="Use"/> does. Through
    /// <c>Offers</c> (Kiln.Gameplay): one offer per entity, never on players or on entities with the game's own offer;
    /// removed with <c>Offers.Remove</c> and when your mod unloads.</summary>
    /// <returns>True when the offer was added; false with the reason otherwise.</returns>
    /// <exception cref="ArgumentException">The entity is none, or the action is malformed.</exception>
    /// <exception cref="InvalidOperationException">Not on the tick thread.</exception>
    /// <exception cref="KilnUnavailableException">The capability <c>blocks.upgrade_station</c> is off.</exception>
    [ServerOnly]
    [TickThreadOnly]
    [RequiresCapability(Capability)]
    public static bool Offer(EntityId entity, UpgradeAction action, out string reason, OfferOptions? options = null)
    {
        ArgumentNullException.ThrowIfNull(action);
        Services.RequireTickThread("UpgradeStation.Offer");
        if (entity.IsNone) throw new ArgumentException("no entity", nameof(entity));
        UpgradeStations.Check(action, nameof(action));
        UpgradeStations.Require();
        var mod = ModContext.CurrentMod;
        var station = UpgradeStations.StationOf(action, mod?.Info.Id, mod?.Info.Id ?? "code", "Upgrade", mod);
        return Offers.TryAdd(entity, null, OfferVerbs.Use, a => _ = UpgradeStations.Use(a.Player, a.Entity, station), out reason, options);
    }
}

/// <summary>The server's half (uses, windows, picks), the channel and the client's window.</summary>
internal static class UpgradeStations
{
    /// <summary>What a station's use runs: the rule's id (<c>"mod:name"</c>), the target, the pick's range, the window's
    /// title, and the mod it runs as.</summary>
    internal sealed record Station(string Mod, string Block, string Rule, UpgradeTarget Target, double Range, string Title, ModContext? Context = null);

    internal const string ChannelName = "kiln:upgrades";
    internal const string ChannelVersion = "1";
    internal const int MaxRows = 32;                                          // gear a window lists
    internal static readonly TimeSpan MenuLife = TimeSpan.FromMinutes(2);     // a window's picks are taken this long

    // ---- checks and the rule's id ----

    /// <summary>What is wrong with an action: <see cref="ArgumentException"/> naming the field.</summary>
    internal static void Check(UpgradeAction a, string param)
    {
        if (string.IsNullOrWhiteSpace(a.Rule)) throw new ArgumentException("Rule: a rarity upgrade's name", param);
        if (!Enum.IsDefined(a.Target)) throw new ArgumentException($"Target: {a.Target} is no target (held, choose)", param);
        if (!double.IsFinite(a.Range) || a.Range is < 1 or > 64) throw new ArgumentException($"Range: {a.Range} is not 1 to 64 metres", param);
    }

    /// <summary>The rule's id: "mod:name" as it is, a bare name in <paramref name="mod"/>.</summary>
    internal static string RuleId(string rule, string? mod)
    {
        var r = rule.Trim();
        return r.Contains(':') || mod is null ? r : $"{mod}:{r}";
    }

    internal static Station StationOf(UpgradeAction a, string? mod, string block, string title, ModContext? context) =>
        new(mod ?? "", block, RuleId(a.Rule, mod), a.Target, a.Range, title, context);

    // ---- the capability row ----

    /// <summary>The row, from whether upgrades run here. Pure.</summary>
    internal static BlockCapabilities.Row RowOf(bool upgrades, string? why) => upgrades
        ? new BlockCapabilities.Row(CapabilityState.Available, CapabilityReason.None, null)
        : new BlockCapabilities.Row(CapabilityState.Unavailable, CapabilityReason.ServiceMissing,
            $"needs items.gear.upgrade{(string.IsNullOrEmpty(why) ? "" : $" ({why})")}");

    /// <summary>Reports the row from the upgrade row now (a service row's refresh, a station's use).</summary>
    internal static void ReportRow()
    {
        try
        {
            var up = Capabilities.Is("items.gear.upgrade");
            var row = RowOf(up, up ? null : Capabilities.Get("items.gear.upgrade").ReasonText);
            Capabilities.Report(UpgradeStation.Capability, row.State, row.Reason, row.Text);
        }
        catch (Exception e) when (e is InvalidOperationException or ArgumentException or NotSupportedException or DllNotFoundException or EntryPointNotFoundException)
        {
            // no capability table in this process (a tool, a test without a harness)
        }
    }

    internal static void Require()
    {
        ReportRow();
        if (!Capabilities.Is(UpgradeStation.Capability))
            throw new KilnUnavailableException(UpgradeStation.Capability, Capabilities.Get(UpgradeStation.Capability).ReasonText);
    }

    // ---- seams (tests) ----

    internal static Func<EntityId, ItemInstanceRef> HeldOf = static p => Items.Held(p);
    internal static Func<EntityId, IReadOnlyList<InventorySlot>> SlotsOf = static p => Items.Read(p)?.Slots ?? [];
    internal static Func<EntityId, bool> CanChoose = static p => UpgradeNet.CanChoose(p);
    internal static Action<EntityId, UpgradeNotice> SendNotice = static (p, n) => UpgradeNet.Notice(p, n);
    internal static Action<EntityId, UpgradeMenu> SendMenu = static (p, m) => UpgradeNet.Menu(p, m);
    internal static Func<EntityId, WorldPosition?> PositionOf = static e => World.Exists(e) ? World.Position(e) : null;
    internal static Func<long> Clock = static () => Environment.TickCount64;
    internal static Func<ItemId, string> NameOf = static id => Items.Get(id)?.Label ?? id.ToString();

    // ---- a use ----

    /// <summary>A player used a station (tick thread): the held item, or the window. Its awaits come back on the tick thread
    /// (<see cref="OnTick{T}"/>), whoever called it: an event handler of Kiln's own runs with no mod's scheduler.</summary>
    internal static Task<UpgradeResult?> Use(EntityId player, EntityId station, Station s) => OnTick(() => UseCore(player, station, s));

    private static async Task<UpgradeResult?> UseCore(EntityId player, EntityId station, Station s)
    {
        try
        {
            if (s.Target == UpgradeTarget.Choose && CanChoose(player))
            {
                await OpenMenu(player, station, s);
                return null;
            }
            ItemInstanceRef item;
            try { item = HeldOf(player); }
            catch (KilnUnavailableException e)
            {
                Tell(player, $"Upgrades can't read what you hold on this server: {e.Message}", false);
                return null;
            }
            if (item.IsNone)
            {
                Tell(player, "Hold the item you want to upgrade in your hand, then use it again.", false);
                return null;
            }
            return await Run(player, item, s);
        }
        catch (Exception e)
        {
            BlocksBridge.Log(LogLevel.Warn, $"blocks: block {s.Block}: the upgrade for {player} failed: {e.Message}");
            return null;
        }
    }

    /// <summary>One upgrade as the station's mod, told to the player.</summary>
    internal static async Task<UpgradeResult?> Run(EntityId player, ItemInstanceRef item, Station s, UpgradeOptions? options = null)
    {
        Task<UpgradeResult> task;
        var outer = ModContext.CurrentMod;
        if (s.Context is { Alive: true } owner) ModContext.CurrentMod = owner;
        try { task = Items.UpgradeRarity(player, item, s.Rule, options); }
        catch (KilnUnavailableException e)
        {
            Tell(player, $"Upgrades are off on this server: {e.Message}", false);
            return null;
        }
        catch (ArgumentException e)
        {
            BlocksBridge.Log(LogLevel.Warn, $"blocks: block {s.Block}: {e.Message}");
            Tell(player, "This station has no upgrade rule loaded.", false);
            return null;
        }
        finally { ModContext.CurrentMod = outer; }
        var r = await task;
        if (options is not { DryRun: true })
        {
            Tell(player, Describe(r, NameOf), r.Ok);
            BlocksBridge.Log(LogLevel.Info, $"blocks: block {s.Block}: {r}");
        }
        return r;
    }

    private static void Tell(EntityId player, string text, bool ok)
    {
        try { SendNotice(player, new UpgradeNotice(text, ok)); }
        catch (Exception e) when (e is KilnUnavailableException or InvalidOperationException) { BlocksBridge.Log(LogLevel.Info, $"blocks: upgrade: {player} not told \"{text}\": {e.Message}"); }
    }

    /// <summary>What a player is told about an upgrade, in one or two sentences. Pure but for the names.</summary>
    internal static string Describe(UpgradeResult r, Func<ItemId, string> name)
    {
        var item = name(r.Before?.Type ?? r.Item.Item);
        var paid = r.Paid.Count == 0 ? "" : string.Join(", ", r.Paid.Select(p => $"{p.Value} x {name(p.Key)}"));
        switch (r.Status)
        {
            case UpgradeStatus.Done:
                var unlocked = r.After != null && r.Before != null && r.After.Unlocked > r.Before.Unlocked;
                return $"{item} is now {r.To}: " + (unlocked ? "its new enhancement is unlocked." : "unlock its new enhancement at a workbench.") +
                       (paid.Length > 0 ? $" Paid {paid}." : "");
            case UpgradeStatus.Failed:
                var dropped = r.After != null && r.After.Rarity < r.From;
                return $"The upgrade of {item} to {r.To} failed" + (r.Chance is > 0 and < 1 ? $" ({r.Chance:0%} chance)" : "") + ": " +
                       (dropped ? $"it dropped to {r.After!.Rarity}" : $"it stays {r.From}") + (paid.Length > 0 ? $", and {paid} are spent." : ", and nothing was paid.");
            case UpgradeStatus.AtMax: return $"{item} is {r.From}: this station upgrades nothing past that.";
            case UpgradeStatus.NoSlot: return $"{item} has no enhancement slot for {r.To}: it can't be upgraded further.";
            case UpgradeStatus.NotGear: return $"{item} has no rarity to raise.";
            case UpgradeStatus.NotAllowed: return $"This station doesn't upgrade {item} to {r.To}.";
            case UpgradeStatus.CantPay: return $"Not enough to upgrade {item} to {r.To}: {r.Message}";
            case UpgradeStatus.Canceled: return $"The upgrade of {item} was stopped.";
            case UpgradeStatus.Stale: return "That item is gone.";
            default: return $"The upgrade of {item} did not happen: {r.Reason}";
        }
    }

    // ---- the window (Kiln clients) ----

    private sealed record Session(uint Token, EntityId Station, Station S, long Until, HashSet<(uint Entity, ulong Serial)> Offered);

    private static readonly object Gate = new();
    private static readonly Dictionary<uint, Session> Sessions = new();

    /// <summary>Prices every piece of gear <paramref name="player"/> carries with a dry run and sends the window.</summary>
    internal static async Task OpenMenu(EntityId player, EntityId station, Station s)
    {
        var refs = new List<ItemInstanceRef>();
        foreach (var slot in SlotsOf(player))
            if (slot is { IsGear: true, Ref.IsNone: false } && !refs.Contains(slot.Ref))
            {
                refs.Add(slot.Ref);
                if (refs.Count >= MaxRows) break;
            }
        var tasks = refs.Select(r => Run(player, r, s, new UpgradeOptions { DryRun = true })).ToArray();
        var priced = await Task.WhenAll(tasks);
        var rows = new List<UpgradeMenuRow>();
        var offered = new HashSet<(uint, ulong)>();
        for (var i = 0; i < refs.Count; i++)
        {
            if (priced[i] is not { } r || r.Status is UpgradeStatus.NotGear or UpgradeStatus.Stale) continue;
            var item = refs[i];
            var can = r.Status == UpgradeStatus.Done;
            if (can) offered.Add((item.Entity.Id.Value, item.Entity.Serial));
            rows.Add(new UpgradeMenuRow(item.Entity.Id.Value, item.Entity.Serial, item.Item.Value, NameOf(r.Before?.Type ?? item.Item),
                (byte)r.From, (byte)r.To, can, (float)r.Chance,
                string.Join(", ", r.Paid.Select(p => $"{p.Value} x {NameOf(p.Key)}")), can ? "" : Describe(r, NameOf)));
        }
        var token = (uint)RandomNumberGenerator.GetInt32(1, int.MaxValue);
        lock (Gate) Sessions[player.Value] = new Session(token, station, s, Clock() + (long)MenuLife.TotalMilliseconds, offered);
        SendMenu(player, new UpgradeMenu(token, s.Title, rows.ToArray()));
    }

    /// <summary>A pick from a window (tick thread): only one the server offered, from a player still near that station, for an
    /// item still in their inventory. The window is sent again afterwards with the new prices.</summary>
    internal static Task<UpgradeResult?> Pick(EntityId player, UpgradePick m) => OnTick(() => PickCore(player, m));

    private static async Task<UpgradeResult?> PickCore(EntityId player, UpgradePick m)
    {
        Session? ses;
        lock (Gate) ses = Sessions.GetValueOrDefault(player.Value);
        if (ses == null || ses.Token != m.Token || Clock() > ses.Until)
        {
            Tell(player, "That upgrade window has closed: use the station again.", false);
            return null;
        }
        if (!ses.Offered.Contains((m.Entity, m.Serial)))
        {
            BlocksBridge.Log(LogLevel.Warn, $"blocks: upgrade: {player} picked {m.Entity}#{m.Serial}, which the window did not offer: ignored");
            return null;
        }
        if (!Near(player, ses.Station, ses.S.Range))
        {
            lock (Gate) Sessions.Remove(player.Value);
            Tell(player, "You are too far from the station: go back and use it again.", false);
            return null;
        }
        var item = new ItemInstanceRef(new EntityRef(new EntityId(m.Entity), m.Serial), new ItemId(m.Item));
        if (!SlotsOf(player).Any(x => x.Ref == item))
        {
            Tell(player, "That item is no longer in your inventory.", false);
            return null;
        }
        var r = await Run(player, item, ses.S);
        if (r != null && CanChoose(player) && Near(player, ses.Station, ses.S.Range)) await OpenMenu(player, ses.Station, ses.S);
        return r;
    }

    /// <summary>The player stands within <paramref name="range"/> metres of the station (which still exists).</summary>
    internal static bool Near(EntityId player, EntityId station, double range)
    {
        if (PositionOf(station) is not { } s || PositionOf(player) is not { } p) return false;
        double dx = p.X - s.X, dy = p.Y - s.Y, dz = p.Z - s.Z;
        return dx * dx + dy * dy + dz * dz <= range * range;
    }

    /// <summary>Tests: forget every window.</summary>
    internal static void Reset()
    {
        lock (Gate) Sessions.Clear();
    }

    // ---- the tick: picks from the network (any thread), and the continuations of a use's awaits ----

    private static readonly ConcurrentQueue<(EntityId Player, UpgradePick Pick)> Inbox = new();
    private static readonly ConcurrentQueue<(SendOrPostCallback Work, object? State)> Later = new();
    private static IDisposable? _tick;

    /// <summary>Continuations posted here run in Kiln.Blocks' service tick, on the tick thread.</summary>
    private sealed class TickSync : SynchronizationContext
    {
        public override void Post(SendOrPostCallback d, object? state)
        {
            Later.Enqueue((d, state));
            EnsureTick();
        }

        public override void Send(SendOrPostCallback d, object? state) => throw new NotSupportedException("upgrade stations post their work to the tick");

        public override SynchronizationContext CreateCopy() => this;
    }

    private static readonly TickSync Sync = new();

    /// <summary>Starts an async use with the tick context current, so each of its awaits continues on the tick thread.</summary>
    private static Task<T> OnTick<T>(Func<Task<T>> start)
    {
        var outer = SynchronizationContext.Current;
        SynchronizationContext.SetSynchronizationContext(Sync);
        try { return start(); }
        finally { SynchronizationContext.SetSynchronizationContext(outer); }
    }

    private static void EnsureTick()
    {
        lock (Gate) _tick ??= ServiceTick.Add("kiln.blocks upgrade stations", static (in TickInfo _) => Pump());
    }

    internal static void Enqueue(EntityId player, UpgradePick m)
    {
        if (player.IsNone || Inbox.Count > 256) return;
        Inbox.Enqueue((player, m));
        EnsureTick();
    }

    private static void Pump()
    {
        while (Inbox.TryDequeue(out var x)) _ = Pick(x.Player, x.Pick);
        // what is posted while these run waits for the next tick
        for (var n = Later.Count; n > 0 && Later.TryDequeue(out var w); n--)
        {
            var outer = SynchronizationContext.Current;
            SynchronizationContext.SetSynchronizationContext(Sync);
            try { w.Work(w.State); }
            catch (Exception e) { BlocksBridge.Log(LogLevel.Warn, $"blocks: upgrade station: {e.Message}"); }
            finally { SynchronizationContext.SetSynchronizationContext(outer); }
        }
    }
}

/// <summary>Server to client: what the player is told.</summary>
internal sealed record UpgradeNotice(string Text, bool Ok)
{
    public const ushort Id = 1;
    public static void Encode(UpgradeNotice m, NetWriter w) => w.String(m.Text).Bool(m.Ok);
    public static UpgradeNotice Decode(ref NetReader r) => new(r.String(4096), r.Bool());
}

/// <summary>One piece of gear in the window: the item (entity, serial, type), its name, the step, whether it can be upgraded
/// now, the chance, the cost and why not.</summary>
internal sealed record UpgradeMenuRow(uint Entity, ulong Serial, uint Item, string Name, byte From, byte To, bool Can, float Chance, string Cost, string Note);

/// <summary>Server to client: the window.</summary>
internal sealed record UpgradeMenu(uint Token, string Title, UpgradeMenuRow[] Rows)
{
    public const ushort Id = 2;

    public static void Encode(UpgradeMenu m, NetWriter w)
    {
        w.UInt32(m.Token).String(m.Title);
        var n = Math.Min(m.Rows.Length, UpgradeStations.MaxRows);
        w.Byte((byte)n);
        for (var i = 0; i < n; i++)
        {
            var r = m.Rows[i];
            w.UInt32(r.Entity).UInt64(r.Serial).UInt32(r.Item).String(r.Name).Byte(r.From).Byte(r.To).Bool(r.Can).Single(r.Chance).String(r.Cost).String(r.Note);
        }
    }

    public static UpgradeMenu Decode(ref NetReader r)
    {
        var token = r.UInt32();
        var title = r.String(512);
        var n = Math.Min((int)r.Byte(), UpgradeStations.MaxRows);
        var rows = new UpgradeMenuRow[n];
        for (var i = 0; i < n; i++)
            rows[i] = new UpgradeMenuRow(r.UInt32(), r.UInt64(), r.UInt32(), r.String(512), r.Byte(), r.Byte(), r.Bool(), r.Single(), r.String(2048), r.String(2048));
        return new UpgradeMenu(token, title, rows);
    }
}

/// <summary>Client to server: the item picked in a window.</summary>
internal sealed record UpgradePick(uint Token, uint Entity, ulong Serial, uint Item)
{
    public const ushort Id = 3;
    public static void Encode(UpgradePick m, NetWriter w) => w.UInt32(m.Token).UInt32(m.Entity).UInt64(m.Serial).UInt32(m.Item);
    public static UpgradePick Decode(ref NetReader r) => new(r.UInt32(), r.UInt32(), r.UInt64(), r.UInt32());
}

/// <summary>The channel kiln:upgrades: registered at the first service tick (CraftStockStartup), so it is there before anyone
/// joins.</summary>
internal static class UpgradeNet
{
    private static readonly object Gate = new();
    private static NetChannel? _channel;
    private static bool _started;

    internal static void Start()
    {
        lock (Gate)
        {
            if (_started) return;
            _started = true;
        }
        if (!Network.Available) return;
        var outer = ModContext.CurrentMod;
        ModContext.CurrentMod = null;  // Kiln's own channel, not a mod's
        try
        {
            _channel = Network.Channel(UpgradeStations.ChannelName, UpgradeStations.ChannelVersion, required: false)
                .Register<UpgradeNotice>(UpgradeNotice.Id, static (m, w) => UpgradeNotice.Encode(m, w),
                    static (ref NetReader r) => UpgradeNotice.Decode(ref r), static (m, _) => { if (!Client.IsHosting) UpgradeWindow.Toast(m); },
                    NetDirection.ToClient)
                .Register<UpgradeMenu>(UpgradeMenu.Id, static (m, w) => UpgradeMenu.Encode(m, w),
                    static (ref NetReader r) => UpgradeMenu.Decode(ref r), static (m, _) => { if (!Client.IsHosting) UpgradeWindow.Show(m); },
                    NetDirection.ToClient)
                .Register<UpgradePick>(UpgradePick.Id, static (m, w) => UpgradePick.Encode(m, w),
                    static (ref NetReader r) => UpgradePick.Decode(ref r), static (m, ctx) => UpgradeStations.Enqueue(ctx.Player, m),
                    NetDirection.ToServer);
        }
        catch (Exception e)
        {
            BlocksBridge.Log(LogLevel.Warn, $"kiln.blocks: the channel {UpgradeStations.ChannelName} is unavailable: {e.Message}");
        }
        finally
        {
            ModContext.CurrentMod = outer;
        }
    }

    private static bool IsLocal(EntityId player) => Client.Available && Client.IsHosting && Client.HasLocalPlayer && Client.LocalPlayer == player;

    /// <summary>The player's game can show the window: the hosting player's own (with the overlay), or a peer that has the
    /// channel.</summary>
    internal static bool CanChoose(EntityId player)
    {
        if (IsLocal(player)) return Ui.Available;
        return _channel != null && Network.PeerOf(player) is { } peer && peer.HasChannel(UpgradeStations.ChannelName);
    }

    /// <summary>A toast for a Kiln player (the hosting one's own, or a peer with the channel), else a server chat line.</summary>
    internal static void Notice(EntityId player, UpgradeNotice n)
    {
        if (IsLocal(player))
        {
            if (Ui.Available)
            {
                UpgradeWindow.Toast(n);
                return;
            }
        }
        else if (_channel != null && Network.PeerOf(player) is { } peer && peer.HasChannel(UpgradeStations.ChannelName) && _channel.SendTo(peer, n)) return;
        if (Chat.Available) Chat.Tell(player, n.Text);
    }

    internal static void Menu(EntityId player, UpgradeMenu m)
    {
        if (IsLocal(player)) UpgradeWindow.Show(m);
        else if (_channel == null || Network.PeerOf(player) is not { } peer || !_channel.SendTo(peer, m))
            BlocksBridge.Log(LogLevel.Info, $"blocks: upgrade: the window for {player} did not go out");
    }

    /// <summary>The client's pick: to the server, or to this process's own server half when it hosts.</summary>
    internal static void SendPick(UpgradePick p)
    {
        if (Client.Available && Client.IsHosting && Client.HasLocalPlayer)
        {
            UpgradeStations.Enqueue(Client.LocalPlayer, p);
            return;
        }
        _channel?.SendToServer(p);
    }
}

/// <summary>The client's window: the server's list, one button per item it can upgrade now. Drawn by Kiln.Ui's overlay.</summary>
internal static class UpgradeWindow
{
    private static readonly object Gate = new();
    private static UiWindow? _window;
    private static UpgradeMenu? _menu;
    private static bool _waiting;

    internal static void Toast(UpgradeNotice n)
    {
        if (Ui.Available) Hud.Toast(n.Text, n.Ok ? ToastKind.Info : ToastKind.Warning, 6);
    }

    internal static void Show(UpgradeMenu m)
    {
        if (!Ui.Available) return;
        UiWindow? w;
        lock (Gate)
        {
            _menu = m;
            _waiting = false;
            w = _window;
        }
        if (w == null)
        {
            var outer = ModContext.CurrentMod;
            ModContext.CurrentMod = null;  // Kiln's own window
            try { w = Ui.Window("Rarity upgrade", Draw, new UiWindowOptions { Width = 460, AutoResize = true }); }
            finally { ModContext.CurrentMod = outer; }
            lock (Gate) _window = w;
        }
        w.Open = true;
    }

    private static string RarityName(byte r) => Enum.IsDefined(typeof(ItemRarity), (int)r) ? ((ItemRarity)r).ToString() : r.ToString();

    private static void Draw(UiDraw d)
    {
        UpgradeMenu? m;
        bool waiting;
        lock (Gate) (m, waiting) = (_menu, _waiting);
        if (m == null) return;
        if (m.Title.Length > 0) d.Separator(m.Title);
        if (m.Rows.Length == 0) d.TextWrapped("You carry no gear this station upgrades.");
        for (var i = 0; i < m.Rows.Length; i++)
        {
            var r = m.Rows[i];
            d.Text($"{r.Name}: {RarityName(r.From)} -> {RarityName(r.To)}");
            if (r.Can)
            {
                d.TextDisabled((r.Cost.Length > 0 ? r.Cost : "free") + (r.Chance < 1 ? $", {r.Chance:0%} chance" : ""));
                if (!waiting && d.Button($"Upgrade##kiln-upgrade-{i}"))
                {
                    lock (Gate) _waiting = true;
                    UpgradeNet.SendPick(new UpgradePick(m.Token, r.Entity, r.Serial, r.Item));
                }
            }
            else d.TextWrapped(r.Note);
            d.Separator();
        }
        if (waiting) d.TextDisabled("Upgrading...");
        if (d.Button("Close##kiln-upgrade-close"))
            lock (Gate)
                if (_window != null) _window.Open = false;
    }
}
