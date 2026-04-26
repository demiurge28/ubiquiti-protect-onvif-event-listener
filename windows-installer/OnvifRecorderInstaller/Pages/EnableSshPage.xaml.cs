using System;
using System.Diagnostics;
using System.Net;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Navigation;
using OnvifRecorderInstaller.Services;

namespace OnvifRecorderInstaller.Pages;

public partial class EnableSshPage : Page {
    private readonly MainWindow _main;

    private const string kRouterPath =
        "/network/default/settings/control-plane/console";
    // Used as the placeholder when no default gateway can be detected.
    private const string kFallbackIp = "192.168.1.1";

    public EnableSshPage(MainWindow main) {
        InitializeComponent();
        _main = main;

        var gw = NetworkService.GetDefaultIPv4Gateway();
        var ip = gw?.ToString() ?? kFallbackIp;
        IpOverride.Text = ip;
        UpdateConsoleLink(ip);
    }

    private void UpdateConsoleLink(string ip) {
        var url = "https://" + ip + kRouterPath;
        ConsoleLink.NavigateUri = new Uri(url);
        ConsoleLinkText.Text = url;
    }

    private void IpOverride_TextChanged(object sender, TextChangedEventArgs e) {
        var ip = IpOverride.Text.Trim();
        if (IPAddress.TryParse(ip, out _)) {
            UpdateConsoleLink(ip);
        }
    }

    private void Done_Changed(object sender, RoutedEventArgs e) {
        NextButton.IsEnabled = Done.IsChecked == true;
    }

    private void Back_Click(object sender, RoutedEventArgs e) {
        _main.Go(new WelcomePage(_main));
    }

    private void Next_Click(object sender, RoutedEventArgs e) {
        _main.Go(new ConnectPage(_main));
    }

    private void Link_RequestNavigate(object sender, RequestNavigateEventArgs e) {
        Process.Start(new ProcessStartInfo(e.Uri.AbsoluteUri) {
            UseShellExecute = true,
        });
        e.Handled = true;
    }
}
