// SPDX-License-Identifier: MIT
// The fake kiln.items' container renames (kiln.items v9, since 0.2.0-dev.30.3): container_rename and rename_status with
// kiln.items' rules (native/services/items/rename_logic.h and rename.cpp, ported): the text checked like the game's
// Rename field at the call, the entity checked at the call and again at the start of the next step, where the name is
// stored and the task completes; an empty name clears it; at most NameLimit different names, as the game's string table.
// The kit has no v3 table, so Items.ContainerName stays offline: a test reads names with HarnessItems.ContainerName.
namespace Kiln.TestKit;

internal sealed unsafe partial class ItemsFake
{
    public const string RenameFeature = "items.rename";

    private sealed class RenameJob
    {
        public required ulong Ticket, Owner, Step;
        public required uint Entity;
        public required string Text;
        public required nint User;
        public required nint Done;  // 0 once canceled: the rename still runs, nobody is told
    }

    private readonly Dictionary<uint, string> _names = [];
    private readonly List<RenameJob> _renamePending = [];
    private uint _renamesDone, _renamesRefused;

    /// <summary>How many different names the game's string table holds (its record pool: 1024 on the known builds).</summary>
    public int NameLimit { get; set; } = 1024;

    /// <summary>The name a container carries now; null when it has none.</summary>
    public string? NameOf(uint entity)
    {
        lock (_lock) return _names.TryGetValue(entity, out var n) ? n : null;
    }

    // rename_logic.h check_text: KILN_ITEMS_RENAME_DONE, TOO_LONG or BAD_TEXT and why
    internal static (int Status, string Why) CheckText(ReadOnlySpan<byte> s)
    {
        if (s.Length > ItemsAbi.NameMaxBytes) return (ItemsAbi.RenameTooLong, $"the name is {s.Length} bytes of UTF-8; the game's Rename field holds at most {ItemsAbi.NameMaxBytes}");
        var chars = 0;
        for (var i = 0; i < s.Length;)
        {
            var c = s[i];
            int cp, n;
            if (c < 0x80) (cp, n) = (c, 1);
            else if (c is >= 0xC2 and <= 0xDF) (cp, n) = (c & 0x1F, 2);
            else if (c is >= 0xE0 and <= 0xEF) (cp, n) = (c & 0x0F, 3);
            else if (c is >= 0xF0 and <= 0xF4) (cp, n) = (c & 0x07, 4);
            else return (ItemsAbi.RenameBadText, $"byte {i} (0x{c:x2}) does not start a UTF-8 character");
            if (i + n > s.Length) return (ItemsAbi.RenameBadText, $"the UTF-8 character at byte {i} is cut off");
            for (var k = 1; k < n; k++)
            {
                if ((s[i + k] & 0xC0) != 0x80) return (ItemsAbi.RenameBadText, $"byte {i + k} (0x{s[i + k]:x2}) is not a UTF-8 continuation");
                cp = (cp << 6) | (s[i + k] & 0x3F);
            }
            if ((n == 3 && cp < 0x800) || (n == 4 && cp is < 0x10000 or > 0x10FFFF) || cp is >= 0xD800 and <= 0xDFFF)
                return (ItemsAbi.RenameBadText, $"the UTF-8 character at byte {i} is not valid (U+{cp:X4})");
            if ((cp < 0x20 && cp != 0x09) || cp == 0x7F)
                return (ItemsAbi.RenameBadText, $"the name holds a control character (U+{cp:X4} at byte {i}); the game's Rename field drops it");
            i += n;
            chars++;
        }
        if (chars > ItemsAbi.NameMaxChars) return (ItemsAbi.RenameTooLong, $"the name is {chars} characters; the game's Rename field takes at most {ItemsAbi.NameMaxChars}");
        return (ItemsAbi.RenameDone, "");
    }

    private bool IsContainer(uint entity) => Of(entity) is { IsPlayer: false };

    public int Rename(ulong owner, ItemsAbi.RenameRequest* r, nint done, nint user, ulong* ticket, ItemsAbi.RenameResult* refusal)
    {
        int Refuse(int rc, int status, string why)
        {
            if (refusal != null)
            {
                *refusal = new ItemsAbi.RenameResult { Size = (uint)sizeof(ItemsAbi.RenameResult), Status = status, Entity = r != null ? r->Entity : 0 };
                SpawnAbi.CopyText(why, refusal->Message, ItemsAbi.MessageMax);
            }
            return rc;
        }
        // rename.cpp rename_submit(), in its order
        if (r == null || r->Size < (uint)sizeof(ItemsAbi.RenameRequest) || r->Reserved != 0)
            return Refuse(FakeTables.InvalidArgument, ItemsAbi.RenameBadText, "no request, a short one, or reserved fields not 0");
        if (r->Entity == 0) return Refuse(FakeTables.InvalidArgument, ItemsAbi.RenameGone, "no entity (0)");
        var len = (int)Math.Min(r->Len, (uint)ItemsAbi.NameCapacity + 1);
        var (status, why) = r->Len > ItemsAbi.NameCapacity
            ? (ItemsAbi.RenameTooLong, $"the name is {r->Len} bytes of UTF-8; the game's Rename field holds at most {ItemsAbi.NameMaxBytes}")
            : CheckText(new ReadOnlySpan<byte>(r->Text, len));
        if (status != ItemsAbi.RenameDone) return Refuse(FakeTables.InvalidArgument, status, why);
        if (FakeTables.Off(RenameFeature)) return Refuse(FakeTables.Unavailable, ItemsAbi.RenameUnavailable, "items.rename is off");
        if (FakeTables.OnTickThread && _harness.EntitiesFake is not null)
        {
            if (!World.Exists(r->Entity)) return Refuse(FakeTables.NotFound, ItemsAbi.RenameGone, "no such entity");
            if (!IsContainer(r->Entity)) return Refuse(FakeTables.NotFound, ItemsAbi.RenameNotContainer, "not a container (no inventory of its own, or a player's or an NPC's)");
        }
        var job = new RenameJob
        {
            Ticket = (ulong)Interlocked.Increment(ref _tickets), Owner = owner, Step = _harness.CurrentStep, Entity = r->Entity,
            Text = System.Text.Encoding.UTF8.GetString(r->Text, (int)r->Len), User = user, Done = done,
        };
        lock (_lock)
        {
            if (_renamePending.Count >= MaxPending) return Refuse(FakeTables.Unavailable, ItemsAbi.RenameUnavailable, "1024 renames are waiting");
            _renamePending.Add(job);
        }
        if (ticket != null) *ticket = job.Ticket;
        return FakeTables.Ok;
    }

