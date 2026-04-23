using System.Diagnostics;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Navigation;

namespace OnvifRecorderInstaller.Pages;

public partial class EnableSshPage : Page {
    private readonly MainWindow _main;

    public EnableSshPage(MainWindow main) {
        InitializeComponent();
        _main = main;
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
