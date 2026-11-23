// SPDX-License-Identifier: MIT
// One rule for every block-typed field: an inline block, a handle from Blocks.Define, or a name (in JSON: a string). A name is a
// block of this mod, "other-mod:name" for another mod's.
using System.Text.Json;
using System.Text.Json.Serialization;

namespace Kiln;

/// <summary>What <see cref="Blocks.Define{T}"/> gives back: a typed reference to the block and what it became.</summary>
public abstract class BlockHandle
{
    internal BlockHandle(string mod, string name, Guid guid, string gameName)
    {
        Mod = mod;
        Name = name;
        Guid = guid;
        GameName = gameName;
    }

    /// <summary>The mod that defined it.</summary>
    public string Mod { get; }
    /// <summary>The block's name in that mod.</summary>
    public string Name { get; }
    /// <summary>The GUID of what it became in the game (an item, a buff). The same for ever for this mod id and block name.</summary>
    public Guid Guid { get; }
    /// <summary>The game's id of it (FNV-1a of the GUID).</summary>
    public uint Id => BlockIds.Fnv(Guid);
    /// <summary>The debug name it has in the game: what <c>Content.Items.Get</c>, <c>Items.Give</c> and data packs take.</summary>
    public string GameName { get; }
    /// <summary>The block's type, for what names a block of the wrong kind.</summary>
    internal abstract Type BlockType { get; }

    /// <summary>The block as <c>mod:name</c>: the form a name takes to reach another mod's block.</summary>
    public override string ToString() => $"{Mod}:{Name}";
}

/// <inheritdoc />
public class BlockHandle<T> : BlockHandle where T : class, IBlock
{
    internal BlockHandle(string mod, string name, Guid guid, string gameName, T block) : base(mod, name, guid, gameName) => Block = block;

    /// <summary>The block as defined.</summary>
    public T Block { get; }

    internal override Type BlockType => Block.GetType();
}

/// <summary>An effect: inline, from <see cref="Blocks.Define{T}"/>, or by name.</summary>
[JsonConverter(typeof(RefConverter<EffectRef, EffectBlock>))]
public sealed class EffectRef : IBlockRef<EffectBlock>
{
    internal EffectRef(EffectBlock? inline, string? name) { Inline = inline; Name = name; }
    /// <summary>The effect itself, when the reference was made from a block or a handle. Null when it is a name only.</summary>
    public EffectBlock? Inline { get; }
    /// <summary>The effect's name: <c>mod:name</c> when made from a handle. Null when it was written in place.</summary>
    public string? Name { get; }
    /// <summary>An effect written in place.</summary>
    public static implicit operator EffectRef(EffectBlock block) => new(block, null);
    /// <summary>The effect that <see cref="Blocks.Define{T}"/> returned: the reference keeps the block and its <c>mod:name</c>.</summary>
    public static implicit operator EffectRef(BlockHandle<EffectBlock> handle) => new(handle.Block, handle.ToString());
    /// <summary>An effect by name: a block of this mod, or <c>"other-mod:name"</c> for another mod's.</summary>
    public static implicit operator EffectRef(string name) => new(null, name);
    static IBlockRef<EffectBlock> IBlockRef<EffectBlock>.Make(EffectBlock? inline, string? name) => new EffectRef(inline, name);
}

/// <summary>A status: inline, from <see cref="Blocks.Define{T}"/>, or by name.</summary>
[JsonConverter(typeof(RefConverter<StatusRef, StatusBlock>))]
public sealed class StatusRef : IBlockRef<StatusBlock>
{
    internal StatusRef(StatusBlock? inline, string? name) { Inline = inline; Name = name; }
    /// <summary>The status itself, when the reference was made from a block or a handle. Null when it is a name only.</summary>
    public StatusBlock? Inline { get; }
    /// <summary>The status's name: <c>mod:name</c> when made from a handle. Null when it was written in place.</summary>
    public string? Name { get; }
    /// <summary>A status written in place.</summary>
    public static implicit operator StatusRef(StatusBlock block) => new(block, null);
    /// <summary>The status that <see cref="Blocks.Define{T}"/> returned: the reference keeps the block and its <c>mod:name</c>.</summary>
    public static implicit operator StatusRef(BlockHandle<StatusBlock> handle) => new(handle.Block, handle.ToString());
    /// <summary>A status by name: a block of this mod, or <c>"other-mod:name"</c> for another mod's. A <c>GameBuffs</c> constant is also a name.</summary>
    public static implicit operator StatusRef(string name) => new(null, name);
    static IBlockRef<StatusBlock> IBlockRef<StatusBlock>.Make(StatusBlock? inline, string? name) => new StatusRef(inline, name);
}

/// <summary>An item a recipe needs: a <c>GameItems</c> constant, or an item this mod defines (a handle or its block name).</summary>
/// <param name="Value">The item as written: a <c>GameItems</c> constant, an item of this mod by its block name, or (made from a handle) the item's debug name in the game.</param>
[JsonConverter(typeof(ItemRefConverter))]
public readonly record struct ItemRef(string Value)
{
    /// <summary>An item by name: a <c>GameItems</c> constant, or the block name of an item this mod defines.</summary>
    public static implicit operator ItemRef(string value) => new(value);
    /// <summary>The item that <see cref="Blocks.Define{T}"/> returned, as the game's debug name (<see cref="BlockHandle.GameName"/>).</summary>
    public static implicit operator ItemRef(BlockHandle handle) => new(handle.GameName);
    /// <summary>The item as written, the same as <see cref="Value"/>.</summary>
    public override string ToString() => Value;
}

