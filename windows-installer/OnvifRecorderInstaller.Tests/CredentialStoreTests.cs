using System;
using System.Runtime.Versioning;
using OnvifRecorderInstaller.Services;
using Xunit;

namespace OnvifRecorderInstaller.Tests;

// The test project targets net8.0-windows, so these tests only run on
// Windows. The OS guard inside each test is belt-and-braces: the project
// system won't build it against non-Windows runtimes anyway.
[SupportedOSPlatform("windows")]
public class CredentialStoreTests {
    [Fact]
    public void Protect_Unprotect_RoundTripsPlaintext() {
        if (!OperatingSystem.IsWindows()) return;
        var original = "hunter2 non-ascii fine \u2603";
        var cipher = CredentialStore.Protect(original);
        Assert.NotNull(cipher);
        var recovered = CredentialStore.Unprotect(cipher);
        Assert.Equal(original, recovered);
    }

    [Fact]
    public void Protect_NullOrEmpty_ReturnsNull() {
        if (!OperatingSystem.IsWindows()) return;
        Assert.Null(CredentialStore.Protect(null));
        Assert.Null(CredentialStore.Protect(""));
    }

    [Fact]
    public void Unprotect_NullOrEmpty_ReturnsNull() {
        if (!OperatingSystem.IsWindows()) return;
        Assert.Null(CredentialStore.Unprotect(null));
        Assert.Null(CredentialStore.Unprotect(Array.Empty<byte>()));
    }

    [Fact]
    public void SaveLoad_RoundTripsConnection() {
        if (!OperatingSystem.IsWindows()) return;
        var store = new CredentialStore();
        var existed = store.Exists();
        var previous = existed ? store.Load() : null;
        try {
            var c = new Connection {
                Host = "10.20.30.40",
                Port = 2222,
                Username = "root",
                AuthMethod = AuthMethod.Password,
                Password = "s3cret!",
                HostFingerprint = "SHA256:abc",
                Channel = "rc",
                LastInstalledVersion = "1.5.0",
            };
            store.Save(c);
            var loaded = store.Load();
            Assert.NotNull(loaded);
            Assert.Equal(c.Host, loaded!.Host);
            Assert.Equal(c.Port, loaded.Port);
            Assert.Equal(c.Username, loaded.Username);
            Assert.Equal(AuthMethod.Password, loaded.AuthMethod);
            Assert.Equal(c.Password, loaded.Password);
            Assert.Equal(c.HostFingerprint, loaded.HostFingerprint);
            Assert.Equal(c.Channel, loaded.Channel);
            Assert.Equal(c.LastInstalledVersion, loaded.LastInstalledVersion);
        } finally {
            store.Forget();
            if (previous != null) store.Save(previous);
        }
    }
}
