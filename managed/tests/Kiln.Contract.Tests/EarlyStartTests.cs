// SPDX-License-Identifier: MIT
// A mod's OnLoad runs before the first service tick (the host loads the mods first in that step), so a row a mod reads
// there must be decided before the mods load: the host starts ServiceTick.EarlyStart right before LoadAll (0.2.0-dev.28
// fix round, the 13b review's minor 3: world.transform_scale read "unknown" in OnLoad). Proven on fresh copies of the
// assemblies, as ServiceStartTests does, with nothing but that early start.
using System.Reflection;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using Xunit;

namespace Kiln.Contract.Tests;

public sealed unsafe class EarlyStartTests
{
    private const BindingFlags All = BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.Static | BindingFlags.Instance;
    private static uint _types;

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static uint TypeCount() => _types;

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static void* TypeByName(NativeStr name) => null;  // a build without CurrentTransform: the row is decided off

    private static void StartEarly(IsolatedKiln kiln) =>
        kiln.Type("Kiln.ServiceTick").GetMethod("StartEarlyServices", All)!.Invoke(null, null);

    [Fact]
    public void Kiln_World_is_started_before_the_mods_load()
    {
        Assert.Contains(("Kiln.World", "Kiln.WorldStartup"), ServiceTick.EarlyStart);
        Assert.All(ServiceTick.EarlyStart, e => Assert.Contains(e, IsolatedKiln.AutoStart()));  // the first tick starts them too
        var host = File.ReadAllText(Path.Combine(Repo(), "managed", "Kiln.Host", "Entry.cs"));
        var early = host.IndexOf("ServiceTick.StartEarlyServices();", StringComparison.Ordinal);
        Assert.True(early > 0 && early < host.IndexOf("_mods.LoadAll();", early, StringComparison.Ordinal), "the host starts them right before LoadAll");
    }

    [Theory]
    [InlineData("world.transform_scale", "needs kiln.types")]
    [InlineData("world.tracked_writes", "kiln.world is not loaded")]
    public void Kiln_World_rows_are_decided_by_the_early_start_with_no_tick(string row, string because)
    {
        var kiln = new IsolatedKiln(["Kiln.World"]);
        Assert.Equal("Unknown", kiln.Row(row).Reason);  // what a mod's OnLoad read before the fix
        StartEarly(kiln);
        var (state, reason, text) = kiln.Row(row);
        Assert.Equal("Unavailable", state);
        Assert.NotEqual("Unknown", reason);
        Assert.StartsWith(because, text);
    }

    [Fact]
    public void An_empty_type_table_makes_the_row_pending_until_the_game_fills_it()
    {
        var t = (TypesNative.Table*)NativeMemory.AllocZeroed((nuint)sizeof(TypesNative.Table));
        t->Header = new ServiceHeader { Size = (uint)sizeof(TypesNative.Table), Version = TypesNative.Version };
        t->TypeCount = &TypeCount;
        t->TypeByName = &TypeByName;
        _types = 0;
        var kiln = new IsolatedKiln(["Kiln.World"]);
        kiln.Serve(TypesNative.Name, (nint)t);
        StartEarly(kiln);
        var pending = kiln.Row("world.transform_scale");
        Assert.Equal(("Pending", "Session"), (pending.State, pending.Reason));
        Assert.StartsWith("waits for the game's type table", pending.Text);
        _types = 11184;  // the game filled it
        for (var i = 0; i < 26; i++) kiln.Step();
        var decided = kiln.Row("world.transform_scale");
        Assert.Equal(("Unavailable", "SymbolMissing"), (decided.State, decided.Reason));
        Assert.StartsWith("this build has no keen::ecs::CurrentTransform", decided.Text);
        Assert.DoesNotContain(kiln.TickHandlers(), n => n.Contains("waiting for the type table"));  // stops asking once decided
    }

    private static string Repo([CallerFilePath] string self = "") =>
        Path.GetFullPath(Path.Combine(Path.GetDirectoryName(self)!, "..", "..", ".."));
}
