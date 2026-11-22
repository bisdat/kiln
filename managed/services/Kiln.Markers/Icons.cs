// SPDX-License-Identifier: MIT
// Markers.Icons: the game's map marker types that have an icon image (chapter 04 §4.3). A client reads them from the
// game's own resources once they are loaded; a dedicated server has no such resources and takes the catalog from a
// verified client. Icons are drawn on the map only; the compass and world surfaces draw a coloured glyph and text.
using System.Collections.ObjectModel;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;

namespace Kiln;

/// <summary>One icon of the catalog. Immutable.</summary>
/// <param name="Icon">The type, for <see cref="MarkerSpec.Icon"/>.</param>
/// <param name="Key">Its canonical name, <c>type:</c> and eight lower-case hex digits (<c>type:ffe5e444</c>).</param>
/// <param name="Alias">Its friendly name: the texture name without a leading <c>mapmarker_</c>, lower-cased
/// (<c>well_plant</c>). Two types can share one; then only the key names it.</param>
/// <param name="TextureName">The icon texture's name in the game.</param>
/// <param name="Texture">The icon image's GUID.</param>
/// <param name="Flags">The type's metadata: 1 has an image, 2 shows a label, 4 shows a range, 8 above the fog of war, 16 a
/// custom-pin type, 32 a fast-travel type.</param>
/// <param name="Surfaces"><see cref="MarkerSurface.Map"/> when it may be used on the map; none for the six fast-travel
/// types, which are listed but refused until their behaviour on the map is checked in the game.</param>
public sealed record MarkerIconInfo(MarkerIcon Icon, string Key, string Alias, string TextureName, Guid Texture, uint Flags, MarkerSurface Surfaces);

/// <summary>The answer of <see cref="Markers.Icons.Resolve"/>, by Kiln's error rule.</summary>
/// <param name="Ok">True when the name named exactly one icon.</param>
/// <param name="Reason">Why not: <c>NotFound</c>, <c>Ambiguous</c> (an alias of several types: use the key) or
/// <c>CatalogNotReady</c>; null when <see cref="Ok"/>.</param>
/// <param name="Icon">The icon; null unless <see cref="Ok"/>.</param>
public sealed record MarkerIconResult(bool Ok, string? Reason, MarkerIconInfo? Icon) : IKilnResult;

/// <summary>The icon catalog became ready (or could not be read). Immutable; safe to read on any thread.</summary>
[AnyThread]
public sealed class MarkerCatalogChangedEvent
{
    internal MarkerCatalogChangedEvent(uint role, bool ready, ulong generation, int count, string? reason)
    {
        Role = role;
        Ready = ready;
        Generation = generation;
        Count = count;
        Reason = reason;
    }

    /// <summary>Whose catalog: 0 the server's, 1 this client's.</summary>
    public uint Role { get; }

    /// <summary>The catalog can be read now.</summary>
    public bool Ready { get; }

    /// <summary>The catalog's generation (1 for the first one; it does not change for the life of the process).</summary>
    public ulong Generation { get; }

    /// <summary>Its icons.</summary>
    public int Count { get; }

    /// <summary>Why native map icons stay off with a ready catalog, or why it could not be read; null otherwise.</summary>
    public string? Reason { get; }
}

public static unsafe partial class Markers
{
    /// <summary>The game's map marker types with an icon image, by canonical key and alias. Warm-up: on a client the
    /// catalog is read once the game has loaded its resources; on a dedicated server it arrives from the first verified
    /// client. Check <see cref="IsReady"/>; if false, listen for <see cref="CatalogChanged"/> and check again (an event is
    /// not replayed for a late listener).</summary>
    public static class Icons
    {
        private static ReadOnlyCollection<MarkerIconInfo> _all = new([]);
        private static ulong _allGeneration;
        private static readonly object AllLock = new();

        /// <summary>Whether the catalog has warmed up.</summary>
        /// <exception cref="KilnUnavailableException"><c>markers.catalog</c> is off.</exception>
        [AnyThread, RequiresCapability("markers.catalog")]
        public static bool IsReady
        {
            get
            {
                Count(out var count, out var generation);
                return count > 0 && generation > 0;
            }
        }

        /// <summary>Every icon, sorted by id; empty while warming up. An immutable snapshot.</summary>
        /// <exception cref="KilnUnavailableException"><c>markers.catalog</c> is off.</exception>
        [AnyThread, RequiresCapability("markers.catalog")]
        public static IReadOnlyList<MarkerIconInfo> All
        {
            get
            {
                Count(out var count, out var generation);
                lock (AllLock)
                {
                    if (generation == _allGeneration) return _all;
                    var t = MarkersNative.T;
                    var rows = new List<MarkerIconInfo>((int)count);
                    for (uint i = 0; i < count; i++)
                    {
                        MarkersNative.Icon icon = default;
                        icon.Size = (uint)sizeof(MarkersNative.Icon);
                        if (t->CatalogAt(i, &icon) != MarkersNative.Ok) break;
                        rows.Add(Info(&icon));
                    }
                    _all = new ReadOnlyCollection<MarkerIconInfo>(rows.ToArray());
                    _allGeneration = generation;
                    return _all;
                }
            }
        }

