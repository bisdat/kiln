// SPDX-License-Identifier: MIT
using Kiln;

namespace TkBoot;

/// <summary>The mod's setting OnBoot reads (tk-boot.json in kiln/config).</summary>
public sealed class TkBootSettings
{
    /// <summary>How long the staged status lasts.</summary>
    public int ColdSeconds { get; set; } = 3;

    /// <summary>The token's PNG icon (a file of the mod's folder), or none.</summary>
    public string? TokenIcon { get; set; }
}

/// <summary>Reads one record of the game's data and its setting in OnBoot and stages a status (its time from the setting) and an item
/// (its stack from the record, its icon from the setting); OnLoad defines nothing, registers the setting again and logs the committed event.</summary>
public sealed class TkBootMod : Mod
{
    public override void OnBoot(BootContext boot)
    {
        var settings = Config.Register<TkBootSettings>().Value;
        var seconds = settings.ColdSeconds;
        var wood = boot.GameData.Read("item:Material_T1_Wood_Logs");
        if (!wood.Ok) throw new InvalidOperationException(wood.Reason);
        var d = BootDefinitions.For(boot);
        var status = d.Define("tk-cold", new StatusBlock { Name = "Kit Cold", Seconds = seconds });
        if (!status.Ok) throw new InvalidOperationException(status.Reason);
        var item = d.Define("tk-token", new ItemBlock
        {
            Name = "Kit Token", Stack = wood.Data!.Value.GetProperty("maxStackSize").GetInt32(),
            Visual = settings.TokenIcon is { } icon ? (PartRef<VisualBlock>)new VisualBlock { Icon = icon } : null,
        });
        if (!item.Ok) throw new InvalidOperationException(item.Reason);
    }

    public override void OnLoad()
    {
        Config.Register<TkBootSettings>();
        Events.On<BootDefinitionsCommittedEvent>(e => Context.Log.Info($"tk-boot: committed {string.Join(", ", e.Definitions)} in generation {e.Generation}"));
    }
}
