// SPDX-License-Identifier: MIT
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;

namespace Kiln;

/// <summary>One returned GEMS row. SourceRow is the selection identity; DisplayedGem names the actual gem source slot.
/// WeaponCarrier is present for a gem already socketed in a weapon. Client identities never authorize server actions.</summary>
public sealed record GemForgeCandidate(uint Index, UiItemView SourceRow, UiItemView DisplayedGem, UiItemView? WeaponCarrier);

/// <summary>The game's returned GEMS list for the local player's inventories, including offscreen rows. Selection is
/// the context at grid entry. Mode is the game's byte (0..4). Generation changes when the list or context changes.
/// CaptureMs uses Environment.TickCount64's clock. Eligibility and native disabled state are unknown.</summary>
public sealed record GemForgeCandidatesSnapshot(ulong Session, ulong Frame, ulong CaptureMs, ulong Generation,
    EntityId LocalPlayer, byte Mode, SlotId? SelectedRow, UiItemView? SelectedWeapon, IReadOnlyList<GemForgeCandidate> Candidates);

/// <summary>Unavailable when the GEMS grid did not draw, changed during capture, expired, or could not be copied.
/// No partial list is returned. This observes the game's returned list, not an authoritative inventory enumeration.</summary>
public sealed record GemForgeCandidatesResult(bool Ok, string? Reason, GemForgeCandidatesSnapshot? Snapshot) : IKilnResult;

public static partial class Ui
{
    public static partial class Values
    {
        /// <summary>The read-only Gem Forge snapshot capability.</summary>
        public const string GemForgeCandidatesCapability = "ui.gemforge.inspect";

        /// <summary>Copies the latest completed GEMS grid snapshot. It observes the local player's inventories,
        /// not nearby chests. A missing capability throws KilnUnavailableException.</summary>
        [ClientOnly, AnyThread, RequiresCapability(GemForgeCandidatesCapability)]
        public static GemForgeCandidatesResult GemForgeCandidates() => UiGemForgeBridge.Read();

        /// <summary>Calls onFrame after each UI frame on the client's main thread, including unavailable frames.
        /// Keep the callback short. Dispose the registration to stop; mod unload also stops it.</summary>
        [ClientOnly, AnyThread, RequiresCapability(GemForgeCandidatesCapability)]
        public static UiValueRegistration SubscribeGemForgeCandidates([ClientMainThread] Action<GemForgeCandidatesResult> onFrame) =>
            UiGemForgeBridge.Subscribe(onFrame);
    }
}

