using System;
using OnvifRecorderInstaller.Services;
using Xunit;

namespace OnvifRecorderInstaller.Tests;

public class InstallScriptsTests {
    [Theory]
    [InlineData("stable")]
    [InlineData("rc")]
    [InlineData("early-access")]
    public void BuildInstallScript_IncludesChannel(string channel) {
        var script = InstallScripts.BuildInstallScript(channel);
        Assert.Contains($"echo \"{channel}\" > {InstallScripts.ChannelFile}", script);
        Assert.Contains($"{InstallScripts.RepoUrl} {channel} main", script);
    }

    [Fact]
    public void BuildInstallScript_ReferencesKeyringPath() {
        var script = InstallScripts.BuildInstallScript("stable");
        Assert.Contains(InstallScripts.Keyring, script);
        Assert.Contains($"{InstallScripts.RepoUrl}/onvif-recorder.gpg", script);
    }

    [Fact]
    public void BuildInstallScript_RejectsUnknownChannel() {
        Assert.Throws<ArgumentException>(() =>
            InstallScripts.BuildInstallScript("weekly"));
    }

    [Fact]
    public void BuildUpgradeScript_UsesOnlyUpgrade() {
        var script = InstallScripts.BuildUpgradeScript();
        Assert.Contains("apt-get install -y --only-upgrade onvif-recorder", script);
        Assert.Contains(InstallScripts.SourcesFile, script);
    }

    [Fact]
    public void BuildUninstallScript_PurgesAndCleansUp() {
        var script = InstallScripts.BuildUninstallScript();
        Assert.Contains("apt-get purge -y onvif-recorder", script);
        Assert.Contains($"rm -f {InstallScripts.SourcesFile}", script);
        Assert.Contains($"rm -f {InstallScripts.Keyring}", script);
    }

    [Fact]
    public void BuildDetectChannelScript_MapsProtectChannelToAptSuite() {
        var script = InstallScripts.BuildDetectChannelScript();
        Assert.Contains(InstallScripts.RunnablesYaml, script);
        Assert.Contains("release)", script);
        Assert.Contains("release-candidate)", script);
        Assert.Contains("beta|early-access)", script);
        Assert.Contains("echo \"stable\"", script);
        Assert.Contains("echo \"rc\"", script);
        Assert.Contains("echo \"early-access\"", script);
    }
}
