# Core2 UI demo images

These 960 × 720 PNGs illustrate the 320 × 240 firmware layout with synthetic
network data. They are maintained drawings, not captures from a physical device.

| File | Screen contents |
| --- | --- |
| `network.png` | Link status, IPv4 address, VLAN, gateway, DNS, DHCP lease, and switch port |
| `switch.png` | Switch identity, connected port, native and voice VLANs, model, and discovery source |
| `tools.png` | Eight diagnostic tool icons and the main navigation tabs |
| `extra-tools.png` | Wi-Fi scanner entry and wireless survey description |
| `wifi-list.png` | Access points sorted by signal, with SSID, channel, security, and BSSID suffix |
| `wifi-identity.png` | Selected access point's SSID, BSSID, and advertised security |
| `wifi-radio.png` | RSSI, channel, frequency, channel width, PHY modes, country, WPS, and ciphers |
| `ping.png` | ICMP replies, packet loss, and latency summary |

Regenerate all images on macOS from the repository root:

```bash
swift scripts/render-demo.swift
```

Update the renderer alongside firmware layout changes. Physical LCD font
rendering may differ from these AppKit illustrations.
