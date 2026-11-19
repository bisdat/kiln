// SPDX-License-Identifier: MIT
// Issue #68: read-only investigation of ALL production-station Fill screens.
// Never modifies stock, enables a native button, cancels a game action, or calls Stations.Fill.
using System;
using System.Collections.Generic;
using System.Linq;
using System.Threading;
using Kiln;

namespace StationFillClientProbe;

public sealed class StationFillClientProbeMod : Mod
{
    private sealed record Snapshot(uint Station, string[] Lines)
    {
        public static readonly Snapshot Empty = new(0, ["Waiting for a hosted production-station screen..."]);
    }

    private readonly List<IDisposable> _subscriptions = [];
    private IDisposable? _panel;
    private Snapshot _snapshot = Snapshot.Empty;
    private uint _lastScreenStation;
    private string _lastPlanSignature = "";
    private string _lastAction = "none observed";
    private ulong _lastSampleStep;

    private void Note(string text) => Log.Info("fillprobe: " + text);

    public override void OnLoad()
    {
        Note("v0.1.0 loaded (read-only; ALL factory stations; never calls Fill or changes native UI)");
        Note($"hosting={Client.IsHosting}, client={Client.IsClient}, stations={Stations.Available}, ui={Ui.Available}");

        // Subscribe before a world starts. This records only actions the game's client actually sends.
        try
        {
            _subscriptions.Add(Events.On<StationPlayerActionEvent>(e =>
            {
                if (e.Action is not (StationPlayerAction.Fill or StationPlayerAction.Select or StationPlayerAction.UseAllOptions))
                    return;
                var line = $"ACTION-RECEIVED player={e.Player} station={e.Station} kind={e.Action}" +
                           $" recipe=0x{e.Recipe:x8} amount={e.Amount} options={e.Choices.Count} canCancel={e.CanCancel}";
                Volatile.Write(ref _lastAction, line);
                Note(line);
                // Observe only. In particular: NEVER call e.Cancel().
            }));
        }
        catch (Exception e)
        {
            Note($"action-event subscription unavailable: {e.Message}");
        }

        if (Ui.Available)
        {
            try
            {
                Ui.Screens.Changed += OnScreenChanged;
                _panel = Ui.Crafting.AddPanel("Station Fill probe (read-only)", Draw, new CraftingPanelOptions
                {
                    AtFactories = true,
                    Interactive = false,
                    Width = 400,
                    When = c => c.IsFactory,
                });
                Note("factory-screen observer and read-only panel enabled");
            }
            catch (Exception e)
            {
                Note($"UI observer unavailable: {e.Message}");
            }
        }

        // Host-side only: Stations.Plan is server-only and must not be called from the UI thread.
        Context.Tick.Pre += OnPre;

        try
        {
            _subscriptions.Add(Commands.Register("fillprobe", c =>
            {
                var snap = Volatile.Read(ref _snapshot);
                c.Reply($"fillprobe: station={snap.Station}; last native action: {Volatile.Read(ref _lastAction)}");
                foreach (var line in snap.Lines) c.Reply("fillprobe: " + line);
            }, CommandPermission.Admin, usage: "status", description: "Read-only factory Fill investigation (#68)"));
        }
        catch (Exception e)
        {
            Note($"console command unavailable: {e.Message}");
        }
    }

    private void OnScreenChanged(GameScreen before, GameScreen now)
    {
        var target = now.Crafting is { IsFactory: true } factory ? factory.Station.Value : 0;
        if (target == _lastScreenStation) return;
        if (target != 0)
            Note($"SCREEN-OPEN station={target} hud={now.Hud} workshop=0x{now.Crafting!.Value.Workshop:x8}");
        else if (_lastScreenStation != 0)
            Note($"SCREEN-CLOSE station={_lastScreenStation}");
        _lastScreenStation = target;
    }

