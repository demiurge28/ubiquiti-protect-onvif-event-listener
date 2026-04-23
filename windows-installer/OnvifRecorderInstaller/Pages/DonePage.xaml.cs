using System.Diagnostics;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Navigation;

namespace OnvifRecorderInstaller.Pages;

public partial class DonePage : Page {
    private readonly MainWindow _main;
    private readonly string _adminUrl;

    public DonePage(MainWindow main, string version) {
        InitializeComponent();
        _main = main;
        _adminUrl = $"https://{main.Connection.Host}/onvif/admin/";
        SummaryText.Text = string.IsNullOrEmpty(version)
            ? $"onvif-recorder is installed on {main.Connection.Host}."
            : $"onvif-recorder v{version} is installed on {main.Connection.Host}.";
        AdminLinkText.Text = _adminUrl;
        AdminLink.NavigateUri = new System.Uri(_adminUrl);
    }

    private void Link_RequestNavigate(object sender, RequestNavigateEventArgs e) {
        Process.Start(new ProcessStartInfo(e.Uri.AbsoluteUri) {
            UseShellExecute = true,
        });
        e.Handled = true;
    }

    private void Dashboard_Click(object sender, RoutedEventArgs e) {
        _main.Go(new DashboardPage(_main));
    }

    private void Close_Click(object sender, RoutedEventArgs e) {
        Window.GetWindow(this)?.Close();
    }
}