    private bool CancelRename(ulong ticket)
    {
        lock (_lock)
        {
            var job = _renamePending.FirstOrDefault(j => j.Ticket == ticket && j.Done != 0);
            if (job is null) return false;
            job.Done = 0;
            return true;
        }
    }

    private void CancelRenameOwner(ulong owner)
    {
        lock (_lock)
            foreach (var job in _renamePending.Where(j => j.Owner == owner)) job.Done = 0;
    }

    public void FillRename(ref ItemsAbi.RenameStatus s)
    {
        lock (_lock)
        {
            s.Feature = 1;  // kiln.items' resolve: fixed for the process (a test switches the row, not this)
            s.Server = 1;   // the kit's harness is the server
            s.Available = FakeTables.Off(RenameFeature) ? (byte)0 : (byte)1;
            s.Pending = (uint)_renamePending.Count;
            s.Done = _renamesDone;
            s.Refused = _renamesRefused;
        }
        if (s.Available == 0)
            fixed (byte* d = s.Detail) SpawnAbi.CopyText("items.rename is off", d, ItemsAbi.MessageMax);
    }

    // rename_logic.h run(): the entity again, then the name stored (or cleared); the task completes now (the game's: in the
    // next sim step's Pre)
    private void RunRenames(ulong step)
    {
        List<RenameJob> jobs;
        lock (_lock)
        {
            jobs = [.. _renamePending];
            _renamePending.Clear();
        }
        foreach (var job in jobs)
        {
            var r = new ItemsAbi.RenameResult
            {
                Size = (uint)sizeof(ItemsAbi.RenameResult), Ticket = job.Ticket, Entity = job.Entity,
                Steps = (uint)Math.Max(0L, (long)step - (long)job.Step - 1),
            };
            string message;
            if (FakeTables.Off(RenameFeature))
                (r.Status, message) = (ItemsAbi.RenameUnavailable, "renames were switched off");
            else if (_harness.EntitiesFake is null || !World.Exists(job.Entity))
                (r.Status, message) = (ItemsAbi.RenameGone, $"entity {job.Entity} does not exist (any more)");
            else if (!IsContainer(job.Entity))
                (r.Status, message) = (ItemsAbi.RenameNotContainer, $"entity {job.Entity} is not a container (no inventory of its own, or a player's or an NPC's)");
            else
            {
                lock (_lock)
                {
                    _names.TryGetValue(job.Entity, out var old);
                    if ((old ?? "") == job.Text)
                    {
                        r.Status = ItemsAbi.RenameDone;
                        r.Cleared = job.Text.Length == 0 ? (byte)1 : (byte)0;
                        message = job.Text.Length == 0 ? $"entity {job.Entity} has no name to clear" : $"entity {job.Entity} already has this name";
                    }
                    // kiln.items refuses before set_string, which would release the old name first: the old one still counts
                    else if (job.Text.Length > 0 && !_names.ContainsValue(job.Text) && _names.Values.Distinct().Count() >= NameLimit)
                    {
                        r.Status = ItemsAbi.RenameFull;
                        message = $"the game's string table holds its maximum of {NameLimit} different names";
                    }
                    else
                    {
                        if (job.Text.Length == 0) _names.Remove(job.Entity);
                        else _names[job.Entity] = job.Text;
                        r.Status = ItemsAbi.RenameDone;
                        r.Changed = 1;
                        r.Cleared = job.Text.Length == 0 ? (byte)1 : (byte)0;
                        message = job.Text.Length == 0 ? $"entity {job.Entity}'s name cleared" : $"entity {job.Entity} renamed";
                    }
                }
            }
            lock (_lock)
            {
                if (r.Status == ItemsAbi.RenameDone) _renamesDone++;
                else _renamesRefused++;
            }
            SpawnAbi.CopyText(message, r.Message, ItemsAbi.MessageMax);
            if (job.Done != 0) ((delegate* unmanaged[Cdecl]<ItemsAbi.RenameResult*, void*, void>)job.Done)(&r, (void*)job.User);
        }
    }
}
