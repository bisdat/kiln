// SPDX-License-Identifier: MIT
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Text.Json.Nodes;
using Xunit;

namespace Kiln.Contract.Tests;

/// <summary>Roger de R'lyeh's report (Kiln 0.2.0-dev.29.18, #mod-dev 2026-11-20), first half: a mod with two StatusBlocks
/// (side server, clients kiln) was hot-loaded on a dedicated server with a player in. The server's content state carried
/// the two statuses' programs; the client served them ("2 program(s) from the server served here"), then asked
/// kiln.abilities to register them and logged "kiln.abilities did not register the programs (4)": program_refresh is a
/// tick function of the server world's thread and answers KILN_ERR_WRONG_THREAD anywhere else, a joined client's main
/// thread included (Kiln's managed tick thread there, not kiln.abilities'). The error failed the sync, and the server
/// refused the player for kiln.content.</summary>
public sealed unsafe class SyncProgramsJoinTests : FakeTableTest
{
    private static int _refreshCalls;

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int FakeIsNew(SyncPrograms.Key* k) => 1;

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int FakeData(SyncPrograms.Key* k, void** p, nuint* len)
    {
        *p = null;
        *len = 0;
        return 2;  // not served here: the client has no such program
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int FakePut(ulong owner, SyncPrograms.Key* k, void* data, nuint len, uint flags) => 0;

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static void FakeRevertAssets(ulong owner) { }

    // kiln.abilities' program_refresh is a "tick" function of the server world's thread: anywhere else, a joined client's
    // main thread included, it answers KILN_ERR_WRONG_THREAD (4), as the native service does
    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int FakeRefresh(uint* added)
    {
        _refreshCalls++;
        *added = 0;
        return 4;
    }

    [Fact]
    public void A_joined_client_takes_the_programs_of_a_servers_two_statusblocks_without_an_error()
    {
        var assets = Alloc<SyncPrograms.AssetsTable>();
        assets->Header = new ServiceHeader { Size = (uint)sizeof(SyncPrograms.AssetsTable), Version = 1 };
        assets->ResourceIsNew = &FakeIsNew;
        assets->ResourceData = &FakeData;
        assets->ResourcePut = &FakePut;
        assets->RevertOwner = &FakeRevertAssets;
        var abilities = Alloc<SyncPrograms.AbilitiesTable>();
        abilities->Header = new ServiceHeader { Size = (uint)sizeof(SyncPrograms.AbilitiesTable), Version = 1 };
        abilities->ProgramRefresh = &FakeRefresh;
        Serve("kiln.assets", assets, typeof(SyncPrograms), "_assets");
        Serve("kiln.abilities", abilities, typeof(SyncPrograms), "_abilities");
        Swap(typeof(SyncPrograms), "_looked", false);
        _refreshCalls = 0;

        var program = new byte[64];
        var server = new JsonObject
        {
            ["@programs"] = new JsonArray(
                new JsonObject { ["guid"] = Guid.NewGuid().ToString(), ["data"] = Convert.ToBase64String(program) },
                new JsonObject { ["guid"] = Guid.NewGuid().ToString(), ["data"] = Convert.ToBase64String(program) }),
        };
        try
        {
            var (served, local, errors) = SyncPrograms.Serve(server, 77);
            Assert.Equal(2, served);
            Assert.Equal(0, local);
            Assert.Equal(1, _refreshCalls);  // asked, and answered WRONG_THREAD as on a joined client
            Assert.Empty(errors);  // 0.2.0-dev.29 to .22: "kiln.abilities did not register the programs (4)", and the server refused the player
        }
        finally
        {
            SyncPrograms.Release(77);
        }
    }
}
