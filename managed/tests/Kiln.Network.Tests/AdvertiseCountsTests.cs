// SPDX-License-Identifier: MIT
// The owner's real-client gem check, 2026-11-16: a game hosting its world with GemEffects switched off in the profile
// refused its own player ("This server requires mods you do not have: gemeffects"): the host still advertised the
// profile-disabled mod as one players need.
using Xunit;
namespace Kiln.Network.Tests;

public sealed class AdvertiseCountsTests
{
    private static string Folder(bool disabledFile)
    {
        var dir = Path.Combine(Path.GetTempPath(), "kiln-advert-counts-" + Guid.NewGuid());
        Directory.CreateDirectory(dir);
        if (disabledFile) File.WriteAllText(Path.Combine(dir, ".disabled"), "");
        return dir;
    }

    [Fact]
    public void A_mod_switched_off_in_the_profile_is_not_one_players_need()
    {
        var dir = Folder(false);
        Assert.False(NetAdvertise.Counts(ModState.Skipped, "disabled in profile", dir));
        Assert.False(NetAdvertise.Counts(ModState.Skipped, NetAdvertise.DisabledInProfile, dir));
    }

    [Fact]
    public void Loaded_and_side_skipped_mods_still_count_failed_and_disabled_files_do_not()
    {
        var dir = Folder(false);
        Assert.True(NetAdvertise.Counts(ModState.Loaded, null, dir));
        Assert.True(NetAdvertise.Counts(ModState.Skipped, "side 'client': not loaded on a server", dir), "players need a client mod a server skips");
        Assert.True(NetAdvertise.Counts(ModState.Disabled, "fault policy", dir), "switched off by faults: unchanged");
        Assert.False(NetAdvertise.Counts(ModState.Failed, "bad manifest", dir));
        Assert.False(NetAdvertise.Counts(ModState.Loaded, null, Folder(true)), "a .disabled file");
        Assert.False(NetAdvertise.Counts(ModState.Loaded, null, ""));
    }
}
