// SPDX-License-Identifier: MIT
namespace Kiln;

internal sealed partial class InstanceStore
{
    // These callbacks run inside Native's quiescent housekeeping point. They only touch memory and readable item
    // entities. In particular, neither user event handlers nor the save writer may run before Native regains control.
    internal void StorageParked(CharacterKey character, EntityId player, EntityId holder, int offset, string key)
    {
        if (_world is not { } w || character.IsNone || character.IsAccountOnly) return;
        _housekeeping++;
        try
        {
            var entry = EntryOfCharacter(character, create: true)!;
            IReadOnlyList<InstanceItem> part = [], whole = [];
            var partReady = false;
            var wholeReady = false;
            try
            {
                w.Require();
                // Event.Target, not the list, owns this lifetime. An unregister may already have removed its row.
                var raw = w is StorageInstanceWorld storage ? storage.ReadPhysical(holder, out partReady) : w.ReadHolder(holder, out partReady);
                part = raw.Select(i => i with { Place = new InstancePlace(player, offset + i.Place.FlatSlot) }).ToArray();
                whole = w.ReadHolder(player, out wholeReady).Concat(part).DistinctBy(i => i.Ref.Entity.Id.Value).OrderBy(i => i.Place.FlatSlot).ToArray();
                if (w.Holder(player) is { Kind: HolderKind.Player } identity && identity.Character == character)
                {
                    if (entry.Bound != player || entry.BoundVisit != identity.Visit)
                    {
                        if (entry.IsBound) Unbind(entry);
                        Bind(entry, player, identity.Visit);
                    }
                }
            }
            catch (KilnUnavailableException)
            {
                // An unavailable read never authorizes deletion or a guessed certain witness.
            }
            var certain = partReady && wholeReady && whole.Select(i => i.Place.FlatSlot).Distinct().Count() == whole.Count;
            if (certain)
            {
                var id = JoinIdBase | (++entry.Joins & ~JoinIdBase);
                entry.Witness[id] = new Witness(whole.Select(i => new WitnessEntry(i.Place.FlatSlot, i.Fields)).ToList(), true);
                AddState(entry, id, player, whole);
                foreach (var item in whole)
                    if (LiveOf(item.Ref) is { } observed)
                    {
                        observed.Seen = id;
                        observed.SeenSlot = item.Place.FlatSlot;
                        observed.SeenEntry = entry;
                    }
            }
            var ids = part.Select(i => i.Ref.Entity.Id.Value).ToHashSet();
            foreach (var live in _live.Values.Where(l => ids.Contains(l.Ref.Entity.Id.Value) ||
                (!partReady && l.Place.Holder == player && l.Place.FlatSlot >= offset && l.Place.FlatSlot < offset + 64)).ToArray())
            {
                if (part.FirstOrDefault(i => i.Ref == live.Ref) is { Ref.IsNone: false } fresh)
                {
                    live.Fields = fresh.Fields;
                    live.Place = fresh.Place;
                }
                _live.Remove(live.Ref.Entity.Id.Value);
                Unfollow(live); // Later tracker Gone belongs to the frozen record, not an item destruction.
                var (seen, slot, alt) = Observe(live, entry);
                var record = new SavedRecord
                {
                    Id = live.RecordId, Slot = slot, Fields = live.Fields, Seen = seen, Alt = alt,
                    OnlyTrusted = !certain, Since = _owner.Generation,
                    Reason = $"storage {key} parked before its holder was destroyed",
                };
                record.Values.Take(live.Values);
                entry.Records.Add(record);
                Parked(entry, record, InstanceParkReason.NoCandidate);
            }
            entry.Matched = true; // Rebuilt invalidates this; ordinary ticks must not retry the deleted holder.
        }
        finally { _housekeeping--; }
    }

    internal void StorageRestored(CharacterKey character, EntityId player)
    {
        if (_world is not { } w || EntryOfCharacter(character, create: false) is not { } entry) return;
        _housekeeping++;
        try
        {
            entry.Matched = false;
            if (w.Holder(player) is { Kind: HolderKind.Player } identity && identity.Character == character)
                TryBindPlayer(w, entry, player, identity.Visit);
        }
        catch (KilnUnavailableException)
        {
            // Matched stays invalidated: a normal tick retries once reads are available.
        }
        finally { _housekeeping--; }
    }
}
