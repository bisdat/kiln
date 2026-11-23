// SPDX-License-Identifier: MIT
// Schematics: a box of voxel cells (terrain and building blocks) plus the props in it, saved as .kschem, pasted
// elsewhere with a 90-degree rotation and an offset. Format: docs/kiln/services/build.md ("The .kschem format").
using System.IO.Compression;
using System.Numerics;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using System.Text.Json.Nodes;

namespace Kiln;

/// <summary>A prop in a schematic: template, position relative to the schematic's low corner (metres), rotation and
/// the item dismantling gives back (0 = the item that places the template; <see cref="uint.MaxValue"/> = none, what a
/// captured prop without UsedItem gets).</summary>
/// <param name="Template">The GUID of the prop's template; a paste places the prop by it.</param>
/// <param name="Name">The name of the template when the prop was captured (a label: a paste uses
/// <paramref name="Template"/>).</param>
/// <param name="Offset">The prop's position in metres from the schematic's low corner.</param>
/// <param name="Rotation">The prop's orientation as a quaternion.</param>
/// <param name="ItemId">The item dismantling gives back: 0 is the item that places the template, <see cref="uint.MaxValue"/>
/// is none.</param>
public sealed record SchematicProp(Guid Template, string Name, Vector3 Offset, Quaternion Rotation, uint ItemId);

/// <summary>How a paste writes cells.</summary>
public enum PasteMode
{
    /// <summary>Every cell of the box, air included: rooms carve themselves into terrain. Default.</summary>
    Replace = 0,
    /// <summary>Only cells that hold matter (fill &gt; 0); the world's cells stay where the schematic has air.</summary>
    Merge = 1,
}

/// <summary>Paste settings.</summary>
public sealed class PasteOptions
{
    /// <summary>Quarter turns about +Y (yaw +90 degrees each): 0..3.</summary>
    public int Rotation { get; init; }

    /// <summary>Extra offset in cells after rotation.</summary>
    public CellPos Offset { get; init; }

    /// <summary>Whether the schematic's air overwrites the world: <see cref="PasteMode.Replace"/> (the default) or
    /// <see cref="PasteMode.Merge"/>.</summary>
    public PasteMode Mode { get; init; }

    /// <summary>Place the schematic's props. Default true.</summary>
    public bool Props { get; init; } = true;

    /// <summary>Remove building props (UsedItem) already inside the target box first.</summary>
    public bool ClearProps { get; init; }

    /// <summary>How the paste may act (<see cref="BuildFlags"/>), as for <see cref="BuildOptions.Flags"/>. Default none.</summary>
    public BuildFlags Flags { get; init; }

    /// <summary>The player the paste is for (zones with <see cref="BuildFlags.RespectZones"/>, journal owner, events); none =
    /// Kiln itself.</summary>
    public EntityId Actor { get; init; }

    /// <summary>Record the paste as one journal entry so <see cref="Build.Undo"/> can revert it. Default true.</summary>
    public bool Journal { get; init; } = true;

    /// <summary>How long the paste waits for the voxel tiles to stream in. Default 30 s.</summary>
    public TimeSpan LoadTimeout { get; init; } = TimeSpan.FromSeconds(30);

    /// <summary>Shown in the journal and events. Default: <c>paste</c> and the schematic's name.</summary>
    public string? Label { get; init; }
}

/// <summary>What a paste did.</summary>
/// <param name="Box">The cells the paste covered.</param>
/// <param name="Cells">The result of the cell write, with its refusal if there was one.</param>
/// <param name="PropsPlaced">How many props were placed.</param>
/// <param name="PropsFailed">How many props could not be placed.</param>
/// <param name="PropsCleared">How many building props <see cref="PasteOptions.ClearProps"/> removed first.</param>
/// <param name="JournalId">The id of the journal entry (<see cref="Build.UndoEntry(long, TimeSpan?)"/>); 0 when the paste was not
/// journaled or recorded nothing.</param>
/// <param name="Props">The entities of the props that were placed.</param>
public sealed record PasteResult(CellBox Box, BuildResult Cells, int PropsPlaced, int PropsFailed, int PropsCleared, long JournalId,
    IReadOnlyList<EntityId> Props) : IKilnResult
{
    /// <summary>The paste was not refused and every prop was placed: <see cref="Cells"/> is not refused and
    /// <see cref="PropsFailed"/> is 0.</summary>
    public bool Ok => !Cells.Refused && PropsFailed == 0;

    /// <summary>Why the paste is not ok: the cells' refusal (<see cref="BuildResult.Reason"/>), else how many props
    /// could not be placed; null when <see cref="Ok"/>. Since 0.2.0-dev.27.</summary>
    public string? Reason => Ok ? null : Cells.Refused ? Cells.Reason : $"{PropsFailed} of {PropsPlaced + PropsFailed} props could not be placed";
}

