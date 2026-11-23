// SPDX-License-Identifier: MIT
// One-time login secrets (docs/kiln/launcher/admin-protocol.md section 8, "Login"): the launcher, which holds the admin
// token and so counts as the owner, mints a single-use secret for a named consumer (the web admin mod) and opens the
// panel with it; the consumer, also on KAP with the admin token, redeems it once and gets "owner". KAP only, never the
// file bridge (its result files would keep the secret on disk). Secrets are never logged and only their SHA-256 is kept.
using System.Security.Cryptography;
using System.Text.Json.Nodes;
using System.Text.RegularExpressions;

namespace Kiln.Admin;

/// <summary>The live secrets. Tick thread only (ops run there).</summary>
internal sealed partial class LoginTokens(Func<long> nowMs)
{
    public const int SecretBytes = 32, TtlMs = 60_000, MaxPerConsumer = 8;

    internal sealed record Minted(string Consumer, byte[] Hash, long Expires, string? Label, string MintedBy, DateTimeOffset MintedAt);

    private readonly List<Minted> _live = [];

    [GeneratedRegex("^[a-z0-9][a-z0-9._-]{0,63}$")]
    private static partial Regex ConsumerName();

    public static bool ValidConsumer(string s) => ConsumerName().IsMatch(s);

    /// <summary>Secrets that can still be redeemed.</summary>
    public int Live
    {
        get
        {
            Purge();
            return _live.Count;
        }
    }

    /// <summary>A new secret (64 hex characters) for <paramref name="consumer"/>, valid <see cref="TtlMs"/> ms and once.
    /// The consumer's oldest is dropped beyond <see cref="MaxPerConsumer"/>.</summary>
    public (string Secret, Minted Entry) Mint(string consumer, string? label, string mintedBy)
    {
        if (!ValidConsumer(consumer)) throw new AdminError("consumer: 1 to 64 of a-z 0-9 . _ - (starting with a letter or digit)");
        Purge();
        var mine = _live.Where(m => m.Consumer == consumer).ToList();
        for (var i = 0; i <= mine.Count - MaxPerConsumer; i++) _live.Remove(mine[i]);
        var secret = RandomNumberGenerator.GetBytes(SecretBytes);
        var entry = new Minted(consumer, SHA256.HashData(secret), nowMs() + TtlMs, label, mintedBy, DateTimeOffset.UtcNow);
        _live.Add(entry);
        var hex = Convert.ToHexStringLower(secret);
        CryptographicOperations.ZeroMemory(secret);
        return (hex, entry);
    }

    /// <summary>The entry the secret was minted as, removed so it never works again; null when it is wrong, used,
    /// expired or another consumer's. Every live secret of the consumer is compared, in constant time each.</summary>
    public Minted? Redeem(string consumer, string secretHex)
    {
        Purge();
        var given = new byte[SecretBytes];
        var wellFormed = secretHex.Length == SecretBytes * 2 && secretHex.All(char.IsAsciiHexDigit);
        if (wellFormed) given = Convert.FromHexString(secretHex);
        var hash = SHA256.HashData(given);
        CryptographicOperations.ZeroMemory(given);
        Minted? hit = null;
        foreach (var m in _live)
            if (CryptographicOperations.FixedTimeEquals(m.Hash, hash) && m.Consumer == consumer && wellFormed)
                hit = m;
        if (hit is not null) _live.Remove(hit);
        return hit;
    }

    private void Purge()
    {
        var now = nowMs();
        _live.RemoveAll(m => m.Expires <= now);
    }
}

internal static partial class Ops
{
    internal static void RegisterLogin(Bridge bridge, Logger log, LoginTokens tokens)
    {
        // {consumer, label?} -> {secret, consumer, expiresAt, ttlS}
        bridge.Register("login_mint", req =>
        {
            var by = KapOnly("login_mint");
            var consumer = Consumer(req);
            var label = OptionalText(req, "label", 120);
            var (secret, entry) = tokens.Mint(consumer, label, by);
            log.Info($"login secret minted for {consumer} by {by} (single use, valid {LoginTokens.TtlMs / 1000} s)");
            return new JsonObject
            {
                ["secret"] = secret, ["consumer"] = consumer,
                ["expiresAt"] = entry.MintedAt.AddMilliseconds(LoginTokens.TtlMs).ToString("yyyy-MM-dd'T'HH:mm:ss'Z'"),
                ["ttlS"] = LoginTokens.TtlMs / 1000,
            };
        });
        // {consumer, secret} -> {role: "owner", consumer, label, mintedBy, mintedAt}
        bridge.Register("login_redeem", req =>
        {
            var by = KapOnly("login_redeem");
            var consumer = Consumer(req);
            var secret = req["secret"] is JsonValue v && v.TryGetValue<string>(out var s) ? s : "";
            if (tokens.Redeem(consumer, secret) is not { } m)
            {
                log.Warn($"login secret for {consumer} refused (asked by {by}): wrong, used or expired");
                throw new AdminError("no such login: the secret is wrong, used or expired");
            }
            log.Info($"login secret for {consumer} redeemed by {by}: owner");
            return new JsonObject
            {
                ["role"] = "owner", ["consumer"] = m.Consumer, ["label"] = m.Label, ["mintedBy"] = m.MintedBy,
                ["mintedAt"] = m.MintedAt.ToString("yyyy-MM-dd'T'HH:mm:ss'Z'"),
            };
        });
    }

    /// <summary>The KAP client's name; refused over the file bridge.</summary>
    private static string KapOnly(string op) =>
        Tcp?.CurrentCaller is null ? throw new AdminError($"{op} is only served over KAP") : Tcp.CurrentCallerName ?? "client";

    private static string Consumer(JsonObject req) =>
        req["consumer"] is JsonValue v && v.TryGetValue<string>(out var s) && LoginTokens.ValidConsumer(s)
            ? s : throw new AdminError("consumer: 1 to 64 of a-z 0-9 . _ - (starting with a letter or digit)");
}
