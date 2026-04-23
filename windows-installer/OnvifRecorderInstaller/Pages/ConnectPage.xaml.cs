using System;
using System.Windows;
using System.Windows.Controls;
using Microsoft.Win32;
using OnvifRecorderInstaller.Services;

namespace OnvifRecorderInstaller.Pages;

public partial class ConnectPage : Page {
    private readonly MainWindow _main;

    public ConnectPage(MainWindow main) {
        InitializeComponent();
        _main = main;
        var c = main.Connection;
        HostBox.Text = c.Host;
        PortBox.Text = c.Port.ToString();
        UserBox.Text = c.Username;
        AuthCombo.SelectedIndex = c.AuthMethod == AuthMethod.Password ? 0 : 1;
        if (!string.IsNullOrEmpty(c.PrivateKeyPath)) {
            KeyPathBox.Text = c.PrivateKeyPath;
        }
    }

    private void AuthCombo_Changed(object sender, SelectionChangedEventArgs e) {
        if (!IsLoaded) return;
        bool password = AuthCombo.SelectedIndex == 0;
        SecretLabel.Text = password ? "Password:" : "Key file:";
        PasswordBox.Visibility = password ? Visibility.Visible : Visibility.Collapsed;
        KeyFileRow.Visibility = password ? Visibility.Collapsed : Visibility.Visible;
        PassphraseLabel.Visibility = password ? Visibility.Collapsed : Visibility.Visible;
        PassphraseBox.Visibility = password ? Visibility.Collapsed : Visibility.Visible;
    }

    private void Browse_Click(object sender, RoutedEventArgs e) {
        var dlg = new OpenFileDialog {
            Title = "Select OpenSSH private key",
            Filter = "All files (*.*)|*.*",
        };
        if (dlg.ShowDialog() == true) {
            KeyPathBox.Text = dlg.FileName;
        }
    }

    private async void Test_Click(object sender, RoutedEventArgs e) {
        if (!ApplyFormToConnection(out var error)) {
            StatusText.Text = error;
            StatusText.Foreground = System.Windows.Media.Brushes.DarkRed;
            return;
        }
        StatusText.Foreground = System.Windows.Media.Brushes.Black;
        StatusText.Text = "Testing…";
        FingerprintText.Text = "";
        NextButton.IsEnabled = false;

        var result = await _main.Ssh.TestConnectionAsync(_main.Connection);
        if (result.Ok) {
            StatusText.Foreground = System.Windows.Media.Brushes.DarkGreen;
            StatusText.Text = "Connected.";
            _main.Connection.HostFingerprint = result.Fingerprint;
            FingerprintText.Text =
                $"Host key fingerprint:\n{result.Fingerprint}\n" +
                "This fingerprint will be pinned on subsequent connections.";
            NextButton.IsEnabled = true;
        } else {
            StatusText.Foreground = System.Windows.Media.Brushes.DarkRed;
            StatusText.Text = "Failed: " + result.Error;
        }
    }

    private bool ApplyFormToConnection(out string error) {
        error = "";
        if (string.IsNullOrWhiteSpace(HostBox.Text)) {
            error = "Host is required."; return false;
        }
        if (!int.TryParse(PortBox.Text, out var port) || port <= 0 || port > 65535) {
            error = "Invalid port."; return false;
        }
        if (string.IsNullOrWhiteSpace(UserBox.Text)) {
            error = "Username is required."; return false;
        }
        var c = _main.Connection;
        c.Host = HostBox.Text.Trim();
        c.Port = port;
        c.Username = UserBox.Text.Trim();
        c.AuthMethod = AuthCombo.SelectedIndex == 0
            ? AuthMethod.Password : AuthMethod.PrivateKey;
        if (c.AuthMethod == AuthMethod.Password) {
            if (string.IsNullOrEmpty(PasswordBox.Password)) {
                error = "Password is required."; return false;
            }
            c.Password = PasswordBox.Password;
            c.PrivateKeyPath = null;
            c.PrivateKeyPassphrase = null;
        } else {
            if (string.IsNullOrWhiteSpace(KeyPathBox.Text)) {
                error = "Key file is required."; return false;
            }
            c.PrivateKeyPath = KeyPathBox.Text.Trim();
            c.PrivateKeyPassphrase = string.IsNullOrEmpty(PassphraseBox.Password)
                ? null : PassphraseBox.Password;
            c.Password = null;
        }
        return true;
    }

    private void Back_Click(object sender, RoutedEventArgs e) {
        _main.Go(new EnableSshPage(_main));
    }

    private void Next_Click(object sender, RoutedEventArgs e) {
        _main.Go(new ChannelPage(_main));
    }
}
