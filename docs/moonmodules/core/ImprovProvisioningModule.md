# ImprovProvisioningModule

Browser-driven WiFi provisioning over USB-serial, using the [Improv-WiFi](https://www.improv-wifi.com/) protocol. Bridges credentials from a Chrome / Edge / Opera tab — or from `scripts/build/improv_provision.py` — into `NetworkModule::setWifiCredentials`, which writes through to the same buffers the AP-fallback UI flow uses.

The protocol parser + UART listener task live in the platform layer (`mm::platform::improvProvisioningInit` at [src/platform/platform.h](../../../src/platform/platform.h)); this module is the status surface plus the bridge that hands credentials off to NetworkModule on the scheduler thread.

The browser flow runs immediately after a Web Serial flash (ESP Web Tools recognises Improv-capable firmware and offers a "Connect to Wi-Fi?" dialog automatically). The CLI flow uses [`scripts/build/improv_provision.py`](../../../scripts/build/improv_provision.py) over the same USB cable for headless or rack provisioning.

## Controls

| Name | Type | Description |
|---|---|---|
| `provision_status` | read-only string (64 chars) | One of: `listening`, `received credentials`, `connecting`, `connected: <ssid>`, `error: <reason>`, or `not supported on this platform` (desktop). |

## ESP32-S3 USB-port footnote

The ESP32-S3-DevKitC-1 has **two USB ports**. Improv only works on the silkscreen-labelled UART port (UART0 routed through the on-board USB-to-UART bridge). The native USB-Serial-JTAG port is a different hardware block and is not supported by the Improv listener.

If your S3 board only exposes the native USB-CDC port (some breakout boards), fall back to the AP-mode flow: device boots a SoftAP at `4.3.2.1`, join from a phone, enter credentials.

## Wire contract

Both transports speak the same Improv-WiFi serial protocol — frames of `IMPROV` + version byte + type + length + payload + checksum. Full protocol details: <https://www.improv-wifi.com/serial/>. The on-device implementation supports four RPC commands:

- `GET_CURRENT_STATE` — returns "authorized" or "provisioned" depending on whether WiFi STA is connected.
- `GET_DEVICE_INFO` — returns `[firmware, version, chipFamily, deviceName]` (where `firmware` = `"projectMM"`, `version` from `kVersion` in `build_info.h`, `chipFamily` from `platform::chipModel()`, `deviceName` from `SystemModule`).
- `GET_WIFI_NETWORKS` — runs a synchronous WiFi scan, returns up to 10 SSIDs with RSSI + auth flag. **Rejected while STA is connected** (see below).
- `WIFI_SETTINGS` — writes SSID + password to NetworkModule via `setWifiCredentials`, polls `wifiStaConnected()` for up to 30 s, replies with success (carrying `http://<ip>/`) or `ERROR_UNABLE_TO_CONNECT`.

`WIFI_SETTINGS` and `GET_WIFI_NETWORKS` are both **rejected with `ERROR_UNABLE_TO_CONNECT` while `platform::wifiStaConnected() == true`**. The scan gate protects large installs: `esp_wifi_scan_start` puts the radio into scan mode for 2-5 s, during which inbound ArtNet packets are dropped. On a 16K-LED rig that's a visible glitch. To re-provision a running device, wipe `ssid` via the UI and reboot, then run Improv before STA reconnects. `GET_CURRENT_STATE` and `GET_DEVICE_INFO` stay available regardless — they're read-only and don't touch the radio.

## How to test

**MoonDeck button** (the everyday flow):

ESP32 tab → pick the device's port → hit **Improv WiFi**. The script reads the host's currently-joined WiFi (macOS Keychain / Linux NetworkManager / Windows `netsh`) and pushes it to the device. The log pane shows `==> provisioned: http://<ip>/` on success.

**Browser** (alternate single-device flow):

1. Open <https://www.improv-wifi.com/> in Chrome / Edge / Opera on desktop.
2. Click **Connect**, pick the device's USB-serial port from the dialog.
3. Device name + chip + version appear in the page; click "Scan" to enumerate WiFi networks.
4. Enter SSID + password → page advances "connecting" → "connected" + clickable URL → opens the device UI.

**CLI** (rack / CI / scripted):

```bash
# Reuse the host's currently-joined WiFi (same path as the MoonDeck button):
uv run scripts/build/improv_provision.py --port /dev/tty.usbserial-XXXX

# Or override to push a different network's credentials:
uv run scripts/build/improv_provision.py \
  --port /dev/tty.usbserial-XXXX \
  --ssid "MyWiFi" \
  --password "hunter2"
# Exit 0 + prints "==> provisioned: http://<ip>/" on success.
```

For multiple devices on a USB hub:

```bash
for port in /dev/tty.usbserial-*; do
  uv run scripts/build/improv_provision.py --port "$port"
done
```

A future `--from-list <devicelist.json>` mode that reads a per-device manifest is on the 2.0 roadmap, deferred until projectMM has a devicelist schema.

## Prior art

- **projectMM-v1's `deploy/wifi.py` + `deploy/flashfs.py --wifi`** — wrote credentials to a local `data/state/sta1.json`, baked them into a LittleFS partition image, esptool-flashed the partition to each `test: true` device in `deploy/devicelist.json`. Same rack-provisioning use case; Improv replaces the partition-baking-and-reflashing path with live serial provisioning (devices stay running, no flash mode required).
- **Improv-WiFi** is the standard ESPHome / Home Assistant uses for cross-firmware WiFi provisioning. Library: `improv/improv` on the [ESP Component Registry](https://components.espressif.com/components/improv/improv) (source: <https://github.com/improv-wifi/sdk-cpp>); specification: <https://www.improv-wifi.com/serial/>.
