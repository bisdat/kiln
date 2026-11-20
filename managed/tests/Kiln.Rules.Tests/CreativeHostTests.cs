// SPDX-License-Identifier: MIT
using System.Reflection;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using Xunit;

namespace Kiln.Rules.Tests;

public unsafe class CreativeHostTests
{
    private static uint _role, _player;
    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int State(void* ptr)
    {
        var p = (uint*)ptr;
        p[1] = 1;
        p[2] = _role;
        p[4] = _player;
        p[5] = _player == 0 ? 0u : 1u;
        return 0;
    }

    [Theory]
    [InlineData(2u, true)] // offline and co-op use Hosting
    [InlineData(3u, false)]
    public void LateLocalPlayerRefreshesOnlyTheHostsMenu(uint role, bool admin)
    {
        var field = typeof(Client).GetField("_table", BindingFlags.NonPublic | BindingFlags.Static)!;
        var previous = field.GetValue(null);
        var table = (byte*)NativeMemory.AllocZeroed(256);
        *(delegate* unmanaged[Cdecl]<void*, int>*)(table + 8) = &State;
        field.SetValue(null, Pointer.Box(table, field.FieldType));
        var state = typeof(CreativeMenu).GetField("_state", BindingFlags.NonPublic | BindingFlags.Static)!;
        var drain = typeof(CreativeNet).GetMethod("Drain", BindingFlags.NonPublic | BindingFlags.Static)!;
        try
        {
            CreativeNet.WorldClosed();
            CreativeMenu.OnDisconnected();
            _role = role;
            _player = 0;
            drain.Invoke(null, null);
            Assert.Null(state.GetValue(null));
            _player = 1;
            drain.Invoke(null, null);
            var message = (CreativeStateMsg?)state.GetValue(null);
            Assert.Equal(admin, message.HasValue && (message.Value.Access & CreativeAccess.Admin) != 0);
            if (admin)
            {
                Assert.False((message!.Value.Access & CreativeAccess.On) != 0);
                CreativeNet.WorldClosed();
                CreativeMenu.OnDisconnected();
                drain.Invoke(null, null);
                Assert.NotNull(state.GetValue(null)); // same entity in a new world
            }
        }
        finally
        {
            field.SetValue(null, previous);
            NativeMemory.Free(table);
            CreativeNet.WorldClosed();
            CreativeMenu.OnDisconnected();
        }
    }
}
