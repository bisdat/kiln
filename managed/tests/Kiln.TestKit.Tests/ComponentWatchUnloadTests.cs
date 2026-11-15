// SPDX-License-Identifier: MIT
// A mod that unloads or reloads with live component watches (Kiln.World's ComponentChanges, 07 S7): the watches are
// released by the mod's owner in one call (release_owner on the kit's kiln.component_changes, the service's rule), the
// pairs are dropped, nothing reaches the unloaded load context, the context is collected (KilnHarness.Unload and Reload
// check it), and the next load watches again. Without the release-by-owner call the watches stay: this test fails.
using TkWatch;
using Xunit;

namespace Kiln.TestKit.Tests;

public sealed class ComponentWatchUnloadTests
{
    private static byte[] Bytes(byte fill) => Enumerable.Repeat(fill, 40).ToArray();

    [Fact]
    public void An_unloading_mod_s_watches_are_released_by_its_owner_and_its_context_is_collected()
    {
        using var kiln = new KilnHarness();
        kiln.Step(2);
        var e = kiln.Entities.Create(new WorldPosition(1, 2, 3));
        kiln.Entities.SetBytes(e, "Health", Bytes(1));
        var mod = kiln.Load<TkWatchMod>(new ModManifest("tk-watch"));
        kiln.Step(3);
        Assert.Contains("watch: made True 1 ", kiln.Lines("tk-watch", "watch: made"));
        Assert.Equal(2, kiln.ComponentChanges.Watches);
        Assert.Equal(1, kiln.ComponentChanges.PresentPairs);
        kiln.Entities.SetBytes(e, "Health", Bytes(2));
        kiln.Step(2);
        Assert.Single(kiln.Lines("tk-watch", "watch: event"));

        kiln.Unload(mod);  // throws ModLeakException when the load context is still alive
        Assert.Equal(0, kiln.ComponentChanges.Watches);  // released by owner: nothing of the mod is watched any more
        Assert.Equal(0, kiln.ComponentChanges.PresentPairs);
        Assert.True(kiln.HasLog("tk-watch unloads: 2 watch(es) released by its owner"));
        var seen = new List<ComponentValueChangedEvent>();
        using (Events.On<ComponentValueChangedEvent>(x => seen.Add(x)))
        {
            kiln.Entities.SetBytes(e, "Health", Bytes(3));
            kiln.Step(3);
        }
        Assert.Empty(seen);  // nobody watches: no event at all, so none can reach the unloaded context
        Assert.Single(kiln.Lines("tk-watch", "watch: event"));

        // the next load watches again
        kiln.Reload(mod);
        kiln.Step(3);
        Assert.Equal(2, kiln.ComponentChanges.Watches);
        kiln.Entities.SetBytes(e, "Health", Bytes(4));
        kiln.Step(2);
        Assert.Equal(2, kiln.Lines("tk-watch", "watch: event").Count);
    }

    // Kiln.World's consumer state is static and the kit's backend is one object for every harness: a harness that
    // watched must not leave the next one bound to its own (ended) service, or the next one's events never come.
    [Fact]
    public void A_harness_after_one_that_watched_gets_its_events()
    {
        for (var run = 1; run <= 2; run++)
        {
            using var kiln = new KilnHarness();
            kiln.Step(2);
            var e = kiln.Entities.Create(new WorldPosition(1, 2, 3));
            kiln.Entities.SetBytes(e, "Health", Bytes(1));
            var mod = kiln.Load<TkWatchMod>(new ModManifest("tk-watch"));
            kiln.Step(3);
            kiln.Entities.SetBytes(e, "Health", Bytes(2));
            kiln.Step(2);
            Assert.True(kiln.Lines("tk-watch", "watch: event").Count == 1, $"harness {run}: {kiln.Lines("tk-watch", "watch: event").Count} event line(s)");
            kiln.Unload(mod);
        }
    }

    [Fact]
    public void A_reload_with_live_watches_leaves_only_the_new_instance_s()
    {
        using var kiln = new KilnHarness();
        kiln.Step(2);
        var e = kiln.Entities.Create(new WorldPosition(1, 2, 3));
        kiln.Entities.SetBytes(e, "Health", Bytes(1));
        var mod = kiln.Load<TkWatchMod>(new ModManifest("tk-watch"));
        kiln.Step(3);
        Assert.Equal(2, kiln.ComponentChanges.Watches);
        for (var i = 0; i < 3; i++)
        {
            kiln.Reload(mod);  // the old instance's context must be collected
            kiln.Step(3);
            Assert.Equal(2, kiln.ComponentChanges.Watches);  // the old watches were released, the new ones made
        }
        kiln.Unload(mod);
        Assert.Equal(0, kiln.ComponentChanges.Watches);
    }
}