        /// <summary>The icon a name names: its canonical key (<c>type:ffe5e444</c>) or an exact alias
        /// (<c>well_plant</c>). The name is lower-cased; nothing else is guessed (no prefix stripping, no partial
        /// match).</summary>
        /// <param name="name">1 to 128 printable ASCII characters without surrounding spaces.</param>
        /// <returns>The icon, or a refusal: <c>NotFound</c>, <c>Ambiguous</c>, <c>CatalogNotReady</c>.</returns>
        /// <exception cref="ArgumentNullException"><paramref name="name"/> is null.</exception>
        /// <exception cref="ArgumentException"><paramref name="name"/> is not a name (empty, too long, not ASCII,
        /// surrounding spaces, <c>type:</c> without eight hex digits).</exception>
        /// <exception cref="KilnUnavailableException"><c>markers.catalog</c> is off.</exception>
        [AnyThread, RequiresCapability("markers.catalog")]
        public static MarkerIconResult Resolve(string name)
        {
            ArgumentNullException.ThrowIfNull(name);
            MarkersCaps.Require(MarkersCaps.Catalog);
            var t = MarkersNative.T;
            var bytes = System.Text.Encoding.UTF8.GetBytes(name);
            MarkersNative.Icon icon = default;
            icon.Size = (uint)sizeof(MarkersNative.Icon);
            MarkersNative.Result r = default;
            r.Size = (uint)sizeof(MarkersNative.Result);
            int rc;
            fixed (byte* p = bytes) rc = t->CatalogResolve(new NativeStr { Ptr = p, Len = (nuint)bytes.Length }, &icon, &r);
            var reason = MarkersNative.Text(r.Reason, 256);
            return rc switch
            {
                MarkersNative.Ok when r.Status == MarkersNative.Accepted => new MarkerIconResult(true, null, Info(&icon)),
                MarkersNative.Ok => new MarkerIconResult(false, reason.Length > 0 ? reason : $"status {r.Status}", null),
                MarkersNative.InvalidArgument => throw new ArgumentException(reason.Length > 0 ? reason : $"'{name}' is not an icon name", nameof(name)),
                MarkersNative.Unavailable => throw MarkersCaps.Off(MarkersCaps.Catalog),
                _ => throw new InvalidOperationException($"kiln.markers answered {rc}{(reason.Length > 0 ? ": " + reason : "")}"),
            };
        }

        private static void Count(out uint count, out ulong generation)
        {
            MarkersCaps.Require(MarkersCaps.Catalog);
            uint c = 0;
            ulong g = 0;
            var rc = MarkersNative.T->CatalogCount(&c, &g);
            if (rc == MarkersNative.Unavailable) throw MarkersCaps.Off(MarkersCaps.Catalog);
            count = rc == MarkersNative.Ok ? c : 0;
            generation = rc == MarkersNative.Ok ? g : 0;
        }

        private static MarkerIconInfo Info(MarkersNative.Icon* i) =>
            new(new MarkerIcon(i->Id), MarkersNative.Text(i->Key, 14), MarkersNative.Text(i->Alias, 129), MarkersNative.Text(i->TextureName, 129),
                new Guid(new ReadOnlySpan<byte>(i->TextureGuid, 16)), i->Flags, (MarkerSurface)(i->Surfaces & 7));

        /// <summary>Tests: forget the cached list.</summary>
        internal static void Forget()
        {
            lock (AllLock)
            {
                _all = new ReadOnlyCollection<MarkerIconInfo>([]);
                _allGeneration = 0;
            }
        }
    }

    /// <summary>The icon catalog became ready, or could not be read (<see cref="MarkerCatalogChangedEvent.Ready"/> false
    /// with a reason). Raised once per catalog, at the next step of the world of its role; not replayed for a handler added
    /// later (check <see cref="Icons.IsReady"/> first). A handler that throws is logged and removed; a mod's handlers go
    /// when it unloads.</summary>
    [AnyThread]
    public static event Action<MarkerCatalogChangedEvent>? CatalogChanged
    {
        add
        {
            CatalogHandlers.Add(value);
            EnsureSubscribed();
        }
        remove => CatalogHandlers.Remove(value);
    }

    internal static readonly MarkerHandlers<Action<MarkerCatalogChangedEvent>> CatalogHandlers = new("Markers.CatalogChanged");

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static void OnCatalog(MarkersNative.CatalogChange* c, void* user)
    {
        try
        {
            if (c == null || c->Size < (uint)sizeof(MarkersNative.CatalogChange)) return;
            var reason = MarkersNative.Text(c->Reason, 256);
            var e = new MarkerCatalogChangedEvent(c->Role, c->Ready != 0, c->Generation, (int)c->Count, reason.Length > 0 ? reason : null);
            CatalogHandlers.Raise(h => h(e));
        }
        catch (Exception x)
        {
            try { Backend.Current.Log(0, LogLevel.Error, $"kiln.markers: a catalog change could not be delivered: {x.Message}"); }
            catch { /* nothing left to say */ }
        }
    }
}
