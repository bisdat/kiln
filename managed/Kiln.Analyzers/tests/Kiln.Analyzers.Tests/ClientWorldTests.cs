// SPDX-License-Identifier: MIT
using Xunit;
using static Kiln.Analyzers.Tests.Harness;

namespace Kiln.Analyzers.Tests;

public sealed class ClientWorldTests
{
    private static readonly ClientWorldAnalyzer A = new();

    [Fact]
    public async Task A_client_world_bind_off_the_games_threads_is_reported()
    {
        var d = await Run(A, """
            public sealed class M : Mod
            {
                public override void OnLoad()
                {
                    Task.Run(() => EcsPrograms.TryBind("pin", c => { }, WorldKind.Client));
                    new Thread(() => EcsPrograms.TryBind("pred", c => { }, WorldKind.Prediction)).Start();
                }
            }
            """);
        Ids(d, "KILN109", "KILN109");
        Assert.Contains("Task.Run", d[0].GetMessage());
        Assert.Contains("Client.Post", d[0].GetMessage());
        Assert.Equal("TryBind", d[0].At());
    }

    [Fact]
    public async Task OnLoad_the_server_world_and_a_world_not_known_at_build_time_are_not()
    {
        var d = await Run(A, """
            public sealed class M : Mod
            {
                public override void OnLoad()
                {
                    EcsPrograms.TryBind("pin", c => { }, WorldKind.Client);
                    Task.Run(() => EcsPrograms.TryBind("srv", c => { }, WorldKind.Server));
                    var w = DateTime.Now.Second > 30 ? WorldKind.Client : WorldKind.Server;
                    Task.Run(() => EcsPrograms.TryBind("any", c => { }, w));
                }
            }
            """);
        Ids(d);
    }
}