/// <summary>Capture settings.</summary>
public sealed class CaptureOptions
{
    /// <summary>Which props to keep. Default: building props (what players placed).</summary>
    public PropFilter Props { get; init; } = PropFilter.Placed;

    /// <summary>The paste point, in cells from the low corner. Default: the bottom centre.</summary>
    public CellPos? Anchor { get; init; }

    /// <summary>The schematic's name. Default: empty.</summary>
    public string? Name { get; init; }

    /// <summary>The schematic's author. Default: empty.</summary>
    public string? Author { get; init; }

    /// <summary>The schematic's description. Default: empty.</summary>
    public string? Description { get; init; }

    /// <summary>How long <see cref="Schematic.CaptureAsync"/> waits for the voxel tiles to stream in. Default 30 s.</summary>
    public TimeSpan LoadTimeout { get; init; } = TimeSpan.FromSeconds(30);
}

/// <summary>A captured region: cells (x fastest, then y, then z) and props. Immutable once built.</summary>
public sealed class Schematic
{
    /// <summary>The version of the .kschem format that Kiln writes and reads.</summary>
    public const int FormatVersion = 1;
    private static readonly byte[] Magic = "KSCHEM\r\n"u8.ToArray();

    private readonly Voxel[] _cells;

    /// <summary>Builds a schematic from cells (x fastest, then y, then z). The array is used as it is, not copied: leave it
    /// alone afterwards.</summary>
    /// <param name="sizeX">The number of cells along x.</param>
    /// <param name="sizeY">The number of cells along y.</param>
    /// <param name="sizeZ">The number of cells along z.</param>
    /// <param name="cells"><c>sizeX * sizeY * sizeZ</c> cells.</param>
    /// <param name="props">The props, or none.</param>
    /// <param name="anchor">The paste point in cells from the low corner; default the bottom centre.</param>
    /// <param name="cellSize">The cell edge in metres; default 0.5.</param>
    /// <exception cref="ArgumentException">A size is not positive, the schematic has more than 4194304 cells, or
    /// <paramref name="cells"/> does not hold one cell per position.</exception>
    public Schematic(int sizeX, int sizeY, int sizeZ, Voxel[] cells, IReadOnlyList<SchematicProp>? props = null, CellPos? anchor = null,
        double cellSize = 0.5)
    {
        if (sizeX <= 0 || sizeY <= 0 || sizeZ <= 0 || (long)sizeX * sizeY * sizeZ > 1 << 22)
            throw new ArgumentException("schematic size must be 1..4194304 cells");
        if (cells.Length != (long)sizeX * sizeY * sizeZ) throw new ArgumentException("cell count does not match the size");
        SizeX = sizeX;
        SizeY = sizeY;
        SizeZ = sizeZ;
        _cells = cells;
        Props = props ?? [];
        Anchor = anchor ?? new CellPos(sizeX / 2, 0, sizeZ / 2);
        CellSize = cellSize;
    }

    /// <summary>The number of cells along x.</summary>
    public int SizeX { get; }

    /// <summary>The number of cells along y.</summary>
    public int SizeY { get; }

    /// <summary>The number of cells along z.</summary>
    public int SizeZ { get; }

    /// <summary>The cell edge in metres (the grid it was captured on; 0.5 by default).</summary>
    public double CellSize { get; }

    /// <summary>The cells, x fastest, then y, then z: <c>index = (z * SizeY + y) * SizeX + x</c>.</summary>
    public ReadOnlySpan<Voxel> Cells => _cells;

