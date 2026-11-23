// SPDX-License-Identifier: MIT
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;

namespace Kiln.TestKit;

internal static unsafe partial class ItemsAbi
{
    [StructLayout(LayoutKind.Sequential)]
    public struct GiveEquipRequest
    {
        public uint Size, Player, Item, Count, Flags;
        public byte EquipmentSlot;
        public fixed byte Reserved[3];
    }
}

internal static unsafe partial class ItemsTable
{
    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int GiveEquip(ulong owner, ItemsAbi.GiveEquipRequest* request, nint done, void* user, ulong* ticket)
    {
        try { return Fake?.GiveEquip(owner, request, done, (nint)user, ticket) ?? FakeTables.Unavailable; }
        catch { return FakeTables.Unavailable; }
    }
}

internal sealed unsafe partial class ItemsFake
{
    public const string GiveEquipFeature = "items.give_equip";
    private sealed class GiveEquipJob
    {
        public required ulong Ticket, Owner;
        public required ItemsAbi.GiveEquipRequest Request;
        public required Inv? Inventory;
        public required nint Done, User;
    }
    private readonly List<GiveEquipJob> _giveEquipPending = [];
    // The kit models logical equipment separately; it does not pretend to reproduce the game's page layout.
    private readonly Dictionary<(Inv Inventory, byte Slot), Stack> _equipment = [];
    internal static byte EquipmentType(byte slot) => slot switch
    {
        <= 4 => (byte)(13 + slot), <= 9 => (byte)(8 + slot), 10 or 11 => 37, 12 => 5, 13 => 18,
        14 => 6, 15 => 7, 16 => 8, 21 => 34, 22 => 35, 23 => 38, _ => 0,
    };
    private bool GiveEquipUsable => _harness.TaskPool.ServerSession != 0 && !FakeTables.Off(GiveEquipFeature) && !FakeTables.Off(TransactionFeature) &&
        Program is ItemsAbi.ProgramPending or ItemsAbi.ProgramRunning;

    public int GiveEquip(ulong owner, ItemsAbi.GiveEquipRequest* r, nint done, nint user, ulong* ticket)
    {
        if (ticket != null) *ticket = 0;
        if (r == null) return GiveEquipUsable ? FakeTables.Ok : FakeTables.Unavailable;
        if (r->Size < sizeof(ItemsAbi.GiveEquipRequest) || r->Player == 0 || r->Item == 0 || r->Count is 0 or > 65535 ||
            EquipmentType(r->EquipmentSlot) == 0 || (r->Flags & ~6u) != 0 || r->Reserved[0] != 0 || r->Reserved[1] != 0 || r->Reserved[2] != 0)
            return FakeTables.InvalidArgument;
        if (!GiveEquipUsable) return FakeTables.Unavailable;
        var job = new GiveEquipJob { Ticket = (ulong)Interlocked.Increment(ref _tickets), Owner = owner, Request = *r,
            Inventory = Of(r->Player), Done = done, User = user };
        lock (_lock)
        {
            if (_giveEquipPending.Count >= MaxPending) return FakeTables.BufferTooSmall;
            _giveEquipPending.Add(job);
        }
        if (ticket != null) *ticket = job.Ticket;
        return FakeTables.Ok;
    }

    private bool CancelGiveEquip(ulong ticket)
    {
        var job = _giveEquipPending.FirstOrDefault(q => q.Ticket == ticket && q.Done != 0);
        if (job == null) return false;
        job.Done = 0;
        return true;
    }

    private void RunGiveEquips(ulong step)
    {
        GiveEquipJob[] jobs;
        lock (_lock) { jobs = [.. _giveEquipPending]; _giveEquipPending.Clear(); }
        foreach (var job in jobs)
        {
            var q = job.Request;
            var result = new ItemsNative.EquipResult { Size = (uint)sizeof(ItemsNative.EquipResult), Ticket = job.Ticket,
                Player = q.Player, Flags = q.Flags, EquipmentSlot = q.EquipmentSlot, Steps = 1 };
            string why = "";
            var inventory = Of(q.Player);
            var item = Item(q.Item);
            _equipment.TryGetValue((inventory ?? job.Inventory!, q.EquipmentSlot), out var old);
            var take = (q.Flags & 4) != 0;
            var dry = (q.Flags & 2) != 0;
            var destination = inventory == null ? -1 : Array.FindIndex(inventory.Slots, s => s.Count == 0);
            if (!GiveEquipUsable) { result.Status = ItemsAbi.StatusUnavailable; why = "give-and-equip was switched off"; }
            else if (inventory == null || !inventory.IsPlayer || !ReferenceEquals(inventory, job.Inventory))
            { result.Status = ItemsAbi.NoInventory; why = "no player in this session"; }
            else if (_harness.CombatFake?.HealthOf(q.Player) is { Dead: true })
            { result.Status = ItemsAbi.Refused; why = "the player is dead"; }
            else if (q.EquipmentSlot == 23) { result.Status = ItemsAbi.Refused; why = "backpack items can't be given straight into the bag slot yet"; }
            else if (item == null) { result.Status = ItemsAbi.UnknownItem; why = "unknown item"; }
            else if (item.EquipmentSlotType != EquipmentType(q.EquipmentSlot)) { result.Status = ItemsAbi.Refused; why = "the item does not fit that equipment slot"; }
            else if (q.Count > item.MaxStack || (item.Gear && q.Count != 1)) { result.Status = ItemsAbi.Refused; why = "count must fit one equipment stack"; }
            else if (old != null && !take && destination < 0) { result.Status = ItemsAbi.NoRoom; why = "no free backpack slot for the replaced item"; }
            else
            {
                result.Status = ItemsAbi.Done;
                result.Item = q.Item;
                // Stable synthetic equipment cells; they are intentionally outside the fake backpack's links.
                result.To = (0x70000000u + q.Player * 4 + q.EquipmentSlot / 8u) | ((ulong)(q.EquipmentSlot % 8) << 32);
                if (old != null)
                {
                    result.Replaced = old.Item; result.ReplacedPide = old.Pide;
                    result.ReplacedStamp = StampOf(old.Pide); result.Reserved[0] = (uint)old.Count;
                    if (!take) result.ReplacedTo = inventory!.SlotId(destination);
                }
                if (!dry)
                {
                    if (old != null)
                    {
                        if (!take)
                        {
                            inventory!.Slots[destination] = old;
                            if (_gear.TryGetValue(old.Pide, out var oldGear)) oldGear.State &= unchecked((ushort)~8);
                        }
                        else if (old.Pide != 0) { _gear.Remove(old.Pide); World.Destroy(old.Pide); }
                    }
                    var stack = new Stack { Item = q.Item, Count = (int)q.Count, Level = (ushort)Math.Max(item!.LevelMin, 0), Rarity = (byte)item.Rarity };
                    if (item.Gear)
                    {
                        stack.Pide = World.Create(null);
                        NewGear(stack.Pide, item, stack.Level, stack.Rarity);
                        _gear[stack.Pide].State |= 8;
                        result.Pide = stack.Pide; result.Stamp = StampOf(stack.Pide);
                    }
                    _equipment[(inventory!, q.EquipmentSlot)] = stack;
                }
            }
            SpawnAbi.CopyText(why, result.Message, ItemsAbi.MessageMax);
            if (job.Done != 0) ((delegate* unmanaged[Cdecl]<ItemsNative.EquipResult*, void*, void>)job.Done)(&result, (void*)job.User);
        }
    }
}
