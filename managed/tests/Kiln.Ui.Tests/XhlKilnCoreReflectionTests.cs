using System.Reflection;
using System.Runtime.InteropServices;
using Xunit;

namespace Kiln.XhlKilnCoreReflectionTests;

// XHL's XHL-Core (Kiln) 0.1.0 (Nexus 377, Xhl.Kiln.Core.dll, built on 0.2.0-dev.29.17) reads these Kiln internals by
// reflection, by name: the language of Kiln's texts and its change event (its labels follow Kiln's language), kiln.ui's
// native table with its key subscription that also takes mouse and controller keys (its key capture), the mod's native
// owner id, and the press code the Keys page stores. A rename here silently breaks his mod's settings panel and key
// capture for every player who has it, so each name, binding and shape it asks for is pinned until Kiln gives him a public
// way (the report of dev29/xhl-compat). Only the lookups as his code makes them; what the members do is tested elsewhere.
public class XhlKilnCoreReflectionTests
{
    private const BindingFlags StaticNonPublic = BindingFlags.Static | BindingFlags.NonPublic;     // his 40
    private const BindingFlags InstanceNonPublic = BindingFlags.Instance | BindingFlags.NonPublic; // his 36
    private const BindingFlags StaticPublic = BindingFlags.Static | BindingFlags.Public;           // his 24

    [Fact]
    public void Kilns_text_language_its_gate_and_its_change_event_are_found_by_name()
    {
        var text = typeof(ModContext).Assembly.GetType("Kiln.KilnText");
        Assert.NotNull(text);
        Assert.Equal(typeof(string), text!.GetProperty("Language", StaticNonPublic)?.PropertyType);
        Assert.Equal(typeof(bool), text.GetProperty("Gated", StaticNonPublic)?.PropertyType);
        var changed = text.GetEvent("Changed", StaticNonPublic);
        Assert.NotNull(changed);
        Assert.Equal(typeof(Action), changed!.EventHandlerType);
        Assert.NotNull(changed.GetAddMethod(true));
        Assert.NotNull(changed.GetRemoveMethod(true));
        // and KeyBinding's public texts, which he reads the same way
        Assert.Equal(typeof(string), typeof(KeyBinding).GetProperty("Category")?.PropertyType);
        Assert.Equal(typeof(string), typeof(KeyBinding).GetProperty("Description")?.PropertyType);
    }

    [Fact]
    public void Kiln_uis_table_its_key_subscription_and_the_mods_owner_are_found_by_name()
    {
        var native = typeof(KeyBinding).Assembly.GetType("Kiln.UiNative");
        Assert.NotNull(native);
        var t = native!.GetProperty("T");
        Assert.NotNull(t);
        Assert.True(t!.PropertyType.IsPointer && t.GetMethod!.IsStatic, "UiNative.T: a static pointer property");
        var table7 = native.GetNestedType("Table7");
        var table = native.GetNestedType("Table");
        Assert.NotNull(table7);
        Assert.NotNull(table);
        // he reads the slot offsets with Marshal.OffsetOf and calls through them
        Assert.True(Marshal.OffsetOf(table7!, "SubscribeKeyEx").ToInt64() > 0);
        Assert.True(Marshal.OffsetOf(table!, "Unsubscribe").ToInt64() > 0);
        Assert.Equal(typeof(ulong), typeof(ModContext).GetProperty("Owner", InstanceNonPublic)?.PropertyType);
    }

    [Fact]
    public void The_keys_pages_press_code_is_found_by_name()
    {
        var names = typeof(KeyBinding).Assembly.GetType("Kiln.KeyNames");
        Assert.NotNull(names);
        var press = names!.GetMethod("PressCode", StaticPublic);
        Assert.NotNull(press);
        Assert.Equal(typeof(uint), press!.ReturnType);
        Assert.Equal([typeof(uint), typeof(uint)], press.GetParameters().Select(p => p.ParameterType));
    }
}
