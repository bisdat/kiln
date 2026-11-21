// SPDX-License-Identifier: MIT
// Renaming a container: kiln.items v9 (native/core/abi/services/kiln_items.h "renaming a container"), since 0.2.0-dev.30.3
// (mod API 0.2.13). GitHub issue #112, thanks Fuzen777. The reader, ContainerName, is in Moves.cs (v3).
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Text;

namespace Kiln;

public static unsafe partial class Items
{
    // The v9 table, or null when the native service is older.
    private static ItemsNative.TableV9* V9
    {
        get
        {
            var t = T;
            return t != null && t->Header.Size >= ItemsNative.V9RenameEnd && ((ItemsNative.TableV9*)t)->ContainerRename != null
                ? (ItemsNative.TableV9*)t
                : null;
        }
    }

    /// <summary>
    /// True when <see cref="SetContainerName"/> can rename here now: capability <c>items.rename</c> (the game build's rename
    /// is known), and this process runs the server world (a dedicated server, or the game client hosting the world). False on
    /// a game client joined to another machine's world: names are set where the world runs. Since 0.2.0-dev.30.3 (mod API
    /// 0.2.13).
    /// </summary>
    public static bool ContainerRenameAvailable => Can(Caps.Rename) && RenameStatusOf(V9) is { Available: true };

    /// <summary>
    /// Renames a container the way a player's Rename button does, through the game's own rename on the server: the name
    /// replicates to every player, is kept in the save, and shows in the game's Rename screen and in
    /// <see cref="ContainerName"/>, like one a player typed. An empty <paramref name="name"/> clears the name. Since
    /// 0.2.0-dev.30.3 (mod API 0.2.13).
    /// </summary>
    /// <param name="entity">The container: an entity with an inventory of its own that is neither a player's nor an NPC's
    /// (a chest, a storage station).</param>
    /// <param name="name">The new name; "" clears it. Checked like the game's Rename field: at most
    /// <see cref="ContainerNameMaxChars"/> characters (Unicode code points) and <see cref="ContainerNameMaxBytes"/> bytes of
    /// UTF-8, no control character other than tab. Kept as given: no trimming and no other filter (the game has none for
    /// names).</param>
    /// <returns>Completes on the tick thread in the next sim step: true when the game took the name (also when the
    /// container already had it), false when the rename could not run there (the entity went away or stopped being a
    /// container before the step, the game's table of names was full, the server world closed or stopped stepping). The
    /// reason of a false is in kiln.log.</returns>
    /// <exception cref="ArgumentNullException"><paramref name="name"/> is null.</exception>
    /// <exception cref="ArgumentException">No entity, an entity that does not exist or is not a container, or a name that is
    /// too long, not valid text, or holds a control character; the message says which.</exception>
    /// <exception cref="InvalidOperationException">This process does not run the server world (a game client joined to
    /// another machine's world, or in its menu).</exception>
    /// <exception cref="NotSupportedException">Capability <c>items.rename</c> is off (an older Kiln, a game build whose
    /// rename is not known).</exception>
    /// <example><code>
    /// foreach (var chest in Items.FindInventories(InventoryKinds.Containers))
    ///     if (Items.ContainerName(chest) is null) await Items.SetContainerName(chest, "Unsorted");
    /// </code></example>
    [RequiresCapability(Caps.Rename)]
    [TickThreadOnly]
    public static Task<bool> SetContainerName(EntityId entity, string name)
    {
        ArgumentNullException.ThrowIfNull(name);
        Services.RequireTickThread("Items.SetContainerName");
        if (entity.IsNone) throw new ArgumentException("Items.SetContainerName: no entity", nameof(entity));
        var bytes = ContainerNameBytes(name);
        var t = V9;
        if (t == null)
            throw T == null
                ? ServiceCaps.Unavailable(Caps.Service, "kiln.items is unavailable")
                : ServiceCaps.Unavailable(Caps.Rename, "Items.SetContainerName: this Kiln runtime's kiln.items cannot rename containers (update Kiln)");
        var mod = ModContext.CurrentMod;
        if (mod is { Alive: false }) throw new InvalidOperationException("the mod is unloading");
        var r = new ItemsNative.RenameRequest { Size = (uint)sizeof(ItemsNative.RenameRequest), Entity = entity.Value, Len = (uint)bytes.Length };
        bytes.CopyTo(new Span<byte>(r.Text, ItemsNative.NameCapacity));
        var tcs = new TaskCompletionSource<bool>(TaskCreationOptions.RunContinuationsAsynchronously);
        long key;
        lock (RenameGate)
        {
            key = ++_nextRename;
            RenamesInFlight[key] = new PendingRename(tcs, mod);
            if (mod != null && RenamesWatched.Add(mod)) mod.Retiring += () => RetireRenames(mod);
        }
        ulong ticket = 0;
        ItemsNative.RenameResult refusal = default;
        var rc = t->ContainerRename(Services.CallerOwner, &r, &OnRenamed, (void*)key, &ticket, &refusal);
        if (rc != ItemsNative.Ok)
        {
            lock (RenameGate) RenamesInFlight.Remove(key);
            throw RenameRefusal(rc, refusal.Status, ItemsNative.Text(refusal.Message, 160), entity);
        }
        lock (RenameGate)
        {
            if (RenamesInFlight.TryGetValue(key, out var p)) p.Ticket = ticket;
        }
        return tcs.Task;
    }