/// <summary>A reference to a block of type <typeparamref name="T"/>: the block written in place, a handle, or a name (in JSON, a string).</summary>
public interface IBlockRef<T> where T : class
{
    /// <summary>The block itself, when the reference holds one. Null when it is a name only.</summary>
    T? Inline { get; }
    /// <summary>The block's name, when the reference has one. Null when it was written in place.</summary>
    string? Name { get; }
    /// <summary>Makes a reference from what was read: an inline block, or a name.</summary>
    static abstract IBlockRef<T> Make(T? inline, string? name);
}

internal sealed class RefConverter<TRef, TBlock> : JsonConverter<TRef> where TRef : class, IBlockRef<TBlock> where TBlock : class
{
    public override TRef? Read(ref Utf8JsonReader r, Type t, JsonSerializerOptions o)
    {
        if (r.TokenType == JsonTokenType.String) return (TRef)TRef.Make(null, r.GetString());
        return (TRef)TRef.Make(JsonSerializer.Deserialize<TBlock>(ref r, o), null);
    }

    public override void Write(Utf8JsonWriter w, TRef v, JsonSerializerOptions o)
    {
        if (v.Name != null) w.WriteStringValue(v.Name);
        else JsonSerializer.Serialize(w, v.Inline, o);
    }
}

// "applies": "chilled", or { "status": "chilled", "seconds": 4 }
internal sealed class StatusApplyConverter : JsonConverter<StatusApply>
{
    public override StatusApply? Read(ref Utf8JsonReader r, Type t, JsonSerializerOptions o)
    {
        if (r.TokenType == JsonTokenType.String) return new StatusApply { Status = r.GetString()! };
        using var doc = JsonDocument.ParseValue(ref r);
        var root = doc.RootElement;
        StatusRef? status = null;
        double? seconds = null;
        foreach (var p in root.EnumerateObject())
        {
            if (p.Name == "status") status = p.Value.Deserialize<StatusRef>(o);
            else if (p.Name == "seconds") seconds = p.Value.GetDouble();
            else throw new JsonException($"\"{p.Name}\" is not part of applies; it takes status and seconds");
        }
        return new StatusApply { Status = status ?? throw new JsonException("applies needs a status"), Seconds = seconds };
    }

    public override void Write(Utf8JsonWriter w, StatusApply v, JsonSerializerOptions o)
    {
        w.WriteStartObject();
        w.WritePropertyName("status");
        JsonSerializer.Serialize(w, v.Status, o);
        if (v.Seconds is { } s) w.WriteNumber("seconds", s);
        w.WriteEndObject();
    }
}

internal sealed class ItemRefConverter : JsonConverter<ItemRef>
{
    public override ItemRef Read(ref Utf8JsonReader r, Type t, JsonSerializerOptions o) => new(r.GetString() ?? "");
    public override void Write(Utf8JsonWriter w, ItemRef v, JsonSerializerOptions o) => w.WriteStringValue(v.Value);
    public override ItemRef ReadAsPropertyName(ref Utf8JsonReader r, Type t, JsonSerializerOptions o) => new(r.GetString() ?? "");
    public override void WriteAsPropertyName(Utf8JsonWriter w, ItemRef v, JsonSerializerOptions o) => w.WritePropertyName(v.Value);
}

internal sealed class PremadeConverter<T> : JsonConverter<T> where T : class, IPremade
{
    private static IReadOnlyList<T> All => (IReadOnlyList<T>)typeof(T).GetField("All", System.Reflection.BindingFlags.NonPublic | System.Reflection.BindingFlags.Static)!.GetValue(null)!;

    public override T? Read(ref Utf8JsonReader r, Type t, JsonSerializerOptions o)
    {
        var key = r.GetString();
        return All.FirstOrDefault(x => x.Key == key)
               ?? throw new JsonException($"\"{key}\" is not a {typeof(T).Name}; there are {string.Join(", ", All.Select(x => x.Key))}");
    }

    public override void Write(Utf8JsonWriter w, T v, JsonSerializerOptions o) => w.WriteStringValue(v.Key);
}

/// <summary>A reference a block kind's record makes to another block, by name: <c>name</c> (the consumer mod's) or
/// <c>mod:name</c>. <see cref="BlockCompileContext.Resolve{T}"/> gives what it stands for.</summary>
/// <param name="Value">The name: the consumer mod's block <c>name</c>, or <c>mod:name</c> for another mod's.</param>
public readonly record struct BlockRef<T>(string Value) where T : class, IBlock;

/// <summary>What a <see cref="BlockRef{T}"/> stands for: the identity of the block, copied. Not the block.</summary>
public sealed record ResolvedBlock<T> where T : class, IBlock
{
    /// <summary>The id of the mod that defined the block.</summary>
    public required string ModId { get; init; }
    /// <summary>The block's name in that mod.</summary>
    public required string Name { get; init; }
    /// <summary>The GUID of what the block became in the game.</summary>
    public required Guid Guid { get; init; }
    /// <summary>The game's id of it (FNV-1a of the GUID).</summary>
    public required uint Id { get; init; }
    /// <summary>The debug name the block has in the game: what <c>Content.Items.Get</c>, <c>Items.Give</c> and data packs take.</summary>
    public required string GameName { get; init; }
    /// <summary>The block's kind: <c>kiln:item</c> for a built-in, <c>provider-mod:kind</c> for an external one.</summary>
    public required string KindId { get; init; }
}
