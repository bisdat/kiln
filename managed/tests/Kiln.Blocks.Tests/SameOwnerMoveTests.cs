// SPDX-License-Identifier: MIT
// GitHub issue #138 (5), 0.2.0-dev.31: a move inside one inventory owner (an equipment slot into the same player's backpack,
// one chest slot to another) stays refused, before anything reaches kiln.items, with no change: Items.Move,
// QuickStack and MoveAll with from == to complete Invalid and kiln.items' move is never called (kiln.items refuses
// from == to too: KILN_ERR_INVALID_ARGUMENT, kiln_items.h). Why it stays: docs/kiln/services/items.md "Moves inside one
// owner". Taking off what a player wears is Items.Unequip (the game's own unequip, into the backpack).
using System.Reflection;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using Xunit;

namespace Kiln.BlockTests;

public sealed unsafe class SameOwnerMoveTests
{
    private sealed class MoveBackend : IKilnBackend
    {
        public string Version => "test";
        public bool IsTickThread => true;
        public bool HasFeature(string name) => false;
        public nint Service(string name, uint minVersion) => 0;
        public void Log(ulong owner, LogLevel level, string text) { }
    }

    private static int _calls;

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int Move(ulong owner, ItemsNative.MoveRequest* request,
        delegate* unmanaged[Cdecl]<ItemsNative.MoveResult*, void*, void> callback, void* user, ulong* ticket)
    {
        _calls++;
        return ItemsNative.Ok;
    }

    public static TheoryData<string> Cases => new() { "equipment-to-backpack", "slot-to-slot", "same-slot", "dry-run", "quick-stack", "move-all" };

    [Theory]
    [MemberData(nameof(Cases))]
    public void A_move_inside_one_owner_is_refused_and_reaches_nothing(string which)
    {
        var flags = BindingFlags.Static | BindingFlags.NonPublic;
        var fields = new[] { "_table", "_v3" }.Select(name => typeof(Items).GetField(name, flags)!).ToArray();
        var previous = fields.Select(field => field.GetValue(null)).ToArray();
        var backend = Backend.Current;
        var table = (ItemsNative.TableV3*)NativeMemory.AllocZeroed((nuint)sizeof(ItemsNative.TableV3));
        table->V2.V1.Header = new ServiceHeader { Size = (uint)sizeof(ItemsNative.TableV3), Version = 3 };
        table->Move = &Move;
        try
        {
            Backend.Current = new MoveBackend();
            fields[0].SetValue(null, Pointer.Box(table, typeof(ItemsNative.Table*)));
            fields[1].SetValue(null, null);
            _calls = 0;
            var player = new EntityId(3);
            var helmet = new SlotId(0x77);                       // a worn equipment slot of the player
            var backpack = new SlotId(0x78 | (2ul << 32));      // a backpack slot of the same player
            var task = which switch
            {
                "equipment-to-backpack" => Items.Move(player, helmet, player, 1, MoveOptions.Equipped, toSlot: backpack),
                "slot-to-slot" => Items.Move(player, backpack, player, 5, toSlot: new SlotId(0x78 | (3ul << 32))),
                "same-slot" => Items.Move(player, backpack, player, 0, toSlot: backpack),
                "dry-run" => Items.Move(player, helmet, player, 1, MoveOptions.Equipped | MoveOptions.DryRun, toSlot: backpack),
                "quick-stack" => Items.QuickStack(player, player),
                _ => Items.MoveAll(player, player),
            };
            Assert.True(task.IsCompleted);
            var r = task.Result;
            Assert.Equal(ItemsStatus.Invalid, r.Status);
            Assert.Contains("same entity", r.Message);
            Assert.Empty(r.Changes);
            Assert.Equal(0, _calls);
        }
        finally
        {
            Backend.Current = backend;
            for (var i = 0; i < fields.Length; i++) fields[i].SetValue(null, previous[i]);
            NativeMemory.Free(table);
        }
    }
}