internal static unsafe class UiGemForgeBridge
{
    private const string Row = Ui.Values.GemForgeCandidatesCapability;
    private const uint Limit = 4096;
    private static readonly System.Collections.Concurrent.ConcurrentDictionary<long, (Action<GemForgeCandidatesResult> Fn, ModContext? Mod)> Subs = new();
    private static long _next;
    internal static Func<long> Now = () => Environment.TickCount64;
    private static GemForgeCandidatesResult No(string reason) => new(false, reason, null);
    private static UiValuesNative.GemForgeTable* Table()
    {
        UiValueBridge.RequireCapability(Row);
        var t = UiValuesNative.GemForgeT;
        if (t == null) throw new KilnUnavailableException(Row, "kiln.ui.values has no Gem Forge snapshot table");
        return t;
    }
    internal static GemForgeCandidatesResult Read()
    {
        var t = Table();
        var owner = UiValueBridge.OwnerOf(UiModCalls.Caller);
        ulong session = 0, frame = 0;
        uint capacity = 64;
        for (int attempt = 0; attempt < 4; ++attempt)
        {
            var rows = new UiValuesNative.GemForgeCandidate[capacity];
            rows[0].Size = (uint)sizeof(UiValuesNative.GemForgeCandidate);
            UiValuesNative.GemForgeFrame meta = new() { Size = (uint)sizeof(UiValuesNative.GemForgeFrame) };
            uint required = 0;
            int rc;
            fixed (UiValuesNative.GemForgeCandidate* p = rows)
                rc = t->CopyGemForge(owner, session, frame, &meta, p, capacity, &required);
            if (rc == UiValuesNative.BufferTooSmall)
            {
                if (required > Limit || meta.Count > Limit) return No("more than 4096 Gem Forge candidates");
                (session, frame, capacity) = (meta.Session, meta.Frame, Math.Max(required, Math.Min(Limit, capacity * 2)));
                continue;
            }
            if (rc == UiValuesNative.Ok)
            {
                if (meta.Ok == 0 && meta.Reason == UiValuesNative.ReasonStaleFrame && session != 0)
                { session = frame = 0; continue; }
                return Result(&meta, rows, meta.Ok != 0 ? meta.Count : 0);
            }
            if (rc == UiValuesNative.Unavailable) throw new KilnUnavailableException(Row, Capabilities.Why(Row));
            if (rc == UiValuesNative.Revoked) throw new InvalidOperationException("the mod is unloading");
            throw new InvalidOperationException($"kiln.ui.values: copy_gemforge_candidates failed ({rc})");
        }
        return No("the Gem Forge list changed faster than it could be copied");
    }
    internal static GemForgeCandidatesResult Result(UiValuesNative.GemForgeFrame* meta,
        ReadOnlySpan<UiValuesNative.GemForgeCandidate> rows, uint count)
    {
        if (meta == null || meta->Size < sizeof(UiValuesNative.GemForgeFrame)) return No("invalid Gem Forge frame");
        if (meta->Ok == 0) return No(meta->Reason switch
        {
            UiValuesNative.ReasonStaleFrame => "stale Gem Forge frame",
            UiValuesNative.ReasonFrameBudget => "more than 4096 Gem Forge candidates",
            _ => "no completed GEMS grid",
        });
        var now = Now();
        if (now < 0 || meta->CaptureMs > (ulong)now || (ulong)now - meta->CaptureMs > 1000) return No("stale Gem Forge frame");
        if (count > Limit || count != meta->Count || count > rows.Length || meta->Mode > 4 || meta->Role != 0 ||
            (meta->HasSelectedRow != 0 && meta->SelectedIndex > 7) ||
            (meta->HasSelectedWeapon != 0 && meta->SelectedWeapon.Size < sizeof(UiValuesNative.ItemView)))
            return No("invalid Gem Forge frame");
        var candidates = new GemForgeCandidate[count];
        for (int i = 0; i < count; ++i)
        {
            var r = rows[i];
            if (r.Size < sizeof(UiValuesNative.GemForgeCandidate) || r.SourceRow.Size < sizeof(UiValuesNative.ItemView) ||
                r.DisplayedGem.Size < sizeof(UiValuesNative.ItemView) ||
                (r.HasWeaponCarrier != 0 && r.WeaponCarrier.Size < sizeof(UiValuesNative.ItemView))) return No("invalid Gem Forge row");
            candidates[i] = new(r.Index, UiValueBridge.ViewOf(&r.SourceRow), UiValueBridge.ViewOf(&r.DisplayedGem),
                r.HasWeaponCarrier != 0 ? UiValueBridge.ViewOf(&r.WeaponCarrier) : null);
        }
        return new(true, null, new(meta->Session, meta->Frame, meta->CaptureMs, meta->Generation,
            new EntityId(meta->LocalPlayer), (byte)meta->Mode,
            meta->HasSelectedRow != 0 ? SlotId.Of(new EntityId(meta->SelectedPage), (int)meta->SelectedIndex) : null,
            meta->HasSelectedWeapon != 0 ? UiValueBridge.ViewOf(&meta->SelectedWeapon) : null, Array.AsReadOnly(candidates)));
    }
    internal static UiValueRegistration Subscribe(Action<GemForgeCandidatesResult> onFrame)
    {
        ArgumentNullException.ThrowIfNull(onFrame);
        var t = Table();
        var mod = UiModCalls.Caller ?? ModContext.OwnerOf(onFrame);
        UiModCalls.RequireAlive(mod);
        var id = Interlocked.Increment(ref _next);
        Subs[id] = (onFrame, mod);
        UiValuesNative.Registration reg = new() { Size = (uint)sizeof(UiValuesNative.Registration) };
        var rc = t->SubscribeGemForge(UiValueBridge.OwnerOf(mod), &OnFrame, (void*)id, &reg);
        if (rc != UiValuesNative.Ok || reg.Ok == 0) Subs.TryRemove(id, out _);
        if (rc == UiValuesNative.Unavailable) throw new KilnUnavailableException(Row, Capabilities.Why(Row));
        if (rc == UiValuesNative.Revoked) throw new InvalidOperationException("the mod is unloading");
        if (rc != UiValuesNative.Ok) throw new ArgumentException($"kiln.ui.values refused subscribe_gemforge_candidates ({rc})");
        if (reg.Ok == 0) return new(false, UiValueBridge.ReasonText(reg.Reason), 0, null);
        var handle = reg.Handle;
        var result = new UiValueRegistration(true, null, handle, _ =>
        {
            Subs.TryRemove(id, out var ignored);
            var current = UiValuesNative.T;
            if (current != null) current->Unsubscribe(handle);
        });
        if (mod is not null) { mod.Retiring += result.Dispose; if (!mod.Alive) result.Dispose(); }
        return result;
    }
    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    internal static void OnFrame(UiValuesNative.GemForgeFrame* meta, UiValuesNative.GemForgeCandidate* rows, uint count, void* user)
    {
        try
        {
            if (!Subs.TryGetValue((long)user, out var sub)) return;
            var result = !Capabilities.Is(Row) ? No("Gem Forge inspection is unavailable") :
                count > Limit || (count != 0 && rows == null) ? No("invalid Gem Forge rows") :
                Result(meta, new ReadOnlySpan<UiValuesNative.GemForgeCandidate>(rows, (int)count), count);
            UiModCalls.Invoke(sub.Mod, sub.Fn, result, "Gem Forge subscriber");
        }
        catch (Exception e) { UiEngine.Log(LogLevel.Error, $"ui.values: Gem Forge subscriber: {e}"); }
    }
    internal static void ResetForTests() => Subs.Clear();
}
