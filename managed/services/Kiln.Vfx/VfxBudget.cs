// SPDX-License-Identifier: MIT
// The VFX resource budget and result-bearing VFX definitions (package 17a S2; chapter 17.1 §5, I1, I5): the native calls
// behind Vfx.ResourceBudget and Kiln.Blocks' Blocks.Define(name, VfxBlock, options) / Blocks.GetVfxRegistration, and their
// change events: CapacityChangedEvent with Quota "vfx.budget", DefinitionRegistrationChangedEvent for a VFX defined with an
// answer. Both are posted at the next step, and only when something listens. Native: kiln_vfx.h resource_budget,
// define_vfx_result, vfx_registration (native/services/vfx/ledger.cpp).
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using System.Text.Json.Nodes;

namespace Kiln;

/// <summary>define_vfx_result and vfx_registration for Kiln.Blocks. Every native answer is checked: a bad argument, a member
/// that is off and a transport failure each say so, and a refusal stays a refusal.</summary>
internal static unsafe class VfxDefinitions
{
    /// <summary>What a native call answered: its result code, the fixed reply and the report JSON.</summary>
    internal readonly record struct Answer(int Rc, KilnDefinitionReply Reply, string Report)
    {
        /// <summary>The report's reason, or null.</summary>
        public string? Reason
        {
            get
            {
                try
                {
                    using var doc = JsonDocument.Parse(Report);
                    return doc.RootElement.ValueKind == JsonValueKind.Object && doc.RootElement.TryGetProperty("reason", out var r) &&
                           r.ValueKind == JsonValueKind.String ? r.GetString() : null;
                }
                catch (JsonException)
                {
                    return null;
                }
            }
        }
    }

    /// <summary>Kiln.Blocks can define VFX with an answer here: the member is in this kiln.vfx's table (or Kiln.Sdk's bake records
    /// definitions, where there is no kiln.vfx).</summary>
    public static bool CanDefine => VfxNative.Has(VfxNative.Member.DefineVfxResult) != null || (VfxNative.T == null && VfxNative.Baking);

    /// <summary>The exception for a member whose table is missing or older.</summary>
    public static KilnUnavailableException Off(string member) => VfxCaps.Off(VfxCaps.RegistrationRow, VfxNative.T == null
        ? $"{member}: kiln.vfx is not here (a dedicated server has no VFX; the two-argument Blocks.Define works on both sides)"
        : $"{member}: this kiln.vfx has no result-bearing VFX definitions (update Kiln)");

    /// <summary>define_vfx_result. <paramref name="document"/>: {"format": 1, "mod", "dir", "block"}.</summary>
    public static Answer Define(ulong owner, string name, string document, uint timing)
    {
        var t = VfxNative.Has(VfxNative.Member.DefineVfxResult);
        if (t == null)
        {
            if (VfxNative.T == null && VfxNative.Baking) return Baked(name, document);
            throw Off("Blocks.Define(name, VfxBlock, options)");
        }
        var n = Encoding.UTF8.GetBytes(name);
        var j = Encoding.UTF8.GetBytes(document);
        var reply = KilnDefinitionReply.New();
        var buf = stackalloc byte[VfxNative.AnswerReport + 1];
        nuint len = 0;
        int rc;
        fixed (byte* pn = n, pj = j)
            rc = t->DefineVfxResult(owner, new GameDataMirror.Str { Ptr = pn, Len = (nuint)n.Length },
                new GameDataMirror.Str { Ptr = pj, Len = (nuint)j.Length }, timing, &reply, buf, (nuint)(VfxNative.AnswerReport + 1), &len);
        return new Answer(rc, reply, Report(buf, len));
    }

    /// <summary>vfx_registration.</summary>
    public static Answer Receipt(Guid registrationId)
    {
        var t = VfxNative.Has(VfxNative.Member.VfxRegistration);
        if (t == null) throw Off("Blocks.GetVfxRegistration");
        var id = DefinitionRegistration.IdBytes(registrationId);
        var reply = KilnDefinitionReply.New();
        var buf = stackalloc byte[VfxNative.AnswerReport + 1];
        nuint len = 0;
        int rc;
        fixed (byte* p = id) rc = t->VfxRegistration(0, p, &reply, buf, (nuint)(VfxNative.AnswerReport + 1), &len);
        return new Answer(rc, reply, Report(buf, len));
    }

    // The report: the buffer is the size the native side asks for before it does anything.
    private static string Report(byte* buf, nuint len) => Encoding.UTF8.GetString(buf, (int)Math.Min(len, (nuint)VfxNative.AnswerReport));