    /// <summary>The props, each with its position relative to the low corner.</summary>
    public IReadOnlyList<SchematicProp> Props { get; }

    /// <summary>The paste point in cells from the low corner (a paste puts this cell at the target position).</summary>
    public CellPos Anchor { get; }

    /// <summary>The schematic's name, saved in the file. Default: empty.</summary>
    public string Name { get; init; } = "";

    /// <summary>The schematic's author, saved in the file. Default: empty.</summary>
    public string Author { get; init; } = "";

    /// <summary>A description, saved in the file. Default: empty.</summary>
    public string Description { get; init; } = "";

    /// <summary>Where it was captured (null for built or loaded ones without it).</summary>
    public JsonObject? Source { get; init; }

    /// <summary>The number of cells: <c>SizeX * SizeY * SizeZ</c>.</summary>
    public long Count => _cells.Length;

    /// <summary>The cell at <paramref name="x"/>, <paramref name="y"/>, <paramref name="z"/>, counted from the low corner.
    /// The axes are not checked one by one: keep each inside the schematic's size.</summary>
    public Voxel this[int x, int y, int z] => _cells[(z * SizeY + y) * SizeX + x];

    /// <summary>Cells that hold matter (fill &gt; 0).</summary>
    public long SolidCount => _cells.LongCount(c => c.Fill > 0);

    /// <summary>SHA-256 of the cells and props (hex): what <see cref="Build.PlaceOnce(string, Schematic, WorldPosition, PasteOptions?)"/> records.</summary>
    public string Hash
    {
        get
        {
            using var h = IncrementalHash.CreateHash(HashAlgorithmName.SHA256);
            Span<byte> dims = stackalloc byte[12];
            BitConverter.TryWriteBytes(dims, SizeX);
            BitConverter.TryWriteBytes(dims[4..], SizeY);
            BitConverter.TryWriteBytes(dims[8..], SizeZ);
            h.AppendData(dims);
            h.AppendData(System.Runtime.InteropServices.MemoryMarshal.AsBytes(_cells.AsSpan()));
            foreach (var p in Props) h.AppendData(Encoding.UTF8.GetBytes($"{p.Template}|{p.Offset}|{p.Rotation}|{p.ItemId};"));
            return Convert.ToHexString(h.GetHashAndReset()).ToLowerInvariant();
        }
    }

    // ---- rotation ----

    /// <summary>The schematic turned <paramref name="quarterTurns"/> times 90 degrees about +Y (yaw +90 each):
    /// cell (x, z) of a box sizeX x sizeZ goes to (z, sizeX - 1 - x); prop offsets and rotations turn with it.</summary>
    public Schematic Rotated(int quarterTurns)
    {
        var k = ((quarterTurns % 4) + 4) % 4;
        if (k == 0) return this;
        var s = this;
        for (var i = 0; i < k; i++) s = s.Rotate90();
        return s;
    }

    private Schematic Rotate90()
    {
        int sx = SizeX, sy = SizeY, sz = SizeZ;
        var cells = new Voxel[_cells.Length];
        // new size (sz, sy, sx); old (x, y, z) -> new (z, y, sx - 1 - x)
        for (var z = 0; z < sz; z++)
            for (var y = 0; y < sy; y++)
                for (var x = 0; x < sx; x++)
                    cells[((sx - 1 - x) * sy + y) * sz + z] = _cells[(z * sy + y) * sx + x];
        var q = Quaternion.CreateFromAxisAngle(Vector3.UnitY, MathF.PI / 2);
        var w = (float)(sx * CellSize);
        var props = Props.Select(p => p with
        {
            Offset = new Vector3(p.Offset.Z, p.Offset.Y, w - p.Offset.X),
            Rotation = Quaternion.Normalize(q * p.Rotation),
        }).ToList();
        return new Schematic(sz, sy, sx, cells, props, new CellPos(Anchor.Z, Anchor.Y, sx - 1 - Anchor.X), CellSize)
        {
            Name = Name, Author = Author, Description = Description, Source = Source,
        };
    }

