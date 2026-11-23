// SPDX-License-Identifier: MIT
using System.Text.Json.Nodes;

namespace Kiln.Admin;

/// <summary>Who a dashboard's command line runs as (command_run, command_complete, commands_describe): the optional
/// <c>source</c> (a name such as <c>web:&lt;user&gt;</c>, shown in kiln.log and in command events) and <c>level</c>
/// (everyone, admin or console: the highest command level it may use). Without both, as before: the remote
/// "dashboard" with admin rights.</summary>
internal static partial class Ops
{
    internal const int MaxSourceName = 80;

    /// <summary>The <c>level</c> argument, or null.</summary>
    internal static CommandPermission? Level(JsonObject req)
    {
        if (req["level"] is null) return null;
        var s = req["level"] is JsonValue v && v.TryGetValue<string>(out var t) ? t : null;
        return s?.ToLowerInvariant() switch
        {
            "everyone" => CommandPermission.Everyone,
            "admin" => CommandPermission.Admin,
            "console" => CommandPermission.Console,
            _ => throw new AdminError("level: everyone, admin or console"),
        };
    }

    /// <summary>The <c>source</c> argument (needs <c>level</c>), or null.</summary>
    internal static string? SourceName(JsonObject req)
    {
        var name = OptionalText(req, "source", MaxSourceName);
        if (name is not null && req["level"] is null) throw new AdminError("source needs a level (everyone, admin or console)");
        return name;
    }

    /// <summary>The remote source a line runs as when no player is named.</summary>
    internal static CommandSource RemoteSource(JsonObject req)
    {
        var name = SourceName(req);
        return Level(req) is { } level ? CommandSource.RemoteCapped(name ?? "dashboard", level) : CommandSource.Remote("dashboard", admin: true);
    }

    /// <summary>command_run / command_complete: with <c>steamId</c> that player (their permissions, below the level when
    /// one is given), else <see cref="RemoteSource"/>.</summary>
    internal static CommandSource LineSource(JsonObject req)
    {
        if (req["steamId"] is null) return RemoteSource(req);
        SourceName(req);  // checked the same way
        var player = CommandSource.ForSteamId(SteamId(req));
        return Level(req) is { } level ? player.WithCap(level) : player;
    }
}
