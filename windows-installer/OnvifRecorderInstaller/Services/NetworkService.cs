using System.Linq;
using System.Net;
using System.Net.NetworkInformation;
using System.Net.Sockets;

namespace OnvifRecorderInstaller.Services;

// Helpers for inspecting local network state — currently just the IPv4
// default gateway, used to pre-fill the UniFi router URL on the SSH
// instructions page.
public static class NetworkService {
    // Returns the IPv4 default gateway of the first up, non-loopback,
    // non-virtual interface that has one configured, or null when none
    // is available (no network, all-virtual host, etc.).
    public static IPAddress? GetDefaultIPv4Gateway() {
        foreach (var ni in NetworkInterface.GetAllNetworkInterfaces()) {
            if (ni.OperationalStatus != OperationalStatus.Up) continue;
            if (ni.NetworkInterfaceType == NetworkInterfaceType.Loopback)
                continue;
            if (ni.NetworkInterfaceType == NetworkInterfaceType.Tunnel)
                continue;
            var props = ni.GetIPProperties();
            var gw = props.GatewayAddresses
                .Select(g => g.Address)
                .FirstOrDefault(a =>
                    a != null
                    && a.AddressFamily == AddressFamily.InterNetwork
                    && !a.Equals(IPAddress.Any));
            if (gw != null) return gw;
        }
        return null;
    }
}
