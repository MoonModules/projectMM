# ImprovProvisioningModule

Browser-driven WiFi provisioning over USB-serial, using the [Improv-WiFi](https://www.improv-wifi.com/) protocol. Bridges credentials from a Chrome / Edge / Opera tab — or from `scripts/build/improv_provision.py` for rack/CI use — into `NetworkModule::setWifiCredentials`, which writes the same buffers the AP-fallback UI flow uses. The protocol parser + UART task live in the platform layer; this module is the status surface that polls a ready-flag and bridges credentials to NetworkModule on the scheduler thread.

A code-wired child of NetworkModule. The wiring calls `markWiredByCode()` so the persistence-apply step preserves the child across reboots even on devices whose `Network.json` predates the addition (see [Persistence — code-wired children](../../architecture.md#persistence)).

The browser flow runs immediately after a Web Serial flash (ESP Web Tools recognises Improv-capable firmware and offers a "Connect to Wi-Fi?" dialog automatically). The CLI flow uses [`scripts/build/improv_provision.py`](../../../scripts/build/improv_provision.py) over the same USB cable for headless or rack provisioning.

## Controls

| Name | Type | Description |
|---|---|---|
| `provision_status` | read-only string (64 chars) | One of: `listening`, `received credentials`, `connecting`, `connected: <ssid>`, `error: <reason>`, or `not supported on this platform` (desktop). |

## ESP32-S3 USB-port footnote

The listener serves **both** serial transports: UART0 (external USB-to-UART bridges) and the S3's native USB-Serial-JTAG port — boards that only expose native USB (the ESP32-S3 N16R8 Dev among them) provision over that port directly (proven on the bench 2026-06-10). If neither serial path is available, the AP-mode flow remains: the device boots a SoftAP at `4.3.2.1`, join from a phone, enter credentials.

## Wire contract

Both transports speak the same Improv-WiFi serial protocol — frames of `IMPROV` + version byte + type + length + payload + checksum. Full protocol details: <https://www.improv-wifi.com/serial/>. The on-device implementation supports four standard RPC commands plus two vendor extensions:

- `GET_CURRENT_STATE` — returns "authorized" or "provisioned" depending on whether WiFi STA is connected.
- `GET_DEVICE_INFO` — returns `[firmware, version, chipFamily, deviceName]` (where `firmware` = `"projectMM"`, `version` from `kVersion` in `build_info.h`, `chipFamily` from `platform::chipModel()`, `deviceName` from `SystemModule`).
- `GET_WIFI_NETWORKS` — runs a synchronous WiFi scan, returns up to 10 SSIDs with RSSI + auth flag. **Rejected while STA is connected** (see below).
- `WIFI_SETTINGS` — writes SSID + password to NetworkModule via `setWifiCredentials`, polls `wifiStaConnected()` for up to 30 s, replies with success (carrying `http://<ip>/`) or `ERROR_UNABLE_TO_CONNECT`.
- `SET_BOARD` (vendor, `0xFE`) — payload `[str_len][board name]`; persists the physical-board name into BoardModule. Sent by the web installer after provisioning.
- `SET_TX_POWER` (vendor, `0xFD`) — payload `[1][dBm]` (0–21; 0 lifts the cap); persists + applies `Network.txPowerSetting` **before** any association attempt. This is the provisioning escape hatch for boards whose LDO browns out at full TX power (a weak LDO / marginal supply): their `boards.json` cap normally arrives over HTTP *after* the device is online — which a browning-out board can never reach, since it fails WiFi auth at 20 dBm first. `improv_provision.py --tx-power 8` (and the MoonDeck flow) sends this ahead of the credentials; error `0x81` on an out-of-range value.

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

## Prior art

- **projectMM-v1's `deploy/wifi.py` + `deploy/flashfs.py --wifi`** — wrote credentials to a local `data/state/sta1.json`, baked them into a LittleFS partition image, esptool-flashed the partition to each `test: true` device in `deploy/devicelist.json`. Same rack-provisioning use case; Improv replaces the partition-baking-and-reflashing path with live serial provisioning (devices stay running, no flash mode required).
- **Improv-WiFi** is the standard ESPHome / Home Assistant uses for cross-firmware WiFi provisioning. Library: `improv/improv` on the [ESP Component Registry](https://components.espressif.com/components/improv/improv) (source: <https://github.com/improv-wifi/sdk-cpp>); specification: <https://www.improv-wifi.com/serial/>.

## Source

[ImprovProvisioningModule.h](../../../src/core/ImprovProvisioningModule.h)
