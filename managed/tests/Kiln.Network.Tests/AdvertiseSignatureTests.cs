// SPDX-License-Identifier: MIT
// What joining players need follows a `reload` at once (0.2.0-dev.31.14): the required-mods recompute is keyed on each
// mod's id, version, state and folder, so a reload that changed only "clients" in kiln.mod.json (its version kept) went
// unseen for up to a minute, and a vanilla client could still join a server whose reloaded mod now needs Kiln on the client.
using Xunit;
namespace Kiln.Network.Tests;

public sealed class AdvertiseSignatureTests
{
    [Fact]
    public void A_rewritten_manifest_changes_the_signature_with_the_version_kept()
    {
        var dir = Path.Combine(Path.GetTempPath(), "kiln-advert-sig-" + Guid.NewGuid());
        Directory.CreateDirectory(dir);
        try
        {
            var manifest = Path.Combine(dir, "kiln.mod.json");
            File.WriteAllText(manifest, """{"id": "bigpack", "version": "1.0.0", "clients": "none"}""");
            File.SetLastWriteTimeUtc(manifest, new DateTime(2026, 11, 25, 12, 0, 0, DateTimeKind.Utc));
            (string, string, ModState, string)[] mods = [("bigpack", "1.0.0", ModState.Loaded, dir)];
            var before = NetAdvertise.Signature(mods);
            Assert.Equal(before, NetAdvertise.Signature(mods));  // nothing changed: no recompute
            File.WriteAllText(manifest, """{"id": "bigpack", "version": "1.0.0", "clients": "kiln"}""");  // a reload's new manifest
            File.SetLastWriteTimeUtc(manifest, new DateTime(2026, 11, 25, 12, 0, 5, DateTimeKind.Utc));
            Assert.NotEqual(before, NetAdvertise.Signature(mods));
            Assert.Equal(NetAdvertise.Signature([("x", "1", ModState.Loaded, "")]), NetAdvertise.Signature([("x", "1", ModState.Loaded, "")]));  // no folder
        }
        finally { Directory.Delete(dir, recursive: true); }
    }
}
