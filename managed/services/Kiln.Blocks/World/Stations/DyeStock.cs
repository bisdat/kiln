// SPDX-License-Identifier: MIT
// Item/count snapshots only: no source slots leave the server. A supported server's Tint hooks interpret the
// client's reserved dye key and resolve/access-check/pay from the shared source registry at use time.
using System.Collections.Concurrent;
namespace Kiln;

internal sealed record DyeStockRows(bool Supported, CraftStockRow[] Rows)
{
    internal const ushort Id = 5;
    internal static void Encode(DyeStockRows m, NetWriter w)
    {
        w.Bool(m.Supported).UInt16((ushort)m.Rows.Length);
        foreach (var r in m.Rows) w.UInt32(r.Item).UInt32(r.Count);
    }
    internal static DyeStockRows Decode(ref NetReader r)
    {
        var supported = r.Bool(); var n = r.UInt16();
        if (n > DyeStock.MaxRows) throw new InvalidDataException("too many dye source rows");
        var rows = new CraftStockRow[n]; var seen = new HashSet<uint>();
        for (var i = 0; i < n; i++)
        {
            var item = r.UInt32(); var count = r.UInt32();
            if (item == 0 || count == 0 || !seen.Add(item)) throw new InvalidDataException("invalid dye source row");
            rows[i] = new(item, count);
        }
        return new(supported, supported ? rows : []);
    }
}
internal sealed record DyeStockHello(bool Open)
{
    internal const ushort Id = 6;
    internal static void Encode(DyeStockHello m, NetWriter w) => w.Bool(m.Open);
    internal static DyeStockHello Decode(ref NetReader r) => new(r.Bool());
}
internal static class DyeStock
{
    internal const string SourceCapability = "crafting.dye_sources", UiCapability = "crafting.dye_ui";
    internal const int MaxRows = 256;
    private static readonly DyeStockRows Off = new(false, []);
    private static readonly ConcurrentQueue<(EntityId Player, bool Open)> Inbox = new();
    private sealed record Session(uint Generation, long Seen);
    private static readonly Dictionary<EntityId, Session> Sessions = [];
    private static readonly object Gate = new();
    private static NetChannel? _channel;
    private static IDisposable? _tick;
    private static ulong _lastStep;
    private static bool _open, _attached;
    private static long _lastHello, _lastRows;