    private void OnPre(in TickInfo tick)
    {
        if (!Client.IsHosting || !Client.HasLocalPlayer || !Stations.Available) return;
        if (tick.Step - _lastSampleStep < 30) return;
        _lastSampleStep = tick.Step;

        var screen = Ui.Screens.Current;
        if (screen.Crafting is not { IsFactory: true } factory || factory.Station.IsNone)
        {
            if (Volatile.Read(ref _snapshot).Station != 0)
            {
                Volatile.Write(ref _snapshot, Snapshot.Empty);
                _lastPlanSignature = "";
            }
            return;
        }

        try
        {
            var station = factory.Station;
            var info = Stations.Read(station);
            if (info is null)
            {
                Publish(station.Value, ["The selected entity is not a readable production station."], "");
                return;
            }

            // This is a +1 craft illustration. The native slider's amount is NOT exposed by CraftingScreen.
            // All inputs are read on the server tick; the UI panel only reads immutable strings.
            var options = new FillOptions
            {
                Crafts = 1,
                Mode = FillMode.Add,
                Exact = true,
                Recipe = info.SelectedRecipe == 0 ? null : info.SelectedRecipe,
                Choices = info.SelectedChoices,
                Player = Client.LocalPlayer,
                UseStationOutputs = true,
            };
            var plain = Stations.Plan(station, options with { UseStationOutputs = false });
            var shared = Stations.Plan(station, options);

            var lines = new List<string>
            {
                $"{info.DisplayName ?? info.Template ?? "Production station"} (entity {station.Value})",
                $"Template: {info.Template ?? "unknown"}; recipe 0x{info.SelectedRecipe:x8}",
                $"State={info.State}; loaded={info.CraftsLoaded}; sharing={Capabilities.Is(Stations.OutputSourcesCapability)}",
                $"+1 craft, bag + chests: {plain.Status} (affordable {plain.CraftsAffordable}, planned {plain.CraftsPlanned})",
                $"+1 craft, plus outputs: {shared.Status} (affordable {shared.CraftsAffordable}, planned {shared.CraftsPlanned})",
            };

            foreach (var i in shared.Inputs)
                lines.Add($"0x{i.Item.Value:x8}: need {i.PerCraft}; in station {i.Have}; bag/chests {i.Stock}; outputs {i.Outputs}");

            if (shared.ShortItem is { } shortItem)
                lines.Add($"Short: 0x{shortItem.Value:x8}, cause {shared.LimitedBy}");
            lines.Add("Native Fill enabled state and actual slider value: NOT READ by this probe.");
            lines.Add("Native Fill events are logged if the client sends them.");

            Publish(station.Value, [.. lines], $"{info.SelectedRecipe:x8}|{string.Join("|", lines.Take(lines.Count - 2))}");
        }
        catch (Exception e)
        {
            Publish(factory.Station.Value,
                [$"Plan unavailable: {e.GetType().Name}: {e.Message}", "Probe did not change any inventories."],
                $"error|{e.GetType().Name}|{e.Message}");
        }
    }

    private void Publish(uint station, string[] lines, string signature)
    {
        Volatile.Write(ref _snapshot, new Snapshot(station, lines));
        var key = station + "|" + signature;
        if (key == _lastPlanSignature) return;
        _lastPlanSignature = key;
        foreach (var line in lines) Note($"station={station} {line}");
    }

    private void Draw(UiDraw ui, CraftingScreen screen)
    {
        if (!screen.IsFactory) return;
        var snap = Volatile.Read(ref _snapshot);
        if (snap.Station == 0 || snap.Station != screen.Station.Value)
        {
            ui.Text($"Factory screen entity {screen.Station.Value}; reading server plan...");
            ui.TextDisabled("For hosted games; never modifies the native Fill UI.");
            return;
        }

        // The panel is entirely observational and does not call any server-side APIs.
        foreach (var line in snap.Lines) ui.Text(line);
    }

    public override void OnUnload()
    {
        Context.Tick.Pre -= OnPre;
        if (Ui.Available) Ui.Screens.Changed -= OnScreenChanged;
        _panel?.Dispose();
        foreach (var sub in _subscriptions) sub.Dispose();
        _subscriptions.Clear();
    }
}
