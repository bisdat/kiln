// SPDX-License-Identifier: MIT
// Ui.Tooltip: lines and stat numbers inside the game's own item detail view (equipment wave, P8b; capabilities
// ui.tooltip.lines and ui.tooltip.values; kiln_ui_values.h tooltip_lines_native and tooltip_values; the wave's
// TOOLTIPS.md). The lines are drawn as the last children of the detail's scrolled content (or between its stats and its
// description), wrapped at the panel's width as the game wraps its description, in the game's own rarity colours. They
// are also in Kiln's "Item details" window, which is where they stay wherever the game's view cannot take them (another
// detail screen, a game build Kiln has not measured, a fault while drawing): a tooltip is never left half drawn. Client
// checks equip.c5..equip.c9 remain unplayed.
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;

namespace Kiln;

/// <summary>Where a tooltip line goes in the game's item detail view.</summary>
public enum TooltipPlace
{
    /// <summary>After the last line the game draws (its type lines and notes).</summary>
    End = 0,
    /// <summary>Between the stats block and the description. Where the game's view has no such place, after the last
    /// line.</summary>
    AfterStats = 1,
}

/// <summary>A tooltip line's colour: the game's own palette (the rarity colours of the detail's header), a positive and a
/// negative colour, and a muted one. <see cref="Default"/> is the detail text's own colour.</summary>
public enum TooltipColor
{
    /// <summary>The detail text's own colour.</summary>
    Default = 0,
    /// <summary>A gain (the game's Uncommon colour until the guided client check equip.c7 confirms a better match).</summary>
    Positive = 1,
    /// <summary>A loss (the detail text's own colour until the guided client check equip.c7 confirms a red).</summary>
    Negative = 2,
    /// <summary>The Common rarity colour.</summary>
    Common = 3,
    /// <summary>The Uncommon rarity colour.</summary>
    Uncommon = 4,
    /// <summary>The Rare rarity colour.</summary>
    Rare = 5,
    /// <summary>The Epic rarity colour.</summary>
    Epic = 6,
    /// <summary>The Legendary rarity colour.</summary>
    Legendary = 7,
    /// <summary>The colour the game gives unique items.</summary>
    Unique = 8,
    /// <summary>The colour of an item without a rarity.</summary>
    Muted = 9,
}

/// <summary>One line in the game's item detail view: its text, colour, and whether it uses the game's smaller detail text
/// size.</summary>
public readonly record struct TooltipLine(UiText Text, TooltipColor Color = TooltipColor.Default, bool Small = false);

/// <summary>A tooltip-lines provider's answer: Ok with the lines to add (at most 16 per detail view, all providers of
/// Ui.Tooltip and Ui.Values.TooltipLines together; a provider whose lines do not fit adds none), or not Ok (adds none;
/// the reason is logged once). Lines are interned for the process: keep them stable, do not make a new text every
/// frame.</summary>
public sealed record UiTooltipLines(bool Ok, string? Reason, IReadOnlyList<TooltipLine> Lines) : IKilnResult
{
    /// <summary>No lines.</summary>
    public static readonly UiTooltipLines None = new(true, null, []);

    /// <summary>These lines.</summary>
    public static UiTooltipLines With(params TooltipLine[] lines) => new(true, null, lines ?? throw new ArgumentNullException(nameof(lines)));

    /// <summary>Refuse, saying why (for the log).</summary>
    public static UiTooltipLines Refuse(string reason) => new(false, reason, []);
}

/// <summary>A provider of lines for the game's item detail view. Runs on the game client's main thread while the game
/// draws an item's detail view: read values cached on a tick (synced item data, affixes), never wait, never call
/// server-world APIs, never keep the arguments. Five exceptions in a row remove it.</summary>
[ClientMainThread]
public delegate UiTooltipLines UiTooltipLinesProvider(in UiValueContext context, in UiItemView item);

/// <summary>A tooltip-values provider's answer: Ok with a factor the detail's stat numbers are multiplied by (finite, 0 to
/// 1000; providers multiply in order), or not Ok (the numbers stay as the earlier providers left them).</summary>
public sealed record UiTooltipScale(bool Ok, string? Reason, float Factor) : IKilnResult
{
    /// <summary>No change (factor 1).</summary>
    public static readonly UiTooltipScale None = new(true, null, 1f);

    /// <summary>Multiply the numbers by <paramref name="factor"/>.</summary>
    public static UiTooltipScale By(float factor) => new(true, null, factor);

    /// <summary>Refuse, saying why (for the log).</summary>
    public static UiTooltipScale Refuse(string reason) => new(false, reason, 1f);
}