    /// <summary>True when both hold the same cells (and sizes).</summary>
    public bool CellsEqual(Schematic other) =>
        other.SizeX == SizeX && other.SizeY == SizeY && other.SizeZ == SizeZ && other._cells.AsSpan().SequenceEqual(_cells);

    // ---- files ----

    /// <summary>Writes the schematic as .kschem (atomically: temp file, then rename).</summary>
    public void Save(string path)
    {
        var dir = Path.GetDirectoryName(Path.GetFullPath(path));
        if (!string.IsNullOrEmpty(dir)) Directory.CreateDirectory(dir);
        var tmp = path + ".tmp";
        using (var f = File.Create(tmp)) Write(f);
        File.Move(tmp, path, true);
    }

    /// <summary>The schematic as .kschem bytes (what <see cref="Save"/> writes to a file).</summary>
    public byte[] ToBytes()
    {
        using var ms = new MemoryStream();
        Write(ms);
        return ms.ToArray();
    }

    /// <summary>Writes the schematic as .kschem to a stream. The stream stays open.</summary>
    public void Write(Stream output)
    {
        var header = new JsonObject
        {
            ["format"] = "kiln.schematic",
            ["version"] = FormatVersion,
            ["name"] = Name,
            ["author"] = Author,
            ["description"] = Description,
            ["size"] = new JsonArray(SizeX, SizeY, SizeZ),
            ["cellSize"] = CellSize,
            ["anchor"] = new JsonArray(Anchor.X, Anchor.Y, Anchor.Z),
            ["cellOrder"] = "x,y,z",
            ["cellEncoding"] = "u16le material|fill<<8, deflate",
            ["props"] = new JsonArray(Props.Select(p => (JsonNode)new JsonObject
            {
                ["template"] = p.Template.ToString(),
                ["name"] = p.Name,
                ["pos"] = new JsonArray(p.Offset.X, p.Offset.Y, p.Offset.Z),
                ["rot"] = new JsonArray(p.Rotation.X, p.Rotation.Y, p.Rotation.Z, p.Rotation.W),
                ["item"] = p.ItemId,
            }).ToArray()),
        };
        if (Source is not null) header["source"] = Source.DeepClone();
        var json = Encoding.UTF8.GetBytes(header.ToJsonString(new JsonSerializerOptions { WriteIndented = true }));
        byte[] packed;
        using (var ms = new MemoryStream())
        {
            using (var z = new DeflateStream(ms, CompressionLevel.SmallestSize, true))
                z.Write(System.Runtime.InteropServices.MemoryMarshal.AsBytes(_cells.AsSpan()));
            packed = ms.ToArray();
        }
        using var w = new BinaryWriter(output, Encoding.UTF8, true);
        w.Write(Magic);
        w.Write((uint)FormatVersion);
        w.Write((uint)json.Length);
        w.Write(json);
        w.Write((uint)packed.Length);
        w.Write(packed);
    }

    /// <summary>Reads a .kschem file.</summary>
    public static Schematic Load(string path)
    {
        using var f = File.OpenRead(path);
        return Read(f);
    }

    /// <summary>Reads a .kschem from bytes. Throws <see cref="InvalidDataException"/> when the data is not a .kschem of this
    /// version or its size or cell data is bad.</summary>
    public static Schematic FromBytes(byte[] data)
    {
        using var ms = new MemoryStream(data);
        return Read(ms);
    }

