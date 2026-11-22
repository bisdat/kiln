// SPDX-License-Identifier: MIT
// The fake kiln.ui.snapshot (native/core/abi/services/kiln_ui_snapshot.h, v1) under Kiln.Blocks' real BlockUi.ReadWorld,
// BlockUi.DemandWorld and the block runtime's world.* bindings: demand handles whose union says what a publication carries,
// tokens that pin one whole publication (an undersized copy writes nothing, a world change revokes every token), the
// budgets of the header. A test publishes the values (KilnHarness.UiSnapshot); the game's collector (05a's stage, the
// client world's Post, GetEntityTransform, kiln.time), the 100 ms cadence and the age limits are not modelled.
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;

namespace Kiln.TestKit;

/// <summary>Mirror of <c>kiln_ui_snapshot.h</c>.</summary>
internal static unsafe class UiSnapshotAbi
{
    public const string Service = "kiln.ui.snapshot";
    public const uint Version = 1;
    public const uint KnownMask = 15;  // KilnUiWorldMask
    public const int MaxRows = 512, OwnerDemands = 64, AllDemands = 1024, OwnerTokens = 2, AllTokens = 128;
    public const int Revoked = 6;      // KILN_ERR_REVOKED

    // KilnUiSnapshotReason
    public const uint NoWorld = 1, NoSnapshot = 2, Budget = 3, Expired = 4, DeferredSource = 6, MissingComponent = 7,
        StaleSource = 8, LayoutOff = 9;

    public static readonly IReadOnlyDictionary<string, uint> ReasonNames = new Dictionary<string, uint>(StringComparer.Ordinal)
    {
        ["NoWorld"] = NoWorld, ["NoSnapshot"] = NoSnapshot, ["Budget"] = Budget, ["Expired"] = Expired,
        ["DeferredSource"] = DeferredSource, ["MissingComponent"] = MissingComponent, ["StaleSource"] = StaleSource,
        ["LayoutOff"] = LayoutOff,
    };

