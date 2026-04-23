using System.Windows;
using System.Windows.Controls;

namespace OnvifRecorderInstaller.Pages;

public partial class WelcomePage : Page {
    private readonly MainWindow _main;

    public WelcomePage(MainWindow main) {
        InitializeComponent();
        _main = main;
    }

    private void Next_Click(object sender, RoutedEventArgs e) {
        _main.Go(new EnableSshPage(_main));
    }
}
