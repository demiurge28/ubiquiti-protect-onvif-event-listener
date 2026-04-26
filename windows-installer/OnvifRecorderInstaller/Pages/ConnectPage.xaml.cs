using System;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Threading;
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

    // Set during PasswordChanged / TextChanged → ApplyFormToConnection so
    // we don't keep two parallel state bags in sync via a getter chain.
    // _syncing prevents the two TextChanged/PasswordChanged handlers from
    // re-firing each other when the Show toggle copies between them.
    private bool _syncing;

    // Mirrors the router's pam_faillock policy
    // (deny=3, unlock_time=180): after 3 consecutive auth failures, lock
    // the Test button locally so the user doesn't trip the server-side
    // lockout (which would shift the failure mode to the misleading
    // "No supported authentication methods available").  181s instead
    // of exactly 180 to give the server a one-second margin.
    private const int kAuthFailureLimit = 3;
    private const int kAuthCooldownSeconds = 181;
    private int _authFailures;
    private DispatcherTimer? _cooldownTimer;
    private DateTime _cooldownExpires;

    private void AuthCombo_Changed(object sender, SelectionChangedEventArgs e) {
        if (!IsLoaded) return;
        bool password = AuthCombo.SelectedIndex == 0;
        SecretLabel.Text = password ? "Password:" : "Key file:";
        // Show whichever password control matches the current toggle state.
        bool showPlain = ShowPassword.IsChecked == true;
        PasswordBox.Visibility = password && !showPlain
            ? Visibility.Visible : Visibility.Collapsed;
        PasswordPlainBox.Visibility = password && showPlain
            ? Visibility.Visible : Visibility.Collapsed;
        ShowPassword.Visibility = password
            ? Visibility.Visible : Visibility.Collapsed;
        KeyFileRow.Visibility = password ? Visibility.Collapsed : Visibility.Visible;
        PassphraseLabel.Visibility = password ? Visibility.Collapsed : Visibility.Visible;
        PassphraseBox.Visibility = password ? Visibility.Collapsed : Visibility.Visible;
    }

    private void ShowPassword_Toggled(object sender, RoutedEventArgs e) {
        if (!IsLoaded) return;
        bool showPlain = ShowPassword.IsChecked == true;
        if (showPlain) {
            // Reveal: copy whatever was masked into the plain text box.
            _syncing = true;
            PasswordPlainBox.Text = PasswordBox.Password;
            _syncing = false;
            PasswordBox.Visibility = Visibility.Collapsed;
            PasswordPlainBox.Visibility = Visibility.Visible;
            PasswordPlainBox.Focus();
        } else {
            // Hide: copy back into the secure box.
            _syncing = true;
            PasswordBox.Password = PasswordPlainBox.Text;
            _syncing = false;
            PasswordPlainBox.Visibility = Visibility.Collapsed;
            PasswordBox.Visibility = Visibility.Visible;
            PasswordBox.Focus();
        }
    }

    private void PasswordBox_PasswordChanged(object sender, RoutedEventArgs e) {
        if (_syncing || !IsLoaded) return;
        _syncing = true;
        PasswordPlainBox.Text = PasswordBox.Password;
        _syncing = false;
    }

    private void PasswordPlainBox_TextChanged(object sender, TextChangedEventArgs e) {
        if (_syncing || !IsLoaded) return;
        _syncing = true;
        PasswordBox.Password = PasswordPlainBox.Text;
        _syncing = false;
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
            _authFailures = 0;  // success clears the local fail counter
        } else {
            StatusText.Foreground = System.Windows.Media.Brushes.DarkRed;
            StatusText.Text = "Failed: " + result.Error;
            if (result.IsAuthFailure) {
                _authFailures++;
                if (_authFailures >= kAuthFailureLimit) {
                    StartLockoutCooldown();
                }
            }
        }
    }

    private void StartLockoutCooldown() {
        _cooldownExpires = DateTime.UtcNow.AddSeconds(kAuthCooldownSeconds);
        TestButton.IsEnabled = false;
        LockoutWarning.Visibility = Visibility.Visible;
        _cooldownTimer?.Stop();
        _cooldownTimer = new DispatcherTimer {
            Interval = TimeSpan.FromSeconds(1),
        };
        _cooldownTimer.Tick += (_, _) => UpdateCooldown();
        _cooldownTimer.Start();
        UpdateCooldown();
    }

    private void UpdateCooldown() {
        var remaining = (int)Math.Ceiling(
            (_cooldownExpires - DateTime.UtcNow).TotalSeconds);
        if (remaining <= 0) {
            _cooldownTimer?.Stop();
            _cooldownTimer = null;
            _authFailures = 0;
            LockoutWarning.Visibility = Visibility.Collapsed;
            TestButton.IsEnabled = true;
            return;
        }
        LockoutWarning.Text =
            $"3 wrong-password attempts; the router locks the root account " +
            $"for 180 s after this point.  Test disabled for {remaining} s " +
            $"to let the lockout clear.";
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
