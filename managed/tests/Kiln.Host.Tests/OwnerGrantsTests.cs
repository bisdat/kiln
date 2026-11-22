// SPDX-License-Identifier: MIT
// A mod whose kiln.mod.json says "steamMessages": true: Kiln.Host grants the native owner it makes for it the Steam
// messages grant through the core's kiln.internal.owners table (kiln.net's steam_open admits that owner); a mod without
// it is never granted, and without the table the mod loads with a warning.
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using Kiln.Host;
using Xunit;

namespace Kiln.Host.Tests;

public sealed unsafe class OwnerGrantsTests : ModsFixture
{
    private static readonly List<(ulong Owner, uint Grants)> Granted = [];
    private static int _grantRc;

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int Grant(ulong owner, uint grants)
    {
        lock (Granted) Granted.Add((owner, grants));
        return _grantRc;
    }

    private static nint Table(uint version = 1)
    {
        // v2 (dev.31) appends owner_id at offset 24: 32 bytes, v1's members unchanged
        var size = version >= 2 ? 32u : (uint)sizeof(OwnerGrants.Table);
        var t = (OwnerGrants.Table*)NativeMemory.AllocZeroed(size);
        t->Size = size;
        t->Version = version;
        t->Grant = &Grant;
        return (nint)t;
    }

    private void InstallHello(bool steamMessages)
    {
        var dir = InstallHello();
        if (!steamMessages) return;
        var path = Path.Combine(dir, Manifest.FileName);
        var text = File.ReadAllText(path).TrimEnd();
        File.WriteAllText(path, text[..^1].TrimEnd() + ",\n  \"steamMessages\": true\n}\n");
    }

    [Fact]
    public void An_opted_in_mod_s_owner_is_granted_Steam_messages_and_its_ModInfo_says_so()
    {
        InstallHello(steamMessages: true);
        var host = new FakeHost();
        var table = Table();
        host.Services[OwnerGrants.Name] = table;
        lock (Granted) Granted.Clear();
        _grantRc = 0;
        try
        {
            using var mods = NewManager(host);
            mods.LoadAll();
            var hello = Assert.Single(mods.Mods);
            Assert.True(hello.Manifest.SteamMessages);
            Assert.True(hello.ModContext.Info.SteamMessages);
            lock (Granted) Assert.Equal(new List<(ulong, uint)> { (hello.Owner, OwnerGrants.SteamMessagesGrant) }, Granted);
            Assert.False(host.Has("steamMessages"), host.Dump());
        }
        finally
        {
            NativeMemory.Free((void*)table);
        }
    }

    [Fact]
    public void The_core_s_v2_table_still_grants_Steam_messages()
    {
        InstallHello(steamMessages: true);
        var host = new FakeHost();
        var table = Table(version: 2);
        host.Services[OwnerGrants.Name] = table;
        lock (Granted) Granted.Clear();
        _grantRc = 0;
        try
        {
            using var mods = NewManager(host);
            mods.LoadAll();
            var hello = Assert.Single(mods.Mods);
            lock (Granted) Assert.Equal(new List<(ulong, uint)> { (hello.Owner, OwnerGrants.SteamMessagesGrant) }, Granted);
            Assert.False(host.Has("different builds"), host.Dump());
        }
        finally
        {
            NativeMemory.Free((void*)table);
        }
    }

    [Fact]
    public void A_mod_without_the_opt_in_is_never_granted()
    {
        InstallHello(steamMessages: false);
        var host = new FakeHost();
        var table = Table();
        host.Services[OwnerGrants.Name] = table;
        lock (Granted) Granted.Clear();
        try
        {
            using var mods = NewManager(host);
            mods.LoadAll();
            Assert.False(Assert.Single(mods.Mods).ModContext.Info.SteamMessages);
            lock (Granted) Assert.Empty(Granted);
        }
        finally
        {
            NativeMemory.Free((void*)table);
        }
    }

    [Fact]
    public void Without_the_core_s_table_or_with_a_refused_grant_the_mod_loads_with_a_warning()
    {
        InstallHello(steamMessages: true);
        var host = new FakeHost();
        using (var mods = NewManager(host))
        {
            mods.LoadAll();
            Assert.Single(mods.Mods);
            Assert.True(host.Has("\"steamMessages\": true is not in force: this Kiln core does not admit mods"), host.Dump());
        }
        var refusing = new FakeHost();
        var table = Table();
        refusing.Services[OwnerGrants.Name] = table;
        _grantRc = 6;  // KILN_ERR_REVOKED
        try
        {
            using var mods = NewManager(refusing);
            mods.LoadAll();
            Assert.Single(mods.Mods);
            Assert.True(refusing.Has("the core refused the grant (6)"), refusing.Dump());
        }
        finally
        {
            _grantRc = 0;
            NativeMemory.Free((void*)table);
        }
    }
}