/// <summary>A provider of a factor for the stat numbers of the game's item detail view (the main damage, armour or block
/// number and the other values its stats block draws), on this client only. Same rules as
/// <see cref="UiTooltipLinesProvider"/>.</summary>
[ClientMainThread]
public delegate UiTooltipScale UiTooltipValuesProvider(in UiValueContext context, in UiItemView item);

public static partial class Ui
{
    /// <summary>Lines and stat numbers inside the game's own item detail view, on this client.</summary>
    public static partial class Tooltip
    {
        /// <summary>The row of drawing lines into the game's item detail view. Without it,
        /// <see cref="AddLines(ItemId, UiTooltipLinesProvider, TooltipPlace)"/> still works: the lines show in Kiln's
        /// "Item details" window only.</summary>
        public const string LinesCapability = "ui.tooltip.lines";

        /// <summary>The row of <see cref="AddValues(ItemId, UiTooltipValuesProvider)"/>.</summary>
        public const string ValuesCapability = "ui.tooltip.values";

        /// <summary>Adds lines for <paramref name="item"/> (default: every item) to the game's item detail view, at
        /// <paramref name="place"/>, and to Kiln's "Item details" window. Where the game's view cannot take them (row
        /// <see cref="LinesCapability"/> off, another detail screen, a fault) they are in that window only. Needs
        /// <see cref="Ui.Values.TooltipCapability"/>.</summary>
        [ClientOnly, AnyThread, RequiresCapability(Ui.Values.TooltipCapability)]
        public static UiValueRegistration AddLines(ItemId item, UiTooltipLinesProvider provider, TooltipPlace place = TooltipPlace.End) =>
            UiTooltipNative.RegisterLines(item, provider, place);

        /// <summary>Adds lines for every item. See <see cref="AddLines(ItemId, UiTooltipLinesProvider, TooltipPlace)"/>.</summary>
        [ClientOnly, AnyThread, RequiresCapability(Ui.Values.TooltipCapability)]
        public static UiValueRegistration AddLines(UiTooltipLinesProvider provider, TooltipPlace place = TooltipPlace.End) =>
            UiTooltipNative.RegisterLines(default, provider, place);

        /// <summary>Multiplies the stat numbers the game's item detail view draws for <paramref name="item"/> (default: every
        /// item) by the provider's factor, on this client only (a picture: the server's values are not changed). The
        /// numbers of the enhancement rows are not scaled in this version.</summary>
        [ClientOnly, AnyThread, RequiresCapability(ValuesCapability)]
        public static UiValueRegistration AddValues(ItemId item, UiTooltipValuesProvider provider) =>
            UiTooltipNative.RegisterValues(item, provider);

        /// <summary>Multiplies the stat numbers of every item. See <see cref="AddValues(ItemId, UiTooltipValuesProvider)"/>.</summary>
        [ClientOnly, AnyThread, RequiresCapability(ValuesCapability)]
        public static UiValueRegistration AddValues(UiTooltipValuesProvider provider) => UiTooltipNative.RegisterValues(default, provider);

        /// <summary>True while lines are drawn into the game's item detail view (false: they show in Kiln's "Item details"
        /// window only, or no provider asked yet).</summary>
        [ClientOnly, AnyThread]
        public static bool InGame => UiTooltipNative.LinesOn();
    }
}

internal static unsafe class UiTooltipNative
{
    private static readonly System.Collections.Concurrent.ConcurrentDictionary<long, UiValueBridge.Reg> Regs = new();
    private static long _next;

    internal static uint FlagsOf(in TooltipLine line, TooltipPlace place) =>
        ((uint)line.Color & 0x0fu) | (line.Small ? UiValuesNative.LineSmall : 0) |
        (place == TooltipPlace.AfterStats ? UiValuesNative.LineAfterStats : 0);

    internal static bool LinesOn()
    {
        var t = UiValuesNative.TooltipT;
        if (t == null) return false;
        UiValuesNative.TooltipNativeState s = default;
        s.Size = (uint)sizeof(UiValuesNative.TooltipNativeState);
        return t->TooltipNativeState(&s) == UiValuesNative.Ok && s.LinesOn != 0;
    }

