// SPDX-License-Identifier: MIT
namespace Kiln.TestKit;

// The recording fakes of dev.27 (Fakes/: kiln.spawn, ...): their life on the harness's, called from KilnHarness.World.cs.
public sealed partial class KilnHarness
{
    /// <summary>After the world, the players and the saves: each fake's state for this harness.</summary>
    private void StartRecordingFakes()
    {
        StartSpawn();
        SpawnFake.Starting();
        StartChat();
        StartItems();
        StartCombat();
        StartTime();
        foreach (var fake in KitFakes.All) fake.Start(this);  // dev.28 on: Fakes/<Service>/ (IKitFake)
    }

    /// <summary>The native services' Pre, after the players' scan: what each fake's game does at the start of a step.</summary>
    private void RecordingFakesBeforeStep(ulong step)
    {
        SpawnFake?.BeforeStep();
        ChatFake?.BeforeStep(step);
        ItemsFake?.BeforeStep(step);
        CombatFake?.BeforeStep(step);
        TimeFake?.BeforeStep();
        foreach (var fake in KitFakes.All) fake.BeforeStep(this, step);
    }

    /// <summary>The budget of the process's first service tick (<see cref="StartServicesOnce"/>).</summary>
    private static readonly TimeSpan ServicesStartBudget = TimeSpan.FromSeconds(60);

    /// <summary>
    /// The first service tick of the process starts Kiln's services (ServiceTick.AutoStart: every service assembly beside
    /// the kit is loaded and its start type run). With the recording fakes, those starts find their tables and do their
    /// work; on a loaded machine that alone can take longer than a short <see cref="KilnHarnessOptions.CallTimeout"/>.
    /// The first harness of the process runs that tick here, on its tick thread at a menu step (as
    /// <c>BootServiceTick</c> does for a later harness), with a budget of its own, so a test's own call budget measures
    /// the test's calls. Later harnesses find the services started.
    /// </summary>
    private void StartServicesOnce()
    {
        if (ServiceTick.Step != 0) return;  // a service tick ran in this process already: the services are started
        var previous = _tick.Watchdog;
        _tick.Watchdog = new Watchdog { Budget = ServicesStartBudget > _options.CallTimeout ? ServicesStartBudget : _options.CallTimeout, Subject = "the services' start" };
        try
        {
            _tick.Invoke(() =>
            {
                ServiceTick.Run(HostAccess.Tick(MenuStepBase + ++_bootTicks, 0, 0, 0, 0));
                return 0;
            });
        }
        finally
        {
            _tick.Watchdog = previous;
        }
    }

    /// <summary>Fakes a test adds to this harness's end (Kiln.TestKit.Tests: an End that throws), ended after the kit's
    /// own in the same order, the later ones first. Not for mods: the kit finds its real fakes itself.</summary>
    internal List<IKitFake> ExtraFakesForTests { get; } = [];

    /// <summary>The harness ends, before the players and the world go. Every fake is ended, also when an earlier one's
    /// End throws: what was thrown is added to <paramref name="failures"/>, and the harness throws it once it is clean.</summary>
    private void EndRecordingFakes(List<Exception> failures)
    {
        IReadOnlyList<IKitFake> fakes = [.. KitFakes.All, .. ExtraFakesForTests];
        for (var i = fakes.Count - 1; i >= 0; i--)  // the later ones first
        {
            var fake = fakes[i];
            Ending(failures, () => fake.End(this));
        }
        Ending(failures, SpawnFake.Ended);
        Ending(failures, () => ItemsFake?.Ended());
    }

    /// <summary>One part of the fakes' end: an exception is kept, so the parts after it still run.</summary>
    private static void Ending(List<Exception> failures, Action end)
    {
        try
        {
            end();
        }
        catch (Exception e)
        {
            failures.Add(e);
        }
    }
}
