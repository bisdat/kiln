// SPDX-License-Identifier: MIT
using Xunit;
using static Kiln.Analyzers.Tests.Harness;

namespace Kiln.Analyzers.Tests;

/// <summary>KILN199: the value type of Replication.Extension&lt;T&gt; (a stand-in of Kiln.Replication's member here).</summary>
public sealed class ReplicationExtensionTests
{
    private static readonly ReplicationExtensionAnalyzer A = new();

    private const string Api = """
        namespace Kiln
        {
            public static class Replication
            {
                public static object Extension<T>(string name, int version) where T : unmanaged => null!;
            }
        }
        """;

    [Fact]
    public async Task Value_types_outside_the_rule_are_reported()
    {
        var d = await Run(A, Api + """
            namespace M
            {
                using System.Runtime.InteropServices;
                using Kiln;
                public enum Mode : ushort { Off, On }
                public enum Wide : long { A }
                [StructLayout(LayoutKind.Sequential, Pack = 1)] public struct Ok { public uint A; public Mode M; public Inner I; public double D; public Wide W; }
                [StructLayout(LayoutKind.Sequential, Pack = 1)] public struct Inner { public short X; public float Y; }
                [StructLayout(LayoutKind.Sequential, Pack = 1)] public struct HasBool { public bool B; }
                [StructLayout(LayoutKind.Sequential, Pack = 1)] public struct HasChar { public char C; }
                [StructLayout(LayoutKind.Sequential, Pack = 1)] public struct HasNint { public nint P; }
                [StructLayout(LayoutKind.Sequential, Pack = 1)] public unsafe struct HasPtr { public int* P; }
                [StructLayout(LayoutKind.Sequential, Pack = 1)] public unsafe struct HasFixed { public fixed byte B[4]; }
                public struct NoLayout { public int A; }
                [StructLayout(LayoutKind.Sequential)] public struct DefaultPack { public int A; }
                [StructLayout(LayoutKind.Explicit, Pack = 1)] public struct Explicit { [FieldOffset(0)] public int A; }
                [StructLayout(LayoutKind.Sequential, Pack = 1)] public struct NestedBad { public NoLayout N; }
                [StructLayout(LayoutKind.Sequential, Pack = 1)] public struct Empty { }
                [StructLayout(LayoutKind.Sequential, Pack = 1)] public struct Q { public ulong A, B, C, D, E, F, G, H; }
                [StructLayout(LayoutKind.Sequential, Pack = 1)] public struct K { public Q A, B, C, D, E, F, G, H, I, J, L, M, N, O, P, R; }
                [StructLayout(LayoutKind.Sequential, Pack = 1)] public struct Big { public K A; public byte One; }
                [StructLayout(LayoutKind.Sequential, Pack = 1)] public struct Exactly { public K A; }
                public sealed class Use
                {
                    public void Go<TAny>() where TAny : unmanaged
                    {
                        Replication.Extension<Ok>("a:ok", 1);
                        Replication.Extension<Exactly>("a:exactly", 1);
                        Replication.Extension<TAny>("a:generic", 1);
                        Replication.Extension<HasBool>("a:1", 1);
                        Replication.Extension<HasChar>("a:2", 1);
                        Replication.Extension<HasNint>("a:3", 1);
                        Replication.Extension<HasPtr>("a:4", 1);
                        Replication.Extension<HasFixed>("a:5", 1);
                        Replication.Extension<NoLayout>("a:6", 1);
                        Replication.Extension<DefaultPack>("a:7", 1);
                        Replication.Extension<Explicit>("a:8", 1);
                        Replication.Extension<NestedBad>("a:9", 1);
                        Replication.Extension<Empty>("a:10", 1);
                        Replication.Extension<Big>("a:11", 1);
                        Replication.Extension<int>("a:12", 1);
                    }
                }
            }
            """);
        Ids(d, "KILN199", "KILN199", "KILN199", "KILN199", "KILN199", "KILN199", "KILN199", "KILN199", "KILN199", "KILN199", "KILN199", "KILN199");
        string[] why =
        [
            "field B is bool", "field C is char", "field P is nint", "field P is a pointer", "field B is a fixed buffer",
            "NoLayout needs [StructLayout(LayoutKind.Sequential, Pack = 1)]", "DefaultPack needs", "Explicit needs",
            "NoLayout (field N) needs", "it has no fields", "it is 1025 bytes; an extension value is at most 1024", "it is not a struct",
        ];
        for (var i = 0; i < why.Length; i++) Assert.Contains(why[i], d[i].GetMessage());
        Assert.Contains("'M.HasBool' cannot be the value of a replicated extension", d[0].GetMessage());
    }

    [Fact]
    public async Task Other_Extension_methods_are_not_its_business()
    {
        var d = await Run(A, """
            public static class Mine { public static void Extension<T>(string n, int v) { } }
            public sealed class Use { public void Go() => Mine.Extension<bool>("x", 1); }
            """);
        Ids(d);
    }
}