    internal static UiValueRegistration RegisterLines(ItemId item, UiTooltipLinesProvider provider, TooltipPlace place)
    {
        ArgumentNullException.ThrowIfNull(provider);
        if (place is not (TooltipPlace.End or TooltipPlace.AfterStats)) throw new ArgumentOutOfRangeException(nameof(place));
        var row = Ui.Values.TooltipCapability;
        UiValueBridge.RequireCapability(row);
        var t = UiValuesNative.TooltipT;
        if (t == null)
        {
            // an older native service: the lines go to the details window through tooltip_lines, without their styles
            if (UiValuesNative.T == null) throw new KilnUnavailableException(row, Capabilities.Why(UiValuesNative.Service));
            return Ui.Values.TooltipLines(item, (in UiValueContext c, in UiItemView v) =>
            {
                var d = provider(in c, in v);
                if (d is null || d.Lines is null) return null!;
                return d.Ok ? new UiTooltipDecision(true, null, d.Lines.Select(l => l.Text).ToArray()) : UiTooltipDecision.Refuse(d.Reason ?? "refused");
            });
        }
        return Register(row, item, (reg, r) => t->TooltipLinesNative(UiValueBridge.OwnerOf(reg.Mod), item.Value,
            UiValuesNative.LinesInGameTooltip, &OnLines, (void*)reg.Id, r), reg =>
        {
            reg.NativeLines = provider;
            reg.Place = place;
        }, provider);
    }

    internal static UiValueRegistration RegisterValues(ItemId item, UiTooltipValuesProvider provider)
    {
        ArgumentNullException.ThrowIfNull(provider);
        var row = Ui.Tooltip.ValuesCapability;
        UiValueBridge.RequireCapability(row);
        var t = UiValuesNative.TooltipT;
        if (t == null) throw new KilnUnavailableException(row, "kiln.ui.values has no tooltip_values (an older Kiln core)");
        return Register(row, item, (reg, r) => t->TooltipValues(UiValueBridge.OwnerOf(reg.Mod), item.Value, &OnScale,
            (void*)reg.Id, r), reg => reg.Values = provider, provider);
    }

    private delegate int Native(UiValueBridge.Reg reg, UiValuesNative.Registration* r);

