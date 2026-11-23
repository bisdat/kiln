// SPDX-License-Identifier: MIT
using System.Text.Json.Nodes;
using Xunit;

namespace Kiln.Admin.Tests;

public sealed class AdminLifecycleTests : AdminTest
{
    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public void Load_and_unload_keep_replication_restart_login_and_staging_together(bool tcp)
    {
        var kiln = Path.Combine(Dir, "kiln");
        var data = Path.Combine(kiln, "data", "kiln-admin");
        Directory.CreateDirectory(data);
        File.WriteAllText(Path.Combine(data, "config.json"), new JsonObject
        {
            ["bridge"] = Path.Combine(Dir, "bridge"),
            ["tcp"] = new JsonObject { ["enabled"] = tcp, ["port"] = 0 },
        }.ToJsonString());
        var registry = Path.Combine(kiln, "data", "registry", "entry-compat.json");
        Directory.CreateDirectory(Path.GetDirectoryName(registry)!);
        File.WriteAllText(registry, "{\"format\":1}");
        byte[]? token = null;
        CancellationTokenSource? previous = null;
        // A hot reload gets a new instance and cancellation source, without losing persistent data.
        for (var load = 0; load < 2; load++)
        {
            File.WriteAllText(Path.Combine(data, Ops.RestartMarker), "{}");
            var context = HostAccess.CreateContext(new ModInfo("kiln-admin", "Admin", "test", Path.Combine(kiln, "mods", "kiln-admin")), 1, data);
            var mod = new AdminMod();
            HostAccess.Bind(mod, context);
            var current = HostAccess.Current;
            HostAccess.Current = context;
            try
            {
                mod.OnLoad();
                Assert.False(File.Exists(Path.Combine(data, Ops.RestartMarker)));
                Assert.NotNull(Ops.Restart);
                Assert.NotSame(previous, Ops.UpdatesCancel);
                Assert.False(Ops.UpdatesCancel.IsCancellationRequested);
                foreach (var op in new[] { "repl_stats", "server_restart", "server_restart_cancel", "login_mint", "update_stage", "update_rollback" })
                    Assert.True(Backend.Logged(op), $"{op} missing after load: {Backend.All()}");
                Assert.Equal(tcp, Ops.Tcp is not null);
                if (tcp)
                {
                    Assert.True(File.Exists(Ops.Tcp!.EndpointFile));
                    var fresh = File.ReadAllBytes(Path.Combine(data, AdminToken.FileName));
                    if (token is not null) Assert.Equal(token, fresh);
                    token = fresh;
                }
                Ops.Restart!.Schedule(60, "test countdown", "test"); // never reaches a real save or stop
                previous = Ops.UpdatesCancel;
            }
            finally
            {
                mod.OnUnload();
                context.Retire();
                HostAccess.Current = current;
            }
            Assert.True(previous!.IsCancellationRequested);
            Assert.Null(Ops.Tcp);
            Assert.False(File.Exists(Path.Combine(data, "endpoint.json")));
            Assert.Equal("{\"format\":1}", File.ReadAllText(registry));
            Assert.True(Backend.Logged("restart dropped"));
        }
    }
}