    /// <summary>Reads a .kschem from a stream, from its current position. The stream stays open. Throws
    /// <see cref="InvalidDataException"/> when the data is not a .kschem of this version or its size or cell data is
    /// bad.</summary>
    public static Schematic Read(Stream input)
    {
        using var r = new BinaryReader(input, Encoding.UTF8, true);
        var magic = r.ReadBytes(8);
        if (!magic.AsSpan().SequenceEqual(Magic)) throw new InvalidDataException("not a .kschem file");
        var version = r.ReadUInt32();
        if (version != FormatVersion) throw new InvalidDataException($".kschem version {version} (this Kiln reads {FormatVersion})");
        var jlen = r.ReadUInt32();
        if (jlen > 64 << 20) throw new InvalidDataException("header too large");
        var header = JsonNode.Parse(r.ReadBytes((int)jlen))!.AsObject();
        var size = header["size"]!.AsArray();
        int sx = (int)size[0]!, sy = (int)size[1]!, sz = (int)size[2]!;
        var count = (long)sx * sy * sz;
        if (sx <= 0 || sy <= 0 || sz <= 0 || count > 1 << 22) throw new InvalidDataException("bad size");
        var plen = r.ReadUInt32();
        var packed = r.ReadBytes((int)plen);
        var cells = new Voxel[count];
        using (var z = new DeflateStream(new MemoryStream(packed), CompressionMode.Decompress))
        {
            var bytes = System.Runtime.InteropServices.MemoryMarshal.AsBytes(cells.AsSpan());
            var got = 0;
            while (got < bytes.Length)
            {
                var n = z.Read(bytes[got..]);
                if (n <= 0) throw new InvalidDataException("cell data is short");
                got += n;
            }
        }
        var props = new List<SchematicProp>();
        if (header["props"] is JsonArray pa)
            foreach (var p in pa.OfType<JsonObject>())
            {
                var pos = p["pos"]!.AsArray();
                var rot = p["rot"]?.AsArray();
                props.Add(new SchematicProp(Guid.Parse((string)p["template"]!), (string?)p["name"] ?? "",
                    new Vector3((float)(double)pos[0]!, (float)(double)pos[1]!, (float)(double)pos[2]!),
                    rot is null ? Quaternion.Identity : new Quaternion((float)(double)rot[0]!, (float)(double)rot[1]!, (float)(double)rot[2]!, (float)(double)rot[3]!),
                    (uint?)p["item"] ?? 0));
            }
        var a = header["anchor"]?.AsArray();
        return new Schematic(sx, sy, sz, cells, props, a is null ? null : new CellPos((int)a[0]!, (int)a[1]!, (int)a[2]!),
            (double?)header["cellSize"] ?? 0.5)
        {
            Name = (string?)header["name"] ?? "",
            Author = (string?)header["author"] ?? "",
            Description = (string?)header["description"] ?? "",
            Source = header["source"] as JsonObject is { } src ? src.DeepClone().AsObject() : null,
        };
    }

    // ---- world ----

    /// <summary>Captures the cells of <paramref name="box"/> and the props in it (tiles must be loaded; see
    /// <see cref="CaptureAsync"/>).</summary>
    [TickThreadOnly]
    [RequiresCapability("kiln.build")]
    public static Schematic Capture(CellBox box, CaptureOptions? options = null)
    {
        var o = options ?? new CaptureOptions();
        var cells = Build.Read(box);
        var corner = Build.CornerOf(box.Min);
        var props = new List<SchematicProp>();
        foreach (var p in Build.Props(box, o.Props))
        {
            if ((p.Flags & PropFlags.Altar) != 0) continue;  // never copy a base's flame altar
            props.Add(new SchematicProp(p.Template, p.TemplateName,
                new Vector3((float)(p.Position.X - corner.X), (float)(p.Position.Y - corner.Y), (float)(p.Position.Z - corner.Z)),
                p.Rotation, p.ItemId == 0 ? uint.MaxValue : p.ItemId));
        }
        var grid = Build.Grid!;
        return new Schematic(box.SizeX, box.SizeY, box.SizeZ, cells, props, o.Anchor, grid.CellSize)
        {
            Name = o.Name ?? "",
            Author = o.Author ?? "",
            Description = o.Description ?? "",
            Source = new JsonObject
            {
                ["world"] = Saves.WorldId,
                ["box"] = new JsonArray(box.X, box.Y, box.Z, box.SizeX, box.SizeY, box.SizeZ),
                ["corner"] = new JsonArray(corner.X, corner.Y, corner.Z),
                ["kiln"] = Game.KilnVersion,
                ["captured"] = DateTimeOffset.UtcNow.ToString("O"),
            },
        };
    }

