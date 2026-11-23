// SPDX-License-Identifier: MIT
using System.Text.Json.Nodes;

namespace Kiln.Admin;

/// <summary>Staged updates (docs/kiln/staged-updates.md, admin-protocol.md section 8): update_list, update_stage,
/// update_cancel, update_rollback. The work is Staging's; the next server start applies what is staged (the proxy for
/// Kiln, kiln-core for mods). Server only.</summary>
internal static partial class Ops
{
    /// <summary>The kiln folder (kiln/data/kiln-admin is Kiln.Admin's data directory on a server).</summary>
    private static string KilnFolder => Path.GetFullPath(Path.Combine(DataDirectory, "..", ".."));

    /// <summary>Canceled when Kiln.Admin unloads: a staging copy in progress stops and its .stage-* folder goes.</summary>
    internal static CancellationTokenSource UpdatesCancel { get; set; } = new();

    static partial void RegisterUpdates(Bridge bridge, Logger log)
    {
        bridge.Register("update_list", _ => Guard(() => Staging.List(KilnFolder)));
        // {kind: "mod" | "kiln", source (a folder under kiln/staged/incoming), id?, folder?, files? [{path, sha256}], label?,
        //  sha256? (a Kiln bundle's zip, for the receipt), by?}: copied and hashed off the tick thread
        bridge.RegisterAsync("update_stage", async req =>
        {
            var kind = req["kind"]?.GetValue<string>();
            var source = req["source"]?.GetValue<string>();
            var label = req["label"]?.GetValue<string>();
            var by = By(req);
            var expect = ExpectedFiles(req);
            var kiln = KilnFolder;
            var token = UpdatesCancel.Token;
            var result = kind switch
            {
                "mod" => await Task.Run(() => Guard(() => Staging.StageMod(kiln, source, req["id"]?.GetValue<string>(), req["folder"]?.GetValue<string>(),
                    expect, label, by, token)), token),
                "kiln" => await Task.Run(() => Guard(() => Staging.StageKiln(kiln, source, expect, label, req["sha256"]?.GetValue<string>(), by, token)), token),
                _ => throw new AdminError("kind must be \"mod\" or \"kiln\""),
            };
            log.Info($"update staged for the next start: {result["unit"]} {result["staged"]?["version"]} ({by}{(label is null ? "" : ", " + label)})");
            return result;
        });
        bridge.Register("update_cancel", req =>
        {
            var r = Guard(() => Staging.Cancel(KilnFolder, req["unit"]?.GetValue<string>()));
            log.Info($"staged change canceled: {r["unit"]} ({string.Join(", ", (r["canceled"] as JsonArray ?? []).Select(x => x?.ToString()))}; {By(req)})");
            return r;
        });
        bridge.Register("update_rollback", req =>
        {
            var by = By(req);
            var r = Guard(() => Staging.Rollback(KilnFolder, req["unit"]?.GetValue<string>(), by));
            log.Info($"rollback requested for the next start: {r["unit"]} to {r["to"]} ({by})");
            return r;
        });
    }

    private static string By(JsonObject req) =>
        req["by"] is JsonValue v && v.TryGetValue<string>(out var s) && s.Length is > 0 and <= 120 ? s : "kiln-admin";

    private static Dictionary<string, string>? ExpectedFiles(JsonObject req)
    {
        if (req["files"] is null) return null;
        if (req["files"] is not JsonArray a) throw new AdminError("files must be a list of {path, sha256}");
        var d = new Dictionary<string, string>(StringComparer.Ordinal);
        foreach (var f in a)
        {
            var path = f?["path"]?.GetValue<string>();
            var sha = f?["sha256"]?.GetValue<string>();
            if (string.IsNullOrEmpty(path) || sha is not { Length: 64 }) throw new AdminError("files: each entry needs path and a 64-digit sha256");
            if (!d.TryAdd(path, sha.ToLowerInvariant())) throw new AdminError($"files: {path} twice");
        }
        return d;
    }

    private static T Guard<T>(Func<T> op)
    {
        try
        {
            return op();
        }
        catch (StagingError e)
        {
            throw new AdminError(e.Message);
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException)
        {
            throw new AdminError("the staged files could not be written or read: " + e.Message);
        }
    }
}