    /// <summary>The most characters (Unicode code points) <see cref="SetContainerName"/> takes: the game's Rename field's
    /// limit (48). Since 0.2.0-dev.30.3 (mod API 0.2.13).</summary>
    public const int ContainerNameMaxChars = ItemsNative.NameMaxChars;

    /// <summary>The most bytes of UTF-8 <see cref="SetContainerName"/> takes: what the game's Rename field holds (64).
    /// Since 0.2.0-dev.30.3 (mod API 0.2.13).</summary>
    public const int ContainerNameMaxBytes = ItemsNative.NameMaxBytes;

    // UTF-8 of a .NET string: refuses a lone surrogate (no text a player could type) and anything past the request's buffer
    // (kiln.items checks the game's rules on the rest and says which one failed).
    private static readonly UTF8Encoding StrictUtf8 = new(encoderShouldEmitUTF8Identifier: false, throwOnInvalidBytes: true);

    private static byte[] ContainerNameBytes(string name)
    {
        byte[] bytes;
        try
        {
            bytes = StrictUtf8.GetBytes(name);
        }
        catch (EncoderFallbackException)
        {
            throw new ArgumentException("Items.SetContainerName: the name is not valid text (a lone UTF-16 surrogate)", nameof(name));
        }
        if (bytes.Length > ItemsNative.NameCapacity)
            throw new ArgumentException($"Items.SetContainerName: the name is {bytes.Length} bytes of UTF-8; the game's Rename field holds at most {ItemsNative.NameMaxBytes}", nameof(name));
        return bytes;
    }

    // What a refusal at the call throws (pure, so every branch is tested without the native table).
    internal static Exception RenameRefusal(int rc, int status, string message, EntityId entity)
    {
        var why = message.Length > 0 ? message : "refused";
        return status switch
        {
            ItemsNative.RenameTooLong or ItemsNative.RenameBadText => new ArgumentException($"Items.SetContainerName: {why}", "name"),
            ItemsNative.RenameGone => new ArgumentException($"Items.SetContainerName: entity {entity.Value}: {why}", nameof(entity)),
            ItemsNative.RenameNotContainer => new ArgumentException($"Items.SetContainerName: entity {entity.Value} is {why}", nameof(entity)),
            ItemsNative.RenameNotServer => new InvalidOperationException($"Items.SetContainerName: {why}"),
            _ when rc == ItemsNative.InvalidArgument => new ArgumentException($"Items.SetContainerName: {why}"),
            _ => ServiceCaps.Unavailable(Caps.Rename, $"Items.SetContainerName: {why}"),
        };
    }

    private static ItemsV9RenameStatus? RenameStatusOf(ItemsNative.TableV9* t)
    {
        if (t == null || t->RenameStatusOf == null) return null;
        ItemsNative.RenameStatus s = default;
        s.Size = (uint)sizeof(ItemsNative.RenameStatus);
        if (t->RenameStatusOf(&s) != ItemsNative.Ok) return null;
        return new ItemsV9RenameStatus(s.Available != 0, s.Feature != 0, s.Server != 0, ItemsNative.Text(s.Detail, 160));
    }

    internal readonly record struct ItemsV9RenameStatus(bool Available, bool Feature, bool Server, string Detail);

    private sealed class PendingRename(TaskCompletionSource<bool> tcs, ModContext? mod)
    {
        public readonly TaskCompletionSource<bool> Tcs = tcs;
        public readonly ModContext? Mod = mod;
        public ulong Ticket;
    }

    private static readonly object RenameGate = new();
    private static readonly Dictionary<long, PendingRename> RenamesInFlight = new();
    private static readonly HashSet<ModContext> RenamesWatched = new();
    private static long _nextRename;

    private static void RetireRenames(ModContext mod)
    {
        List<ulong> tickets = new();
        lock (RenameGate)
        {
            RenamesWatched.Remove(mod);
            foreach (var (k, p) in RenamesInFlight.Where(kv => kv.Value.Mod == mod).ToList())
            {
                RenamesInFlight.Remove(k);
                if (p.Ticket != 0) tickets.Add(p.Ticket);
            }
        }
        var t = _table;
        if (t != null) foreach (var tk in tickets) t->Cancel(tk);
    }

    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
    private static void OnRenamed(ItemsNative.RenameResult* r, void* user)
    {
        try
        {
            PendingRename? p;
            lock (RenameGate)
            {
                if (!RenamesInFlight.Remove((long)user, out p)) return;
            }
            p.Tcs.TrySetResult(r->Status == ItemsNative.RenameDone);
        }
        catch
        {
            // never let an exception cross into native code
        }
    }
}