    internal static void Attach(NetChannel channel)
    {
        if (_attached) return;
        _attached = true; _channel = channel;
        channel.Register<DyeStockRows>(DyeStockRows.Id, DyeStockRows.Encode,
            static (ref NetReader r) => DyeStockRows.Decode(ref r), static (m, _) => ClientRows(m), NetDirection.ToClient);
        channel.Register<DyeStockHello>(DyeStockHello.Id, DyeStockHello.Encode,
            static (ref NetReader r) => DyeStockHello.Decode(ref r), static (m, ctx) => Enqueue(ctx.Player, m.Open), NetDirection.ToServer);
        try
        {
            if (Client.IsClient) Ui.Screens.UiTick += ClientTick;
        }
        catch (Exception e) when (e is KilnUnavailableException or InvalidOperationException or TypeInitializationException)
        {
            ModLog.Warn($"crafting: the dye source screen is unavailable: {e.Message}");
        }
    }
    private static void Enqueue(EntityId player, bool open)
    {
        if (player.IsNone || Inbox.Count >= 1024) return;
        Inbox.Enqueue((player, open));
        lock (Gate) _tick ??= ServiceTick.Add("crafting dye sources", static (in TickInfo t) => Pump(t.Step));
    }
    private static void Pump(ulong step)
    {
        lock (Gate)
        {
            if (step < _lastStep) Sessions.Clear();
            var now = Environment.TickCount64;
            for (var n = 0; n < 256 && Inbox.TryDequeue(out var message); n++)
            {
                var player = Players.Find(message.Player);
                if (!message.Open || player is not { InGame: true }) Sessions.Remove(message.Player);
                else Sessions[message.Player] = new(player.Handle, now);
            }
            if (step >= _lastStep && step - _lastStep < 30) return;
            _lastStep = step;
            foreach (var pair in Sessions.ToArray())
            {
                var player = Players.Find(pair.Key);
                if (now - pair.Value.Seen > 15000 || player is not { InGame: true } || player.Handle != pair.Value.Generation)
                {
                    Sessions.Remove(pair.Key); continue;
                }
                var message = Build(pair.Key, Capabilities.Is(SourceCapability));
                if (Client.IsHosting && Client.HasLocalPlayer && Client.LocalPlayer == pair.Key) ClientRows(message);
                else _channel?.SendTo(pair.Key, message);
            }
        }
    }
    internal static DyeStockRows Build(EntityId player, bool supported)
    {
        if (!supported) return Off;
        var (rc, rows) = CraftingCore.AllStock(player);
        if (rc == StationsNative.BufferTooSmall) (rc, rows) = DyeStockNative.AllStock(player);
        if (rc != StationsNative.Ok) return Off;
        var dyes = rows.Where(r => Items.Get(new ItemId(r.Item))?.Category == ItemCategory.ColorPalette)
            .Select(r => new CraftStockRow(r.Item, r.Count)).ToArray();
        return dyes.Length <= MaxRows ? new(true, dyes) : Off;
    }
    // An older or vanilla server sends no supported announcement. Opening the menu alone never enables source rows.
    internal static DyeStockRows Visible(DyeStockRows rows, bool kilnServer, bool open) =>
        kilnServer && open && rows.Supported ? rows : Off;
    private static void ClientRows(DyeStockRows rows)
    {
        _lastRows = Environment.TickCount64;
        Apply(Visible(rows, Client.IsHosting || Network.ServerHasKiln, _open));
    }
    private static void ClientTick(GameScreen now)
    {
        var kiln = Client.IsHosting || Network.ServerHasKiln;
        var open = kiln && now.Hud == HudScreen.ItemTinting && Capabilities.Is(UiCapability);
        var time = Environment.TickCount64;
        if (open != _open || (open && time - _lastHello >= 5000))
        {
            _open = open; _lastHello = time;
            if (!open) Apply(Off);
            if (Client.IsHosting && Client.HasLocalPlayer) Enqueue(Client.LocalPlayer, open);
            else if (kiln) _channel?.SendToServer(new DyeStockHello(open));
        }
        if (_open && time - _lastRows > 15000) Apply(Off);
    }
    private static void Apply(DyeStockRows message)
    {
        if (!Client.HasLocalPlayer || !message.Supported) { DyeStockNative.Set(EntityId.None, []); return; }
        // A server may know a content item that this client lacks: never offer an unknown/non-palette row.
        var rows = message.Rows.Where(r => Items.Get(new ItemId(r.Item))?.Category == ItemCategory.ColorPalette)
            .Select(r => new StationsNative.Stack { Item = r.Item, Count = r.Count }).ToArray();
        DyeStockNative.Set(Client.LocalPlayer, rows);
    }
}
internal static unsafe class DyeStockNative
{
    private static StationsNative.Table* Table => (StationsNative.Table*)Services.Get<ServiceHeader>(StationsNative.Name, 1);
    internal static int Set(EntityId player, StationsNative.Stack[] rows)
    {
        var t = Table;
        if (t == null || t->Header.Size < 288 || t->SetDyeStock == null)
        {
            ServiceCaps.Static(DyeStock.UiCapability, false, t == null ? CapabilityReason.ServiceMissing : CapabilityReason.ServiceOlder,
                "kiln.stations has no dye-source snapshot member", StationsNative.Name);
            return StationsNative.Unavailable;
        }
        fixed (StationsNative.Stack* p = rows) return t->SetDyeStock(player.Value, p, (uint)rows.Length);
    }
    // Retry the common bridge's 1024-row buffer before filtering palettes; unrelated stored materials must not hide
    // dyes. The native source registry, generation binding, filters and access checks remain the only authority.
    internal static (int Rc, StationsNative.Stack[] Rows) AllStock(EntityId player)
    {
        var t = Table; uint n = 0;
        if (t == null || t->Header.Size < 280 || t->SourceStock == null) return (StationsNative.Unavailable, []);
        var rc = t->SourceStock(player.Value, 1, null, 0, &n);
        if (rc is not (StationsNative.Ok or StationsNative.BufferTooSmall) || n > 65536) return (StationsNative.Unavailable, []);
        var rows = new StationsNative.Stack[n];
        fixed (StationsNative.Stack* p = rows) rc = t->SourceStock(player.Value, 1, p, n, &n);
        return rc == StationsNative.Ok && n <= rows.Length ? (rc, rows[..(int)n]) : (StationsNative.Unavailable, []);
    }
}
