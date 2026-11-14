// SPDX-License-Identifier: MIT
using Kiln;

namespace TkBootSdk;

/// <summary>Stages one status in OnBoot, its length from the mod's settings; the project opts into the boot phase with &lt;KilnBoot&gt;
/// only, and its build makes KILN035 an error (Config.Register and Value are allowed in OnBoot).</summary>
public sealed class TkBootSdkMod : Mod
{
    public override void OnBoot(BootContext boot)
    {
        var settings = Config.Register<TkBootSdkSettings>().Value;
        var status = BootDefinitions.For(boot).Define("tk-sdk-cold", new StatusBlock { Name = "Kit SDK Cold", Seconds = settings.Seconds });
        if (!status.Ok) throw new InvalidOperationException(status.Reason);
    }
}

public sealed class TkBootSdkSettings
{
    public int Seconds { get; set; } = 2;
}
