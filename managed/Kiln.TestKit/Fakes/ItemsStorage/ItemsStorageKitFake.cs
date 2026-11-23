// SPDX-License-Identifier: MIT
// The kit's fake kiln.items.storage (0.2.0-dev.30.7, GitHub issue 128): the shared model (Shared/ItemsStorageFake.cs) with its
// holders as fake containers of the kit's kiln.items, so Items.Read, Give, Take and FindInventories(PlayerStorage) see them
// and a give into a holder meets the storage's filter. The test plays the game and Kiln.Saves through
// KilnHarness.PlayerStorage (KilnHarness.PlayerStorage.cs).
namespace Kiln.TestKit;

internal sealed class ItemsStorageKitState
{
    public ItemsStorageFake? Fake;
}

internal sealed unsafe class ItemsStorageKitFake : IKitFake
{
    public const string Service = StorageNative.Service;

    string IKitFake.Service => Service;

    public void Register()
    {
        KitBackend.Instance.RegisterTable(Service, (nint)ItemsStorageFake.Table);
        FakeTables.Features([PlayerStorage.Capability, "items.storage.persist"]);
    }

    public void Start(KilnHarness harness)
    {
        var st = KitFakes.State<ItemsStorageKitState>(harness);
        st.Fake = new ItemsStorageFake(new KitStorageWorld(harness)).Activate();
        PlayerStorageRuntime.ResetForTests();  // registrations are per harness: an earlier harness's mods have retired
    }

    public void BeforeStep(KilnHarness harness, ulong step)
    {
        if (KitFakes.State<ItemsStorageKitState>(harness).Fake is not { } fake) return;
        fake.StepNow = step;
        fake.Step();
        fake.StepNow = step;
    }

    public void End(KilnHarness harness)
    {
        var st = KitFakes.State<ItemsStorageKitState>(harness);
        st.Fake?.Dispose();
        st.Fake = null;
        PlayerStorageRuntime.ResetForTests();
    }

    public void AddCoverage(List<CoverageRow> rows) =>
        rows.Add(new CoverageRow("Player storage", CoverageKind.RecordingFake, [Service, PlayerStorage.Capability, "items.storage.persist"],
            "Kiln.Items' player storage on the kit's kiln.items.storage: registrations with kiln.items.storage's rules (keys, " +
            "limits, filters), holders as fake containers (Items.Read, Give, Take, FindInventories with InventoryKinds.PlayerStorage; " +
            "a give the filter refuses is Filtered), restores with quarantine and parking, live registrations, captures into " +
            "batches and the captures-done stage, parks, returns and death, scripted through KilnHarness.PlayerStorage (the game's " +
            "side: Join, Apply, Restore, Capture, CapturesDone, Leave, Die). Offline: the game's record codec (records carry slot, " +
            "item, count and level), item entities in storage, moves (the kit has no v3 table), captures at the kit's own saves, " +
            "the client copy (items.storage.client)."));
}

/// <summary>Holders as fake containers of the kit's kiln.items.</summary>
internal sealed class KitStorageWorld(KilnHarness harness) : IStorageFakeWorld
{
    private ItemsFake Items => harness.ItemsFake ?? throw new InvalidOperationException("the harness has no items yet");
    private EntitiesFake World => harness.EntitiesFake ?? throw new InvalidOperationException("the harness has no world yet");

    public uint CreateHolder(int pages, Func<uint, bool> accepts)
    {
        var e = World.Create(null);
        var inv = Items.AddContainer(e, pages);
        inv.Storage = true;
        inv.Accepts = accepts;
        return e;
    }

    public void DestroyHolder(uint holder) => Items.RemoveInventory(holder);

    public void WriteHolder(uint holder, int slots, IReadOnlyList<StorageFakeRecord> records)
    {
        if (Items.Of(holder) is not { } inv) return;
        lock (Items.Lock)
        {
            foreach (var s in inv.Slots)
            {
                s.Item = 0;
                s.Count = 0;
                s.Level = 0;
                s.Pide = 0;
            }
            foreach (var r in records)
            {
                var s = inv.Slots[r.Slot];
                s.Item = r.Item;
                s.Count = r.Count;
                s.Level = (ushort)r.Level;
            }
        }
    }

    public IReadOnlyList<StorageFakeRecord> ReadHolder(uint holder, int slots)
    {
        if (Items.Of(holder) is not { } inv) return [];
        lock (Items.Lock)
            return [.. inv.Slots.Take(slots).Select((s, i) => (s, i)).Where(x => x.s.Count > 0)
                .Select(x => new StorageFakeRecord(x.i, x.s.Item, x.s.Count, x.s.Level))];
    }

    public StorageFakeItem? Item(uint id)
    {
        if (Items.Item(id) is not { } i) return null;
        var ammo = i.EquipmentSlotType == 0 ? 0u
            : i.EquipmentSlotType == ItemsFake.EquipmentType(global::Kiln.Items.GameSlot(EquipmentSlot.Arrow)) ? StorageNative.FilterAmmoArrow
            : i.EquipmentSlotType == ItemsFake.EquipmentType(global::Kiln.Items.GameSlot(EquipmentSlot.Spell)) ? StorageNative.FilterAmmoSpell
            : 0u;
        return new StorageFakeItem(i.MaxStack, i.Category, i.Gear, ammo);
    }

    public bool Deliver(uint target, StorageFakeRecord record)
    {
        if (Items.Of(target) is not { } inv || Items.Item(record.Item) is not { } item) return false;
        lock (Items.Lock)
        {
            var free = Array.FindIndex(inv.Slots, s => s.Count == 0);
            if (free < 0) return false;
            var s = inv.Slots[free];
            s.Item = item.Id;
            s.Count = record.Count;
            s.Level = (ushort)record.Level;
            return true;
        }
    }
}
