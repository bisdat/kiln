// SPDX-License-Identifier: MIT
// A content transaction (kiln.content txn_begin / txn_end, since 0.2.0-dev.28): every layer of one owner set aside, so that what the owner
// applies next can be taken back whole. Rollback puts the owner's layers back exactly as they were, writes them and reads every touched field
// back; when one does not read back, kiln.content takes every layer of the owner off (fail closed) and says so. Kiln-internal (Kiln.Blocks'
// routed weapons); nothing a mod calls. The change event of what was applied inside is posted at the commit, not while the transaction is open,
// so no handler runs (and writes as the owner) between the start and the end.
using System.Text.Json;

namespace Kiln;

/// <summary>How a content transaction ended.</summary>
internal enum ContentTransactionOutcome
{
    /// <summary>Kept: what was applied inside stays.</summary>
    Committed,
    /// <summary>Taken back: the owner's layers are what they were at the start, every touched field read back.</summary>
    Restored,
    /// <summary>The rollback did not read back: every layer of the owner was taken off.</summary>
    Revoked,
    /// <summary>The owner's layers were taken off while it was open (revert, the owner going, the world's reset): nothing was put back.</summary>
    Cancelled,
}

/// <summary>The end of a content transaction: its outcome, how many fields the rollback touched, and why it was revoked.</summary>
internal readonly record struct ContentTransactionResult(ContentTransactionOutcome Outcome, int Fields, string? Error);

internal sealed unsafe class ContentTransaction : IDisposable
{
    private readonly ulong _id;
    private readonly List<string> _applied = new();
    private bool _ended;

    public ulong Owner { get; }

    private ContentTransaction(ulong owner, ulong id)
    {
        Owner = owner;
        _id = id;
    }

    /// <summary>Opens one for <paramref name="owner"/>. Throws <see cref="KilnUnavailableException"/> when this kiln.content has no
    /// transactions, <see cref="InvalidOperationException"/> when the owner has one open already (or it is refused).</summary>
    public static ContentTransaction Begin(ulong owner)
    {
        var t = Content.Require("Content.BeginTransaction");
        if (!ContentNative.HasTransactions(t))
            throw ContentCaps.Off(ContentCaps.Service, "this kiln.content has no content transactions (txn_begin, since 0.2.0-dev.28)");
        ulong id = 0;
        var rc = t->TxnBegin(owner, &id);
        if (rc == ContentNative.InvalidArgument) throw new InvalidOperationException("Content.BeginTransaction: the owner has a transaction open already");
        if (rc != ContentNative.Ok) throw Content.Failure("Content.BeginTransaction", rc);
        return new ContentTransaction(owner, id);
    }

    /// <summary>Applies a document of field rules as the owner's layers, inside the transaction.</summary>
    public ContentReport Apply(string json, string source)
    {
        if (_ended) throw new InvalidOperationException("the content transaction has ended");
        var report = Content.ApplyAs(Owner, json, source, notify: false);
        if (report.Applied > 0) _applied.Add(source);
        return report;
    }

    /// <summary>Keeps what was applied, and posts its change event.</summary>
    public ContentTransactionResult Commit()
    {
        var r = End(rollback: false);
        if (r.Outcome == ContentTransactionOutcome.Committed)
            foreach (var source in _applied.Distinct()) Content.Changed(ContentChange.Applied, null, source);
        return r;
    }

    /// <summary>Takes back what was applied, reading every touched field back; revoked (every layer of the owner off) when that fails.</summary>
    public ContentTransactionResult Rollback()
    {
        var r = End(rollback: true);
        if (r.Outcome == ContentTransactionOutcome.Revoked) Content.Changed(ContentChange.Reverted, null, "");
        return r;
    }

    /// <summary>An open transaction is rolled back.</summary>
    public void Dispose()
    {
        if (!_ended) Rollback();
    }

    private ContentTransactionResult End(bool rollback)
    {
        if (_ended) throw new InvalidOperationException("the content transaction has ended");
        _ended = true;
        var t = Content.Require(rollback ? "Content.RollbackTransaction" : "Content.CommitTransaction");
        const int cap = 1024;
        var buf = stackalloc byte[cap];
        nuint len = 0;
        var rc = t->TxnEnd(_id, rollback ? 1u : 0u, buf, cap, &len);
        string? error = null;
        var fields = 0;
        try
        {
            using var doc = JsonDocument.Parse(ContentNative.Text(buf, cap));
            if (doc.RootElement.TryGetProperty("error", out var e)) error = e.GetString();
            if (doc.RootElement.TryGetProperty("fields", out var f)) fields = f.GetInt32();
        }
        catch (JsonException) { }
        var outcome = rc switch
        {
            ContentNative.Ok => rollback ? ContentTransactionOutcome.Restored : ContentTransactionOutcome.Committed,
            ContentNative.NotFound => ContentTransactionOutcome.Cancelled,
            ContentNative.Revoked => ContentTransactionOutcome.Revoked,
            _ => throw Content.Failure(rollback ? "Content.RollbackTransaction" : "Content.CommitTransaction", rc),
        };
        return new ContentTransactionResult(outcome, fields, error);
    }
}