    // Kiln.Sdk's bake runs the mod with no kiln.vfx: the block is recorded as define_json records one (VfxBake.cs) and is in the mod's
    // baked file, so it is there at the first start. The answer is "accepted for the next start"; its id is derived here, not by the
    // native side (no game process needs it).
    private static Answer Baked(string name, string document)
    {
        // a data file's block comes as the bytes the boot read: comments and trailing commas as its file has them
        var doc = JsonNode.Parse(document, documentOptions: new JsonDocumentOptions { CommentHandling = JsonCommentHandling.Skip, AllowTrailingCommas = true })
                      as JsonObject ?? throw new ArgumentException("not a definition document", nameof(document));
        var mod = (string?)doc["mod"] ?? "";
        var block = doc["block"]?.DeepClone() ?? new JsonObject();
        VfxNative.DefineJson(new JsonObject { ["format"] = 1, ["mod"] = mod, ["dir"] = (string?)doc["dir"] ?? "", ["vfx"] = new JsonObject { [name] = block } }.ToJsonString(), 0);
        var guid = BakeGuid(mod, (string?)block["was"] ?? name);
        var id = DefinitionRegistration.DeriveIdBytes(mod, DefinitionRegistrationKind.Vfx, guid, SHA256.HashData(Encoding.UTF8.GetBytes(block.ToJsonString())));
        var r = KilnDefinitionReply.New();
        r.Ok = 1;
        r.Disposition = (uint)DefinitionRegistrationState.AcceptedNextStart;
        r.Kind = (uint)DefinitionRegistrationKind.Vfx;
        id.CopyTo(new Span<byte>(r.RegistrationId, 16));
        guid.ToByteArray().CopyTo(new Span<byte>(r.DefinitionGuid, 16));
        return new Answer(0, r, "{\"reason\":null,\"warnings\":[\"recorded by Kiln.Sdk's bake: the mod's baked file has it from the first start\"]}");
    }

    private static Guid BakeGuid(string mod, string name) => VfxId.Named(mod, name).Guid;
}

/// <summary>The budget's and the definitions' change events.</summary>
internal static unsafe class VfxBudgetWatch
{
    private static readonly object Gate = new();
    private static CapacityInfo? _last;   // the budget as last seen: a read, a define, or the watch
    private static ulong _receiptsAt;     // the budget revision the known receipts were last read at
    private static readonly Dictionary<Guid, (string Mod, DefinitionRegistrationState State, uint Slots)> Known = new();

    /// <summary>A budget a mod read: it becomes the one the next change is measured from.</summary>
    public static CapacityQueryResult Seen(CapacityQueryResult r)
    {
        Compare(r.Budget);
        return r;
    }

    /// <summary>After a define: its registration's change, then the budget's.</summary>
    public static void Defined(string modId, DefinitionRegistrationResult r)
    {
        (string Mod, DefinitionRegistrationState State, uint Slots)? before;
        lock (Gate)
        {
            before = Known.TryGetValue(r.RegistrationId, out var b) ? b : null;
            Known[r.RegistrationId] = (modId, r.State, r.SlotsConsumed);
        }
        if (Events.HasSubscribers<DefinitionRegistrationChangedEvent>() &&
            DefinitionRegistration.Changed(modId, r, before?.State, before?.Slots ?? 0) is { } ev)
            Events.Enqueue(ev);
        if (Read() is { } now) Compare(now);
    }

    /// <summary>The service tick: every 25 steps, while something listens. A receipt the update thread settled (the game's table
    /// was read: registered, or refused) posts its change once.</summary>
    public static void Tick(ulong step)
    {
        if (step % 25 != 6) return;
        var budget = Events.HasSubscribers<CapacityChangedEvent>();
        var receipts = Events.HasSubscribers<DefinitionRegistrationChangedEvent>();
        if (!budget && !receipts) return;
        if (Read() is not { } now) return;
        if (budget) Compare(now);
        if (!receipts || VfxNative.Has(VfxNative.Member.VfxRegistration) == null) return;
        KeyValuePair<Guid, (string Mod, DefinitionRegistrationState State, uint Slots)>[] known;
        lock (Gate)
        {
            if (now.Revision == _receiptsAt) return;
            _receiptsAt = now.Revision;
            known = [.. Known];
        }
        foreach (var (id, was) in known)
        {
            var a = VfxDefinitions.Receipt(id);
            if (a.Rc != VfxNative.Ok) continue;
            var r = DefinitionRegistration.ToResult(a.Reply, a.Report);
            lock (Gate) Known[id] = (was.Mod, r.State, r.SlotsConsumed);
            if (DefinitionRegistration.Changed(was.Mod, r, was.State, was.Slots) is { } ev) Events.Enqueue(ev);
        }
    }

    private static CapacityInfo? Read()
    {
        var t = VfxNative.Has(VfxNative.Member.ResourceBudget);
        if (t == null) return null;
        var n = KilnCapacityInfo.New();
        return t->ResourceBudget(&n) == VfxNative.Ok ? CapacityMapping.ToResult(n).Budget : null;
    }

    private static void Compare(CapacityInfo now)
    {
        CapacityInfo? prev;
        lock (Gate)
        {
            prev = _last;
            _last = now;
        }
        if (prev != null && Events.HasSubscribers<CapacityChangedEvent>() && CapacityMapping.Changed(VfxCaps.BudgetRow, prev, now) is { } ev)
            Events.Enqueue(ev);
    }
}