    /// <summary>Loads the tiles under the box, then <see cref="Capture"/>s.</summary>
    [TickThreadOnly]
    [RequiresCapability("kiln.build")]
    public static async Task<Schematic> CaptureAsync(CellBox box, CaptureOptions? options = null)
    {
        var o = options ?? new CaptureOptions();
        if (!await Build.Load(box, o.LoadTimeout)) throw new BuildException($"voxel tiles under {box} did not load", BuildNative.Unavailable);
        return Capture(box, o);
    }

    /// <summary>The cell box a paste at <paramref name="at"/> covers (the anchor cell lands on the cell of
    /// <paramref name="at"/>).</summary>
    [TickThreadOnly]
    [RequiresCapability("kiln.build")]
    public CellBox TargetBox(WorldPosition at, int rotation = 0, CellPos offset = default)
    {
        var s = Rotated(rotation);
        var c = Build.CellOf(at) - s.Anchor + offset;
        return new CellBox(c.X, c.Y, c.Z, s.SizeX, s.SizeY, s.SizeZ);
    }

    /// <summary>Pastes at <paramref name="at"/> (the anchor cell lands on its cell), turned and offset as asked. One
    /// journal entry (cells and props) for <see cref="Build.Undo"/>.</summary>
    [TickThreadOnly]
    [RequiresCapability("kiln.build")]
    public async Task<PasteResult> Paste(WorldPosition at, PasteOptions? options = null)
    {
        var o = options ?? new PasteOptions();
        var s = Rotated(o.Rotation);
        var box = TargetBox(at, o.Rotation, o.Offset);
        return await s.PasteAt(box.Min, o);
    }

    /// <summary>Pastes with the low corner at a cell (no rotation is applied here: rotate first with
    /// <see cref="Rotated"/>).</summary>
    [TickThreadOnly]
    [RequiresCapability("kiln.build")]
    public async Task<PasteResult> PasteAt(CellPos corner, PasteOptions? options = null)
    {
        var o = options ?? new PasteOptions();
        var box = new CellBox(corner.X, corner.Y, corner.Z, SizeX, SizeY, SizeZ);
        var bo = new BuildOptions { Flags = o.Flags, Actor = o.Actor, Journal = o.Journal, Label = o.Label ?? $"paste {Name}".Trim() };
        if (!await Build.Load(box, o.LoadTimeout)) return new PasteResult(box, Build.NotLoaded(box), 0, 0, 0, 0, []);
        if (!Build.Editing(BuildEditKind.Paste, box, bo)) return new PasteResult(box, Build.Canceled(box), 0, 0, 0, 0, []);
        var entry = o.Journal ? Kiln.Journal.Begin(BuildEditKind.Paste, bo) : null;
        bool[]? mask = null;
        if (o.Mode == PasteMode.Merge)
        {
            mask = new bool[_cells.Length];
            for (var i = 0; i < mask.Length; i++) mask[i] = _cells[i].Fill > 0;
        }
        var cells = Build.WriteCore(box, _cells, mask, bo, entry);
        var placed = new List<EntityId>();
        int failed = 0, cleared = 0;
        if (!cells.Refused && o.Props)
        {
            if (o.ClearProps)
                foreach (var p in Build.Props(box, PropFilter.Placed))
                    if ((p.Flags & PropFlags.Altar) == 0 && Build.RemovePropCore(p, entry)) cleared++;
            var corner0 = Build.CornerOf(corner);
            foreach (var p in Props)
            {
                var pos = new WorldPosition(corner0.X + p.Offset.X, corner0.Y + p.Offset.Y, corner0.Z + p.Offset.Z);
                try
                {
                    var tmp = new JournalEntry(BuildEditKind.Paste, "");
                    var id = Build.PlacePropCore(p.Template, null, pos, p.Rotation, p.ItemId, bo, tmp);
                    if (id.IsNone)
                    {
                        failed++;
                        continue;
                    }
                    placed.Add(id);
                    entry?.Placed.Add(id);
                }
                catch (Exception e) when (e is BuildException or ArgumentException)
                {
                    failed++;
                }
            }
        }
        Build.Finish(BuildEditKind.Paste, box, bo, cells, entry);
        return new PasteResult(box, cells, placed.Count, failed, cleared, entry?.Id ?? 0, placed);
    }
}
