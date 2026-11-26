// SPDX-License-Identifier: MIT
#pragma warning disable KILNX031
using Kiln;
using Kiln.Host;
using System.Reflection;
using System.Reflection.Metadata;
using System.Reflection.Metadata.Ecma335;
using System.Reflection.PortableExecutable;
using Xunit;

namespace Kiln.Host.Tests;

public sealed class RegistryDeclarationTests
{
    private static int _constructed;

    [Fact]
    public void MetadataDeclaresPointsWithoutConstructingTheMod()
    {
        _constructed = 0;
        Assert.Equal(RegistryEntryPoint.WorldLoad | RegistryEntryPoint.HotReload, RegistryDeclarations.PointsOf(typeof(Declared)));
        Assert.Equal(0, _constructed);
        Assert.Equal(RegistryEntryPoint.GameStart, RegistryDeclarations.PointsOf(typeof(Default)));
        Assert.Equal((RegistryEntryPoint)0, RegistryDeclarations.PointsOf(typeof(NoOverride)));
    }

    [Fact]
    public void InvalidDeclarationsFailClosed()
    {
        Assert.Throws<InvalidDataException>(() => RegistryDeclarations.PointsOf(typeof(Unknown)));
        Assert.Throws<InvalidDataException>(() => RegistryDeclarations.PointsOf(typeof(WithoutOverride)));
    }

    [Fact]
    public void GameStartExportLookupHasATypedRefusalAndPrivateHelpersFallThrough()
    {
        var exports = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase) { ["SharedRuntime.dll"] = "dependency" };
        var error = Assert.Throws<RegistryContextException>(() => RegistryDeclarations.GameStartExport("author", exports, new AssemblyName("SharedRuntime")));
        Assert.Equal(RegistryContextError.RuntimeExportUnavailable, error.Code);
        Assert.Contains("ModLoad or later", error.Message);
        Assert.Null(RegistryDeclarations.GameStartExport("author", exports, new AssemblyName("OwnHelper")));
    }

    [Theory]
    [InlineData("")]
    [InlineData("invalid\0.dll")]
    public void MetadataPrefilterRefusesMalformedPathsWithoutThrowing(string file)
        => Assert.False(RegistryDeclarations.MightOverride(file, []));

    [Fact]
    public void MetadataPrefilterFollowsExternalGenericBases()
    {
        var directory = Path.Combine(Path.GetTempPath(), "registry-metadata-" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(directory);
        try
        {
            Image(directory, "Base", false);
            var child = Image(directory, "Child", true);
            Assert.True(RegistryDeclarations.MightOverride(child, new[] { directory }));
        }
        finally { Directory.Delete(directory, recursive: true); }
    }
    // review4 nit: the code identity hashes the mod's DLLs only when a context's Identity is first read (never when the mod
    // loads on the tick thread), and a DLL whose path, size and write time are unchanged is not read again.
    [Fact]
    public void TheCodeIdentityIsHashedAtFirstUseAndCached()
    {
        var directory = Path.Combine(Path.GetTempPath(), "registry-code-" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(directory);
        try
        {
            var dll = Path.Combine(directory, "Mod.dll");
            File.WriteAllBytes(dll, [1, 2, 3]);
            File.SetLastWriteTimeUtc(dll, new DateTime(2026, 11, 25, 0, 0, 0, DateTimeKind.Utc));
            var code = RegistryDispatch.Code(directory);
            Assert.Equal(0, RegistryDispatch.FileHashReads(dll));
            var first = code();
            Assert.Equal(1, RegistryDispatch.FileHashReads(dll));
            Assert.Equal(first, code());
            Assert.Equal(first, RegistryDispatch.Code(directory)());  // a reload of the same files
            Assert.Equal(1, RegistryDispatch.FileHashReads(dll));
            File.WriteAllBytes(dll, [1, 2, 3, 4]);
            File.SetLastWriteTimeUtc(dll, new DateTime(2026, 11, 25, 1, 0, 0, DateTimeKind.Utc));
            Assert.NotEqual(first, RegistryDispatch.Code(directory)());
            Assert.Equal(2, RegistryDispatch.FileHashReads(dll));
        }
        finally { Directory.Delete(directory, recursive: true); }
    }

    private static string Image(string directory, string name, bool genericBase)
    {
        var metadata = new MetadataBuilder();
        metadata.AddModule(0, metadata.GetOrAddString(name + ".dll"), metadata.GetOrAddGuid(Guid.NewGuid()), default, default);
        metadata.AddAssembly(metadata.GetOrAddString(name), new Version(1, 0), default, default, 0, AssemblyHashAlgorithm.None);
        var reference = metadata.AddAssemblyReference(metadata.GetOrAddString(genericBase ? "Base" : "Kiln.Api"), new Version(1, 0), default, default, 0, default);
        var parent = metadata.AddTypeReference(reference, metadata.GetOrAddString(genericBase ? "Fixture" : "Kiln"), metadata.GetOrAddString(genericBase ? "Author`1" : "RegistryContext"));
        EntityHandle baseType = default;
        if (genericBase)
        {
            var signature = new BlobBuilder();
            new BlobEncoder(signature).TypeSpecificationSignature().GenericInstantiation(parent, 1, false).AddArgument().Int32();
            baseType = metadata.AddTypeSpecification(metadata.GetOrAddBlob(signature));
        }
        metadata.AddTypeDefinition(0, default, metadata.GetOrAddString("<Module>"), default, default, default);
        metadata.AddTypeDefinition(TypeAttributes.Public, metadata.GetOrAddString("Fixture"), metadata.GetOrAddString("Derived"), baseType, default, default);
        var image = new BlobBuilder();
        new ManagedPEBuilder(new PEHeaderBuilder(), new MetadataRootBuilder(metadata), new BlobBuilder()).Serialize(image);
        var file = Path.Combine(directory, name + ".dll"); File.WriteAllBytes(file, image.ToArray()); return file;
    }

    [RegistryEntryPoints(RegistryEntryPoint.WorldLoad | RegistryEntryPoint.HotReload)]
    private sealed class Declared : Mod
    {
        public Declared() => ++_constructed;
        public override void OnRegistry(RegistryContext context) { }
    }
    private sealed class Default : Mod { public override void OnRegistry(RegistryContext context) { } }
    private sealed class NoOverride : Mod;
    [RegistryEntryPoints((RegistryEntryPoint)32)]
    private sealed class Unknown : Mod { public override void OnRegistry(RegistryContext context) { } }
    [RegistryEntryPoints(RegistryEntryPoint.WorldLoad)]
    private sealed class WithoutOverride : Mod;
}
