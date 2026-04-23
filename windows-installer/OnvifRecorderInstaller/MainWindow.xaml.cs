using System.Windows;
using OnvifRecorderInstaller.Pages;
using OnvifRecorderInstaller.Services;

namespace OnvifRecorderInstaller;

public partial class MainWindow : Window {
    public CredentialStore Store { get; } = new();
    public SshService Ssh { get; } = new();
    public Connection Connection { get; private set; } = new();

    public MainWindow() {
        InitializeComponent();
        var saved = Store.Load();
        if (saved != null && saved.HasCredentials) {
            Connection = saved;
            RootFrame.Navigate(new DashboardPage(this));
        } else {
            RootFrame.Navigate(new WelcomePage(this));
        }
    }

    public void Go(System.Windows.Controls.Page page) {
        RootFrame.Navigate(page);
    }
}
