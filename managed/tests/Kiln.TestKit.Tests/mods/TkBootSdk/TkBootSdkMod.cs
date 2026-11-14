// SPDX-License-Identifier: MIT
using Kiln;

namespace TkBootSdk;

/// <summary>Stages one status in OnBoot; the project opts into the boot phase with &lt;KilnBoot&gt; only.</summary>
public sealed class TkBootSdkMod : Mod
{
    public override void OnBoot(BootContext boot)
    {
        var status = BootDefinitions.For(boot).Define("tk-sdk-cold", new StatusBlock { Name = "Kit SDK Cold", Seconds = 2 });
        if (!status.Ok) throw new InvalidOperationException(status.Reason);
    }
}