    [StructLayout(LayoutKind.Sequential)]
    public struct Acquire
    {
        public uint Size, Ok, Reason, Reserved;
        public ulong Token, Publication, CaptureMs;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct Values
    {
        public uint Size, AvailableMask, RequestedMask, InventoryCount;
        public fixed uint UnavailableReason[4];
        public ulong Publication, LocalSession, LocalStep, LocalCaptureMs;
        public uint LocalPlayer, Health, HealthMax, Stamina, StaminaMax, Mana, ManaMax;
        public long XFixed, YFixed, ZFixed;
        public ulong ServerSession, ServerStep, ServerCaptureMs;
        public double ServerHours;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct InventoryRow
    {
        public uint Holder, InventoryPage, SlotIndex, Item, Count, Pide;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct DemandResult
    {
        public uint Size, Ok, Reason, Reserved;
        public ulong Handle;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct Table
    {
        public ServiceHeader Header;
        public delegate* unmanaged[Cdecl]<ulong, uint, DemandResult*, int> DemandAdd;
        public delegate* unmanaged[Cdecl]<ulong, int> DemandRemove;
        public delegate* unmanaged[Cdecl]<ulong, Acquire*, int> AcquireSnapshot;
        public delegate* unmanaged[Cdecl]<ulong, Values*, InventoryRow*, uint, uint*, int> Copy;
        public delegate* unmanaged[Cdecl]<ulong, int> Release;
    }
}

internal sealed class UiSnapshotFakeState
{
    /// <summary>One publication: the header as copy writes it and its records, never changed after it is made.</summary>
    public sealed class Publication
    {
        public UiSnapshotAbi.Values Header;
        public UiSnapshotAbi.InventoryRow[] Rows = [];
    }

    public sealed record Token(ulong Owner, Publication Pinned, ulong World);

    public readonly object Lock = new();
    public readonly Dictionary<ulong, (ulong Owner, uint Mask)> Demands = [];
    public readonly Dictionary<ulong, Token> Tokens = [];
    public Publication? Current;
    /// <summary>Bumped by a world change: tokens of an older world are revoked.</summary>
    public ulong World = 1;
    public bool HasWorld = true;
    public ulong NextHandle = 1, NextPublication = 1;

    public uint Requested()
    {
        var mask = 0u;
        foreach (var d in Demands.Values) mask |= d.Mask;
        return mask;
    }
}

internal sealed unsafe class UiSnapshotFake : IKitFake
{
    public string Service => UiSnapshotAbi.Service;

    private static UiSnapshotAbi.Table* _table;

    public void Register()
    {
        if (_table == null)
        {
            var t = (UiSnapshotAbi.Table*)NativeMemory.AllocZeroed((nuint)sizeof(UiSnapshotAbi.Table));  // process lifetime
            t->Header = new ServiceHeader { Size = (uint)sizeof(UiSnapshotAbi.Table), Version = UiSnapshotAbi.Version };
            t->DemandAdd = &DemandAdd;
            t->DemandRemove = &DemandRemove;
            t->AcquireSnapshot = &Acquire;
            t->Copy = &Copy;
            t->Release = &Release;
            _table = t;
        }
        KitBackend.Instance.RegisterTable(UiSnapshotAbi.Service, (nint)_table);
        // ui.blocks.world and world.* are Kiln.Blocks' rows: it reports them when it finds this table (world.vitals and
        // world.inventory also follow ui.values.hud, the kit's kiln.ui.values fake's row)
    }

    public void Start(KilnHarness harness) { }
    public void BeforeStep(KilnHarness harness, ulong step) { }

    public void End(KilnHarness harness)
    {
        var st = KitFakes.State<UiSnapshotFakeState>(harness);
        lock (st.Lock)
        {
            st.Demands.Clear();
            st.Tokens.Clear();
            st.Current = null;
            st.World++;
        }
    }

    public void AddCoverage(List<CoverageRow> rows) =>
        rows.Add(new CoverageRow("Live game values (BlockUi.ReadWorld)", CoverageKind.RecordingFake,
            [UiSnapshotAbi.Service, "ui.blocks.world", "world.vitals", "world.position", "world.inventory", "world.host_time"],
            "Kiln.Blocks' own BlockUi.ReadWorld, DemandWorld and the world.* bindings on the kit's kiln.ui.snapshot: demands join " +
            "into the groups a publication carries (64 per mod, 1024 in all), a test publishes the local player's vitals, position, " +
            "own inventory and the hosted world's hour (KilnHarness.UiSnapshot.Publish): what nobody demanded is NoSnapshot, more " +
            "than 512 slots is Budget, the hour on a JoinedClient harness is DeferredSource. A read pins one whole publication (2 " +
            "tokens per mod, 128 in all; an undersized copy writes nothing and the retry gets the same one); EndWorld clears it and " +
            "revokes every token (NoWorld until the next Publish). Offline: the game's collector (05a's stage, the client world's " +
            "Post, GetEntityTransform, kiln.time), the 100 ms cadence, the 1000 ms age and the 2 s token life; the fake answers on " +
            "every side, a dedicated server's harness included. Window state, mods' own data and Server-target commands need " +
            "kiln.ui and kiln.net, which the kit does not fake."));

    internal static UiSnapshotFakeState? State => KitBackend.Instance.Harness is { } h ? KitFakes.State<UiSnapshotFakeState>(h) : null;

    private static bool Live(out UiSnapshotFakeState st)
    {
        st = null!;
        if (FakeTables.Active(UiSnapshotAbi.Service) is null || State is not { } s) return false;
        st = s;
        return true;
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int DemandAdd(ulong owner, uint mask, UiSnapshotAbi.DemandResult* r)
    {
        try
        {
            if (r == null || r->Size < (uint)sizeof(UiSnapshotAbi.DemandResult) || owner == 0 || mask == 0 || (mask & ~UiSnapshotAbi.KnownMask) != 0)
                return FakeTables.InvalidArgument;
            if (!Live(out var st)) return FakeTables.Unavailable;
            lock (st.Lock)
            {
                if (st.Demands.Count >= UiSnapshotAbi.AllDemands || st.Demands.Values.Count(d => d.Owner == owner) >= UiSnapshotAbi.OwnerDemands)
                {
                    r->Ok = 0;
                    r->Reason = UiSnapshotAbi.Budget;
                    r->Handle = 0;
                    return FakeTables.Ok;
                }
                var h = (0x5557ul << 48) | st.NextHandle++;
                st.Demands[h] = (owner, mask);
                r->Ok = 1;
                // HOST_TIME on a joined world is accepted, and nothing will come (v1 has no network subscription for it)
                r->Reason = (mask & (uint)UiWorldFields.HostTime) != 0 && Joined() ? UiSnapshotAbi.DeferredSource : 0;
                r->Handle = h;
            }
            return FakeTables.Ok;
        }
        catch
        {
            return FakeTables.Internal;
        }
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int DemandRemove(ulong handle)
    {
        if (handle == 0) return FakeTables.InvalidArgument;
        if (State is not { } st) return FakeTables.NotFound;
        lock (st.Lock) return st.Demands.Remove(handle) ? FakeTables.Ok : FakeTables.NotFound;
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int Acquire(ulong owner, UiSnapshotAbi.Acquire* a)
    {
        try
        {
            if (a == null || a->Size < (uint)sizeof(UiSnapshotAbi.Acquire) || owner == 0) return FakeTables.InvalidArgument;
            if (!Live(out var st)) return FakeTables.Unavailable;
            lock (st.Lock)
            {
                a->Ok = 0;
                a->Token = 0;
                a->Publication = 0;
                a->CaptureMs = 0;
                if (!st.HasWorld || st.Current == null)
                {
                    a->Reason = st.HasWorld ? UiSnapshotAbi.NoSnapshot : UiSnapshotAbi.NoWorld;
                    return FakeTables.Ok;
                }
                var live = st.Tokens.Values.Where(t => t.World == st.World).ToArray();
                if (live.Length >= UiSnapshotAbi.AllTokens || live.Count(t => t.Owner == owner) >= UiSnapshotAbi.OwnerTokens)
                {
                    a->Reason = UiSnapshotAbi.Budget;
                    return FakeTables.Ok;
                }
                var token = (0x5554ul << 48) | st.NextHandle++;
                st.Tokens[token] = new UiSnapshotFakeState.Token(owner, st.Current, st.World);
                a->Ok = 1;
                a->Reason = 0;
                a->Token = token;
                a->Publication = st.Current.Header.Publication;
                a->CaptureMs = st.Current.Header.LocalCaptureMs;
            }
            return FakeTables.Ok;
        }
        catch
        {
            return FakeTables.Internal;
        }
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int Copy(ulong token, UiSnapshotAbi.Values* v, UiSnapshotAbi.InventoryRow* rows, uint capacity, uint* required)
    {
        try
        {
            if (token == 0 || v == null || v->Size < (uint)sizeof(UiSnapshotAbi.Values) || (capacity > 0 && rows == null))
                return FakeTables.InvalidArgument;
            if (State is not { } st) return FakeTables.NotFound;
            lock (st.Lock)
            {
                if (!st.Tokens.TryGetValue(token, out var t)) return FakeTables.NotFound;
                if (t.World != st.World) return UiSnapshotAbi.Revoked;
                var pub = t.Pinned;
                if (required != null) *required = (uint)pub.Rows.Length;
                if (capacity < (uint)pub.Rows.Length) return FakeTables.BufferTooSmall;  // nothing written
                var size = v->Size;  // the caller's: bytes past v1 stay as they are
                *v = pub.Header;
                v->Size = size;
                for (var i = 0; i < pub.Rows.Length; i++) rows[i] = pub.Rows[i];
            }
            return FakeTables.Ok;
        }
        catch
        {
            return FakeTables.Internal;
        }
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int Release(ulong token)
    {
        if (token == 0) return FakeTables.InvalidArgument;
        if (State is not { } st) return FakeTables.NotFound;
        lock (st.Lock) return st.Tokens.Remove(token) ? FakeTables.Ok : FakeTables.NotFound;
    }

    private static bool Joined() => KitBackend.Instance.Harness?.Options.Side == HarnessSide.JoinedClient;

    /// <summary>Makes the next publication from a test's sample (KilnHarness.UiSnapshot.Publish): what nobody demanded is
    /// NoSnapshot, a group the sample lacks is NoSnapshot too, a reason the sample gives wins.</summary>
    internal static ulong Publish(KilnHarness harness, FakeWorldSample sample)
    {
        ArgumentNullException.ThrowIfNull(sample);
        var reasons = new uint[4];
        foreach (var (field, name) in sample.Unavailable ?? new Dictionary<UiWorldFields, string>())
        {
            var bit = (uint)field;
            if (bit == 0 || (bit & (bit - 1)) != 0 || (bit & ~UiSnapshotAbi.KnownMask) != 0)
                throw new ArgumentException($"{field} is not one group of live game values", nameof(sample));
            if (!UiSnapshotAbi.ReasonNames.TryGetValue(name, out var code))
                throw new ArgumentException($"\"{name}\" is not a reason kiln.ui.snapshot gives ({string.Join(", ", UiSnapshotAbi.ReasonNames.Keys)})", nameof(sample));
            reasons[System.Numerics.BitOperations.TrailingZeroCount(bit)] = code;
        }
        var slots = (sample.Inventory ?? []).OrderBy(s => s.InventoryPage).ThenBy(s => s.SlotIndex).ToArray();
        var position = sample.Position is { } p ? (X: Fixed(p.X), Y: Fixed(p.Y), Z: Fixed(p.Z)) : default;  // checked before anything changes
        var st = KitFakes.State<UiSnapshotFakeState>(harness);
        lock (st.Lock)
        {
            var requested = st.Requested();
            if (slots.Length > UiSnapshotAbi.MaxRows && reasons[2] == 0) reasons[2] = UiSnapshotAbi.Budget;  // never a partial list
            if (harness.Options.Side == HarnessSide.JoinedClient && reasons[3] == 0) reasons[3] = UiSnapshotAbi.DeferredSource;  // a joined world's hour: never read remotely
            bool[] given = [sample.Vitals != null, sample.Position != null, sample.Inventory != null, sample.HostHours != null];
            var available = 0u;
            var unavailable = new uint[4];
            for (var i = 0; i < 4; i++)
            {
                var reason = (requested & (1u << i)) == 0 ? UiSnapshotAbi.NoSnapshot
                    : reasons[i] != 0 ? reasons[i]
                    : !given[i] ? UiSnapshotAbi.NoSnapshot
                    : 0u;
                if (reason == 0) available |= 1u << i;
                unavailable[i] = reason;
            }
            var h = new UiSnapshotAbi.Values { Size = (uint)sizeof(UiSnapshotAbi.Values), RequestedMask = requested };
            for (var i = 0; i < 4; i++) h.UnavailableReason[i] = unavailable[i];
            h.AvailableMask = available;
            h.Publication = st.NextPublication++;
            h.LocalSession = st.World;
            h.LocalStep = harness.CurrentStep;
            h.LocalCaptureMs = (ulong)Environment.TickCount64;
            h.LocalPlayer = sample.LocalPlayer;
            if ((available & (uint)UiWorldFields.LocalVitals) != 0 && sample.Vitals is { } vit)
            {
                h.Health = vit.Health;
                h.HealthMax = vit.HealthMax;
                h.Stamina = vit.Stamina;
                h.StaminaMax = vit.StaminaMax;
                h.Mana = vit.Mana;
                h.ManaMax = vit.ManaMax;
            }
            if ((available & (uint)UiWorldFields.LocalPosition) != 0)
            {
                h.XFixed = position.X;
                h.YFixed = position.Y;
                h.ZFixed = position.Z;
            }
            var rows = Array.Empty<UiSnapshotAbi.InventoryRow>();
            if ((available & (uint)UiWorldFields.LocalInventory) != 0)
                rows = slots.Select(s => new UiSnapshotAbi.InventoryRow
                {
                    Holder = sample.LocalPlayer, InventoryPage = s.InventoryPage, SlotIndex = s.SlotIndex, Item = s.Item, Count = s.Count,
                    Pide = s.Pide,
                }).ToArray();
            h.InventoryCount = (uint)rows.Length;
            if ((available & (uint)UiWorldFields.HostTime) != 0 && sample.HostHours is { } hours)
            {
                h.ServerSession = st.World;
                h.ServerStep = harness.CurrentStep;
                h.ServerCaptureMs = h.LocalCaptureMs;
                h.ServerHours = hours;
            }
            st.Current = new UiSnapshotFakeState.Publication { Header = h, Rows = rows };
            st.HasWorld = true;
            return h.Publication;
        }
    }

    private static long Fixed(double metres)
    {
        if (!double.IsFinite(metres) || Math.Abs(metres) >= 2147483648d)
            throw new ArgumentOutOfRangeException(nameof(metres), metres, "a position is a finite number of metres within ±2147483648 (32.32 fixed point)");
        return (long)Math.Round(metres * 4294967296d);
    }

    internal static void EndWorld(KilnHarness harness)
    {
        var st = KitFakes.State<UiSnapshotFakeState>(harness);
        lock (st.Lock)
        {
            st.Current = null;
            st.HasWorld = false;
            st.World++;  // every token of the old world is revoked, also within its life
        }
    }
}
