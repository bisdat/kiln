// SPDX-License-Identifier: MIT
using System.Reflection;
using Xunit;

namespace Kiln.Hooks.Tests;

/// <summary>Tests that change Kiln.Hooks' process-wide state (its native table, its registrations) run one at a time.</summary>
[CollectionDefinition("kiln.hooks state", DisableParallelization = true)]
public sealed class HooksStateCollection;

/// <summary>Mod contexts for tests, made the way the loader makes them (Kiln.Api's internal HostAccess; this test
/// project has no InternalsVisibleTo from Kiln.Api, so through reflection).</summary>
internal static class TestMods
{
    static readonly Type HostAccess = typeof(ModInfo).Assembly.GetType("Kiln.HostAccess", throwOnError: true)!;
    static readonly MethodInfo Create = HostAccess.GetMethod("CreateContext", BindingFlags.Public | BindingFlags.Static)!;
    static readonly PropertyInfo CurrentProperty = HostAccess.GetProperty("Current", BindingFlags.Public | BindingFlags.Static)!;
    static readonly MethodInfo RetireMethod = typeof(ModContext).GetMethod("Retire", BindingFlags.NonPublic | BindingFlags.Instance)!;

    /// <summary>A mod context as the loader builds it. Its folder has no kiln.mod.json: the flag comes from
    /// <see cref="ModInfo.Advanced"/> only.</summary>
    public static ModContext Make(string id, bool advanced, ulong owner = 0)
    {
        var dir = Path.Combine(AppContext.BaseDirectory, "no-such-mod-folder", id);
        var info = new ModInfo(id, id, "1.0.0", dir) { Advanced = advanced };
        return (ModContext)Create.Invoke(null, [info, owner, dir])!;
    }

    public static ModContext? Current
    {
        get => (ModContext?)CurrentProperty.GetValue(null);
        set => CurrentProperty.SetValue(null, value);
    }

    /// <summary>Runs <paramref name="f"/> as <paramref name="mod"/> on this thread (as the host does around mod code).</summary>
    public static T As<T>(ModContext? mod, Func<T> f)
    {
        var outer = Current;
        Current = mod;
        try { return f(); }
        finally { Current = outer; }
    }

    public static void As(ModContext? mod, Action f) => As<object?>(mod, () => { f(); return null; });

    public static void DrainPending(ModContext mod)
    {
        var field = typeof(ModContext).GetField("Pending", BindingFlags.NonPublic | BindingFlags.Instance)!;
        var queue = (Queue<Action>)field.GetValue(mod)!;
        var pending = queue.ToArray(); queue.Clear();
        As(mod, () => { foreach (var action in pending) action(); });
    }

    static readonly PropertyInfo AliveProperty = typeof(ModContext).GetProperty("Alive", BindingFlags.NonPublic | BindingFlags.Instance)!;

    public static bool IsAlive(ModContext mod) => (bool)AliveProperty.GetValue(mod)!;

    public static bool IsUnloading(ModContext mod) => (bool)typeof(ModContext)
        .GetProperty("Unloading", BindingFlags.NonPublic | BindingFlags.Instance)!.GetValue(mod)!;

    static readonly EventInfo RetiringEvent = typeof(ModContext).GetEvent("Retiring", BindingFlags.NonPublic | BindingFlags.Instance)!;

    /// <summary>Adds a cleanup to the context's Retiring event, as another service does.</summary>
    public static void OnRetiring(ModContext mod, Action cleanup) =>
        RetiringEvent.GetAddMethod(nonPublic: true)!.Invoke(mod, [cleanup]);

    /// <summary>The unload path: services drop the mod's state (its Retiring event).</summary>
    public static void Retire(ModContext mod) => RetireMethod.Invoke(mod, null);

    public static string? BeginUnload(ModContext mod) => (string?)typeof(ModContext)
        .GetMethod("BeginUnload", BindingFlags.NonPublic | BindingFlags.Instance)!.Invoke(mod, null);

    public static string? UnloadWaitsFor(ModContext mod) => (string?)typeof(ModContext)
        .GetMethod("UnloadWaitsFor", BindingFlags.NonPublic | BindingFlags.Instance)!.Invoke(mod, null);

    /// <summary>Asserts the gate's refusal for an ordinary mod: InvalidOperationException naming the entry point and
    /// telling the author to set "advanced": true.</summary>
    public static void AssertGated(string api, string modId, Action call)
    {
        var e = Assert.Throws<InvalidOperationException>(call);
        Assert.StartsWith($"{api}: mod '{modId}' is not advanced.", e.Message);
        Assert.Contains("add \"advanced\": true to its kiln.mod.json to allow it.", e.Message);
    }
}
