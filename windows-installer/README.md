# ONVIF Recorder — Windows installer

Native Windows GUI that installs / upgrades / uninstalls `onvif-recorder` on a
UniFi Dream Router or NVR over SSH. Builds to a single self-contained
`.exe`. Source of truth for the apt install commands it runs is
[`gh-pages/install.sh`](../gh-pages/install.sh).

## Build

Requires the .NET 8 SDK on Windows.

```powershell
dotnet restore OnvifRecorderInstaller.sln
dotnet build    OnvifRecorderInstaller.sln -c Release
dotnet test     OnvifRecorderInstaller.Tests
```

## Publish a single-file exe

```powershell
dotnet publish OnvifRecorderInstaller\OnvifRecorderInstaller.csproj `
  -c Release -r win-x64 --self-contained `
  -p:PublishSingleFile=true `
  -p:IncludeNativeLibrariesForSelfExtract=true `
  -p:PublishTrimmed=false `
  -o publish-out
```

CI publishes this exe automatically on `v*` tag pushes via
`.github/workflows/release-windows.yml`.

## Layout

```
OnvifRecorderInstaller/
  App.xaml                       # Application-level resources + styles
  MainWindow.xaml                # Hosts a Frame; swaps Pages
  Pages/
    WelcomePage                  # Intro + overview
    EnableSshPage                # Guide user through enabling SSH in UniFi OS
    ConnectPage                  # Host/user/password form, test connection
    ChannelPage                  # Auto-detects Protect channel; user confirms
    InstallPage                  # Streams install/upgrade/uninstall over SSH
    DonePage                     # Shown after fresh install
    DashboardPage                # Shown on subsequent launches (creds saved)
  Services/
    Connection                   # In-memory credential holder
    CredentialStore              # Registry + DPAPI persistence
    InstallScripts               # Install/upgrade/uninstall shell scripts
    SshService                   # SSH.NET wrapper
  Assets/                        # icon.ico, enable-ssh.png (optional)

OnvifRecorderInstaller.Tests/
  InstallScriptsTests            # Verify script generation
  CredentialStoreTests           # Verify DPAPI + registry round-trip
```

## Where credentials live

`HKEY_CURRENT_USER\Software\OnvifRecorderInstaller` — see
[`CredentialStore.cs`](OnvifRecorderInstaller/Services/CredentialStore.cs).
Secrets are DPAPI-encrypted with `CurrentUser` scope + a fixed entropy
string so a raw registry dump is not enough to recover them.

"Change router" / "Uninstall → forget credentials" deletes the whole
subkey.
