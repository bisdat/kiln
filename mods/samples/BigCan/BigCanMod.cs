// SPDX-License-Identifier: MIT
using System.Runtime.InteropServices;
using Kiln;

namespace BigCan;

/// <summary>What bigcan:water carries: a can's water in units (charges x 64, as the game counts it) and its maximum.</summary>
[StructLayout(LayoutKind.Sequential, Pack = 1)]
public struct Water
{
    public uint Units;
    public uint Max;
}

/// <summary>
/// Sidecar sample (docs/kiln/services/replication.md "Sidecars"): the game's Watering Can holds 2,000 charges. The server
/// keeps the real water, but the game sends players a container's water as two 16-bit numbers, which stop at 1,023
/// charges (65,535 units): above that every player's game would show a full can. So the server also sends Kiln clients
/// that have this mod the true value on Kiln's own channel (a replicated extension, bigcan:water@1), and their HUD shows
/// it (Ui.Values, where the client has it). The value only travels while a can holds more than 1,023 charges: below that
/// the game's own number is exact, and the value is cleared. A player without the mod (or without Kiln) never gets a
/// frame; their can shows full until it is down to 1,023 charges (a per-peer filter can show them a fraction instead:
/// Replication.Filter).
/// </summary>
public sealed class BigCanMod : Mod
{
    private const string Can = "Tool_T1_WateringCan", CanTemplate = "Pide_Fuel_Watering_Can";
    private const int Charges = 2000, Exact = 1023;
    private static readonly UiText Label = UiText.Of(("en", "Charges"), ("de", "Ladungen"));
    private ReplicatedExtension<Water>? _water;
    private ItemId _can;

    public override void OnLoad()
    {
        // both sides, in OnLoad: Kiln advertises it in the join handshake and opens its channel before anyone joins
        _water = Replication.Extension<Water>("bigcan:water", 1);
        // the server: the can holds 2,000 charges (cans made from now on), and its water travels while it holds more than 1,023
        if (Mods.Side == ModSide.Server || Capabilities.Is("repl.sidecar"))
        {
            try
            {
                _water.On<Ecs.Fuel>(Provide);
            }
            catch (KilnUnavailableException e)
            {
                Log.Warn($"bigcan: {e.Message}: Kiln clients see the game's 16-bit water");
            }
            Content.OnReady(_ => Content.Templates.Get(CanTemplate)?.Set("FuelSetup.maxFuelCharges", Charges));
        }
        // the client: the can's detail number and slot bar from the true value
        _can = Items.Lookup(Can)?.Id ?? default;
        Try(Ui.Values.DetailCapability, () => Ui.Values.ItemBar(_can, Detail));
        Try(Ui.Values.SlotCapability, () => Ui.Values.SlotBar(_can, Slot));
        _water.Changed += e => Log.Debug($"bigcan: can {e.Entity}: {(e.Value is { } v ? $"{v.Units / 64}/{v.Max / 64} charges" : "the game's own value")}");
    }

    // tick thread, once per step for each can a Kiln client with the mod was sent
    private static bool Provide(EntityId can, ref Water value)
    {
        if (Items.Fuel.Read(can) is not { } f || f.Units <= Exact * Items.Fuel.UnitsPerCharge) return false;  // exact on the wire: no value
        value = new Water { Units = (uint)f.Units, Max = (uint)f.Max };
        return true;
    }

    private UiProviderDecision Detail(in UiValueContext context, in UiItemView item, UiBarValue accumulated)
    {
        if (_water is null || !_water.TryGet(item.ServerEntity, out var w) || w.Max == 0) return UiProviderDecision.Pass;
        return UiProviderDecision.Replace(new UiBarEdit(Fraction: Math.Clamp((float)w.Units / w.Max, 0f, 1f), Current: w.Units / 64,
            Max: w.Max / 64, Label: new UiLabelEdit(UiLabelMode.Set, Label)));
    }

    private UiProviderDecision Slot(in UiValueContext context, in UiItemView item, UiBarValue accumulated) =>
        _water is not null && _water.TryGet(item.ServerEntity, out var w) && w.Max > 0
            ? UiProviderDecision.Replace(new UiBarEdit(Fraction: Math.Clamp((float)w.Units / w.Max, 0f, 1f)))
            : UiProviderDecision.Pass;

    private void Try(string capability, Func<UiValueRegistration> register)
    {
        try
        {
            var r = register();
            if (!r.Ok) Log.Info($"bigcan: {capability} refused: {r.Reason}");
        }
        catch (KilnUnavailableException e)
        {
            Log.Info($"bigcan: {e.Message} (the HUD keeps the game's number)");
        }
    }
}