    private static UiValueRegistration Register(string row, ItemId item, Native call, Action<UiValueBridge.Reg> fill, Delegate provider)
    {
        var mod = UiModCalls.Caller ?? ModContext.OwnerOf(provider);
        UiModCalls.RequireAlive(mod);
        var reg = new UiValueBridge.Reg
        {
            Id = Interlocked.Increment(ref _next), Kind = UiValueKind.Detail, Item = item, Mod = mod,
            ModName = mod?.Info.Id ?? "kiln",
        };
        fill(reg);
        Regs[reg.Id] = reg;
        UiValuesNative.Registration r = default;
        r.Size = (uint)sizeof(UiValuesNative.Registration);
        var rc = call(reg, &r);
        if (rc != UiValuesNative.Ok || r.Ok == 0) Regs.TryRemove(reg.Id, out _);
        switch (rc)
        {
            case UiValuesNative.Ok when r.Ok != 0:
                break;
            case UiValuesNative.Ok:
                return new UiValueRegistration(false, UiValueBridge.ReasonText(r.Reason), 0, null);
            case UiValuesNative.Unavailable:
                throw new KilnUnavailableException(row, Capabilities.Is(row) ? "kiln.ui.values could not hook the item detail view (see kiln.log)" : Capabilities.Why(row));
            case UiValuesNative.Revoked:
                throw new InvalidOperationException("the mod is unloading");
            default:
                throw new ArgumentException($"kiln.ui.values refused the tooltip registration ({rc})");
        }
        reg.Handle = r.Handle;
        var id = reg.Id;
        var handle = r.Handle;
        reg.Public = new UiValueRegistration(true, null, handle, _ =>
        {
            if (!Regs.TryRemove(id, out var gone)) return;
            var table = UiValuesNative.T;
            if (table != null) table->Unsubscribe(handle);
            UiValueEvents.ProviderChanged(gone, UiValueProviderChange.Removed);
        });
        UiValueEvents.ProviderChanged(reg, UiValueProviderChange.Added);
        if (mod is not null)
        {
            mod.Retiring += () => reg.Public.Dispose();
            if (!mod.Alive) reg.Public.Dispose();
        }
        return reg.Public;
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    internal static int OnLines(UiValuesNative.ProviderContext* c, UiValuesNative.ItemView* v, UiValuesNative.LineWriter* w, void* user)
    {
        try
        {
            return CallLines(c, v, w, (long)user);
        }
        catch (Exception e)
        {
            UiEngine.Log(LogLevel.Error, $"ui.tooltip: lines bridge: {e}");
            return UiValuesNative.Internal;
        }
    }

    internal static int CallLines(UiValuesNative.ProviderContext* c, UiValuesNative.ItemView* v, UiValuesNative.LineWriter* w, long id)
    {
        if (!Regs.TryGetValue(id, out var reg) || reg.Mod is { Alive: false } || reg.NativeLines is null) return UiValuesNative.Ok;
        var ctx = new UiValueContext(UiValueKind.Detail, c->Session, c->Frame, c->ClientStep);
        var view = UiValueBridge.ViewOf(v);
        UiTooltipLines? d;
        var prevMod = ModContext.CurrentMod;
        if (reg.Mod is not null) ModContext.CurrentMod = reg.Mod;
        try
        {
            d = reg.NativeLines(in ctx, in view);
        }
        catch (Exception e)
        {
            UiValueBridge.FaultOf(reg, $"threw {e.GetType().Name}: {e.Message}", e, Drop);
            return UiValuesNative.Internal;
        }
        finally
        {
            if (reg.Mod is not null) ModContext.CurrentMod = prevMod;
        }
        if (d is null || d.Lines is null)
        {
            UiValueBridge.FaultOf(reg, "returned no decision", null, Drop);
            return UiValuesNative.Internal;
        }
        if (!d.Ok)
        {
            UiValueBridge.RefusedOf(reg, ctx.Frame, d.Reason ?? "refused");
            return UiValuesNative.Unavailable;
        }
        var styled = w->Size >= sizeof(UiValuesNative.LineWriter) && w->AppendLine != null;
        foreach (var line in d.Lines)
        {
            if (line.Text is null || (uint)line.Color > (uint)TooltipColor.Muted)
            {
                UiValueBridge.FaultOf(reg, "returned a line without text or with an unknown colour", null, Drop);
                return UiValuesNative.Internal;
            }
            var rc = UiValuesNative.Intern(c, line.Text.ToString(), out var tid);
            if (rc == UiValuesNative.BufferTooSmall)
            {
                UiValueBridge.RefusedOf(reg, ctx.Frame, "text budget");
                return UiValuesNative.Unavailable;
            }
            if (rc != UiValuesNative.Ok)
            {
                UiValueBridge.FaultOf(reg, "returned a line that is not valid UTF-8 text", null, Drop);
                return UiValuesNative.Internal;
            }
            var appended = styled ? w->AppendLine(w, tid, FlagsOf(in line, reg.Place)) : w->Append(w, tid);
            if (appended != UiValuesNative.Ok)
            {
                UiValueBridge.RefusedOf(reg, ctx.Frame, "more than 16 lines in the detail view");
                return UiValuesNative.Unavailable;  // the native writer refuses this provider's lines as a whole
            }
        }
        reg.Faults = 0;
        reg.LastRefusal = null;
        return UiValuesNative.Ok;
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    internal static int OnScale(UiValuesNative.ProviderContext* c, UiValuesNative.ItemView* v, float* scale, void* user)
    {
        try
        {
            return CallScale(c, v, scale, (long)user);
        }
        catch (Exception e)
        {
            UiEngine.Log(LogLevel.Error, $"ui.tooltip: values bridge: {e}");
            return UiValuesNative.Internal;
        }
    }

    internal static int CallScale(UiValuesNative.ProviderContext* c, UiValuesNative.ItemView* v, float* scale, long id)
    {
        if (!Regs.TryGetValue(id, out var reg) || reg.Mod is { Alive: false } || reg.Values is null) return UiValuesNative.Ok;
        var ctx = new UiValueContext(UiValueKind.Detail, c->Session, c->Frame, c->ClientStep);
        var view = UiValueBridge.ViewOf(v);
        UiTooltipScale? d;
        var prevMod = ModContext.CurrentMod;
        if (reg.Mod is not null) ModContext.CurrentMod = reg.Mod;
        try
        {
            d = reg.Values(in ctx, in view);
        }
        catch (Exception e)
        {
            UiValueBridge.FaultOf(reg, $"threw {e.GetType().Name}: {e.Message}", e, Drop);
            return UiValuesNative.Internal;
        }
        finally
        {
            if (reg.Mod is not null) ModContext.CurrentMod = prevMod;
        }
        if (d is null)
        {
            UiValueBridge.FaultOf(reg, "returned no decision", null, Drop);
            return UiValuesNative.Internal;
        }
        if (!d.Ok)
        {
            UiValueBridge.RefusedOf(reg, ctx.Frame, d.Reason ?? "refused");
            return UiValuesNative.Unavailable;
        }
        var next = *scale * d.Factor;
        if (!float.IsFinite(d.Factor) || d.Factor < 0 || !float.IsFinite(next) || next > 1000f)
        {
            UiValueBridge.FaultOf(reg, $"returned the factor {d.Factor} (finite, 0 to 1000 in all)", null, Drop);
            return UiValuesNative.Internal;
        }
        *scale = next;
        reg.Faults = 0;
        reg.LastRefusal = null;
        return UiValuesNative.Ok;
    }

    private static void Drop(long id)
    {
        if (!Regs.TryRemove(id, out var reg)) return;
        var t = UiValuesNative.T;
        if (t != null && reg.Handle != 0) t->Unsubscribe(reg.Handle);
        UiValueEvents.ProviderChanged(reg, UiValueProviderChange.Faulted);
    }

    internal static void ResetForTests() => Regs.Clear();
}
