using System.Windows;
using System.Windows.Controls;
using OnvifRecorderInstaller.Services;

namespace OnvifRecorderInstaller.Pages;

public partial class ChannelPage : Page {
    private readonly MainWindow _main;

    public ChannelPage(MainWindow main) {
        InitializeComponent();
        _main = main;
        Select(_main.Connection.Channel);
        _ = DetectAsync();
    }

    private async System.Threading.Tasks.Task DetectAsync() {
        RunResult result;
        try {
            result = await _main.Ssh.RunCaptureAsync(
                _main.Connection, InstallScripts.BuildDetectChannelScript());
        } catch (System.Exception ex) {
            DetectedText.Text =
                "Could not read release channel from the device (" + ex.Message +
                "); defaulting to Stable.";
            return;
        }
        if (!result.Ok) {
            DetectedText.Text =
                "Could not read release channel from the device; defaulting to Stable.";
            return;
        }
        var detected = result.Stdout.Trim();
        if (string.IsNullOrEmpty(detected)) {
            DetectedText.Text =
                "Could not determine the Protect channel; defaulting to Stable.";
            return;
        }
        DetectedText.Text = $"Detected Protect channel: {detected}";
        Select(detected);
    }

    private void Select(string channel) {
        StableRadio.IsChecked = channel == "stable";
        RcRadio.IsChecked = channel == "rc";
        EaRadio.IsChecked = channel == "early-access";
        if (StableRadio.IsChecked != true
            && RcRadio.IsChecked != true
            && EaRadio.IsChecked != true) {
            StableRadio.IsChecked = true;
        }
    }

    private string Selected() {
        if (RcRadio.IsChecked == true) return "rc";
        if (EaRadio.IsChecked == true) return "early-access";
        return "stable";
    }

    private void Back_Click(object sender, RoutedEventArgs e) {
        _main.Go(new ConnectPage(_main));
    }

    private void Install_Click(object sender, RoutedEventArgs e) {
        _main.Connection.Channel = Selected();
        _main.Go(new InstallPage(_main, InstallPage.Mode.Install));
    }
}
