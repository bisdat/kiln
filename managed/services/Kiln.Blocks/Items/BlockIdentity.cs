// SPDX-License-Identifier: MIT
// Legacy identities (since 0.2.0-dev.30.5): an item or recipe block that keeps the GUID another loader gave it, so the game id
// (FNV-1a of the GUID) stays the same and what players made under that loader is still this item. A block without one keeps the
// derived GUID (BlockIds). Claims are checked here: two blocks of one mod, or two mods, never get the same id.
using System.Collections.Concurrent;
using System.Text.Json;
using System.Text.Json.Serialization;

namespace Kiln;

/// <summary>A fixed identity for an item or recipe block, in place of the one Kiln derives from the mod and the block's name:
/// <c>Identity = BlockIdentity.Legacy(new Guid("…"))</c>. For a mod that takes over items from another loader: the game id is FNV-1a of
/// the GUID, so stacks and recipes players made there are this block's. In <c>blocks/*.json</c>: <c>"identity": { "legacy": "&lt;guid&gt;" }</c>
/// or <c>{ "legacyId": 3205570361 }</c>. Never change it once released: saves keep the id. Since 0.2.0-dev.30.5 (mod API 0.2.14).</summary>
[JsonConverter(typeof(BlockIdentityConverter))]
public sealed record BlockIdentity
{
    private readonly bool _byId;

    private BlockIdentity(Guid guid, bool byId) => (Guid, _byId) = (guid, byId);

    /// <summary>The GUID the block is defined under.</summary>
    public Guid Guid { get; }

    /// <summary>The game's id of it: FNV-1a over the GUID's 16 bytes, as for every item and recipe.</summary>
    public uint Id => BlockIds.Fnv(Guid);

    /// <summary>The GUID another loader gave the item or recipe.</summary>
    /// <exception cref="ArgumentException"><paramref name="guid"/> is empty.</exception>
    public static BlockIdentity Legacy(Guid guid) =>
        guid == Guid.Empty ? throw new ArgumentException("an empty GUID is no identity", nameof(guid)) : new(guid, false);

    /// <summary>Only the game id another loader gave it, when its GUID is not known: the block gets a GUID whose FNV-1a is <paramref name="id"/>
    /// (the same one on every machine).</summary>
    /// <exception cref="ArgumentException"><paramref name="id"/> is 0.</exception>
    public static BlockIdentity Legacy(uint id) =>
        id == 0 ? throw new ArgumentException("id 0 is no item or recipe", nameof(id)) : new(BlockIds.WithFnv(id), true);

    internal bool ById => _byId;

    public bool Equals(BlockIdentity? other) => other is not null && other.Guid == Guid;
    public override int GetHashCode() => Guid.GetHashCode();
    public override string ToString() => _byId ? $"legacy id 0x{Id:x8}" : $"legacy {Guid:D} (0x{Id:x8})";
}

internal sealed class BlockIdentityConverter : JsonConverter<BlockIdentity>
{
    public override BlockIdentity Read(ref Utf8JsonReader r, Type t, JsonSerializerOptions o)
    {
        if (r.TokenType != JsonTokenType.StartObject) throw new JsonException("an identity is { \"legacy\": \"<guid>\" } or { \"legacyId\": <id> }");
        BlockIdentity? result = null;
        while (r.Read() && r.TokenType != JsonTokenType.EndObject)
        {
            var key = r.GetString();
            r.Read();
            if (result != null) throw new JsonException("an identity is one of legacy or legacyId, not both");
            try
            {
                result = key switch
                {
                    "legacy" when r.TokenType == JsonTokenType.String && Guid.TryParse(r.GetString(), out var g) => BlockIdentity.Legacy(g),
                    "legacyId" when r.TokenType == JsonTokenType.Number && r.TryGetUInt32(out var n) => BlockIdentity.Legacy(n),
                    "legacyId" when r.TokenType == JsonTokenType.String && r.GetString() is { } s && s.StartsWith("0x", StringComparison.OrdinalIgnoreCase)
                                    && uint.TryParse(s.AsSpan(2), System.Globalization.NumberStyles.HexNumber, null, out var h) => BlockIdentity.Legacy(h),
                    "legacy" => throw new JsonException("legacy is a GUID"),
                    "legacyId" => throw new JsonException("legacyId is a game id: a number, or \"0x…\""),
                    _ => throw new JsonException($"\"{key}\" is not a key of an identity; there are legacy, legacyId"),
                };
            }
            catch (ArgumentException e)
            {
                throw new JsonException(e.Message);
            }
        }
        return result ?? throw new JsonException("an identity says legacy or legacyId");
    }

    public override void Write(Utf8JsonWriter w, BlockIdentity v, JsonSerializerOptions o)
    {
        w.WriteStartObject();
        if (v.ById) w.WriteNumber("legacyId", v.Id);
        else w.WriteString("legacy", v.Guid.ToString("D"));
        w.WriteEndObject();
    }
}

/// <summary>Who holds which legacy id, once a block is committed: per mod by block, across mods by mod. Items and recipes are separate
/// registries in the game.</summary>
internal static class LegacyIdentities
{
    private static readonly ConcurrentDictionary<(string Kind, uint Id), string> Mods = new();

    /// <summary>The GUID a block of <paramref name="kind"/> ("item", "recipe") is defined under: its identity's, checked and claimed, or
    /// <paramref name="derived"/> without one.</summary>
    internal static Guid Claim(BlockContext ctx, string kind, BlockIdentity? identity, string path, Guid derived)
    {
        if (identity is null) return derived;
        var key = (kind, identity.Id);
        var mod = ctx.Mod.Id;
        if (Mods.TryGetValue(key, out var holder) && holder != mod)
            throw new BlockError(ctx.Block, path, $"{kind} id 0x{identity.Id:x8} ({identity.Guid:D}) is mod {holder}'s: two mods cannot define one {kind}");
        if (ctx.Mod.Identities.TryGetValue(key, out var mine) && mine != ctx.Block)
            throw new BlockError(ctx.Block, path, $"{kind} id 0x{identity.Id:x8} ({identity.Guid:D}) is already {mine}'s in this mod: two blocks cannot be one {kind}");
        // held once the block is in: a block that fails after this point holds nothing
        var (mods, block) = (ctx.Mod, ctx.Block);
        void Hold()
        {
            mods.Identities.TryAdd(key, block);
            Mods.TryAdd(key, mod);
        }
        if (ctx.Low is { } low) low.AfterCommit(Hold);
        else Hold();
        return identity.Guid;
    }
}
