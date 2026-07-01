# Drivers

Every driver, one block each: what it does and what each control means — together. A driver reads its window of the [Drivers](../Drivers.md) container's shared buffer, applies the shared [output correction](../Drivers.md#output-correction) (brightness / channel order / RGBW white) per light, and sends the result out — over a wire protocol (WS2812 on RMT / LCD_CAM / Parlio), over the network (Art-Net / E1.31 / DDP), to a smart-light hub (Hue), or to the web UI (Preview). A driver is added as a child of the `Drivers` container per board through the catalog (`POST /api/modules`, a board's [`deviceModels.json`](../../../install/deviceModels.json) `modules` entry), so a device only carries the outputs its board actually has; `PreviewDriver` is the one boot-wired driver.

Each block links to that driver's **detail page** for the deeper material a control list can't carry — wire contracts, buffer slicing, memory sizing, per-protocol chunking, the on-device loopback self-test, and troubleshooting. Every driver shares the `start` / `count` **source-window** controls ([Drivers § Per-driver source window](../Drivers.md#per-driver-source-window-start--count)): the slice `[start, start+count)` of the buffer this driver sends (`count` 0 = to the end), so different drivers can cover different ranges of one buffer.

**Jump to:** [LED output](#led-output-drivers) · [Network](#network-drivers) · [Smart light](#smart-light-drivers) · [Preview](#preview-drivers)

## LED output drivers

<a id="rmtled"></a>

### RMT LED

WS2812B-class addressable LEDs over the ESP32 **RMT** peripheral — one GPIO and one RMT TX channel per strand. The general single-/few-strand LED output; the default LED driver for classic ESP32 and S3 board entries. Runs on any chip whose RMT has TX channels (classic ESP32: 8, S3: 4, P4: 4 DMA-backed); inert on desktop.

- `pins` — comma-separated data / TX GPIO list, e.g. `18,17,16` (one RMT TX channel per pin). Empty by default (idles until set). Changing it re-inits the channels live, no reboot.
- `ledsPerPin` — comma-separated lights-per-pin, matched to `pins` by position; empty or shorter than `pins` splits the remainder evenly.
- `loopbackTest` — persistent on/off mode for the RMT TX→RX loopback self-test (jumper the first `pins` entry to `loopbackRxPin`); verdict lands in the status field.
- `loopbackFrame` — whole-frame variant of the self-test (transmits a real frame back-to-back, bit-verifies the whole capture — catches frame-rate / RF corruption a 24-bit burst misses). Shown only while `loopbackTest` is on.
- `loopbackTxPin` — optional TX override for the self-test (transmit on this pin instead of `pins[0]`). Shown only while `loopbackTest` is on.
- `loopbackRxPin` — the RX pin for the self-test. Shown only while `loopbackTest` is on.

Detail: [RmtLedDriver.md](RmtLedDriver.md) — WS2812B wire contract, buffer slicing, concurrent show, the loopback self-test, and the LED-flicker troubleshooting playbook.

<a id="lcdled"></a>

### LCD LED

Parallel WS2812B on the **ESP32-S3** over the **LCD_CAM** peripheral: up to **8 strands clock out simultaneously**, one GPIO per strand, all fed by a single autonomous DMA transfer — the S3's scale path where RMT tops out at 4 channels.

- `pins` — comma-separated data GPIOs, one lane each, **exactly 8** (the i80 peripheral configures every data line of the bus width). Empty by default (idles until set). Changing it re-creates the i80 bus live.
- `ledsPerPin` — lights per lane, matched by position; empty = even split. Give unused lanes `0` to drive fewer than 8 strands.
- `clockPin` (default 10) — the i80 bus WR line; required on a real GPIO by the peripheral, ignored by the LEDs, overridable.
- `dcPin` (default 11) — the i80 data/command line; same story — required, unused by the LEDs, overridable.
- `loopbackTest` — one-shot whole-frame signal self-test (jumper the first pin to `loopbackRxPin`). Result in the status field.
- `loopbackTxPin` — optional TX override (drives lane 0 with the test pattern on this pin instead of `pins[0]`). Shown only while `loopbackTest` is on.
- `loopbackRxPin` — the RX pin for the self-test. Shown only while `loopbackTest` is on.

Detail: [LcdLedDriver.md](LcdLedDriver.md) — 3-slot-per-bit wire contract, buffer slicing, DMA memory sizing, and cross-domain wiring.

<a id="parlioled"></a>

### Parlio LED

Parallel WS2812B on the **ESP32-P4** over the **Parlio (Parallel IO)** TX peripheral: up to **8 strands** clock out simultaneously via one autonomous DMA transfer — the P4's preferred parallel path (the sibling of the LCD driver on the S3). Runs on **1–8 lanes** (no exactly-8 rule), with no clock/dc pins (Parlio generates its own pixel clock).

- `pins` — comma-separated data GPIOs, one lane each, **1 to 8**. Empty by default (idles until set). On the P4-NANO avoid the strapping (34–38), Ethernet, C6-SDIO and I2C pins; a known-good set is `20,21,22,23,24,25,26,27`. Changing it re-creates the Parlio TX unit live.
- `ledsPerPin` — lights per lane, matched by position; empty = even split over the wired lanes.
- `loopbackTest` — one-shot whole-frame signal self-test (TX on the first pin, RX on `loopbackRxPin`). Verdict in the status field.
- `loopbackTxPin` — optional TX override (lane 0's test pattern goes to this pin instead of `pins[0]`). Shown only while `loopbackTest` is on.
- `loopbackRxPin` — the RX pin for the self-test. Shown only while `loopbackTest` is on.

Detail: [ParlioLedDriver.md](ParlioLedDriver.md) — the P4 pin budget (all three LED peripherals at once, up to 20 parallel strands), the shared 3-slot wire contract, memory sizing, and cross-domain wiring.

## Network drivers

<a id="networksend"></a>

### Network Send

![NetworkSend controls](../../../assets/light/drivers/NetworkSendDriver.png)

Streams the light buffer over UDP in one of three industry protocols — **Art-Net**, **E1.31 / sACN**, or **DDP** — selected by a control. Sends the whole frame as one burst per configured rate; compatible with pixel controllers (Falcon, Advatek), xLights, and LedFx.

- `protocol` (Art-Net / E1.31 / DDP, default Art-Net) — the wire protocol; the destination port follows automatically (6454 / 5568 / 4048). Changing it re-targets the socket live.
- `ip` (default 255.255.255.255) — destination address; the default limited-broadcast reaches every LAN receiver with no IP to type. Set a unicast address to target one device. (E1.31 multicast is deliberately not implemented — see the detail page.)
- `universe_start` (default 0) — first universe for Art-Net and E1.31; DDP is byte-addressed and ignores it.
- `fps` (default 50, range 1–120) — frame-rate limit (without it the loop would re-send every render tick; receivers expect a steady cadence).

Detail: [NetworkSendDriver.md](NetworkSendDriver.md) — per-protocol chunking table, E1.31/Art-Net interop notes, the synchronous-send caveat, and the packet-layout headers.

## Smart light drivers

<a id="hue"></a>

### Hue

![A HueDriver in the UI](../../../assets/light/drivers/Hue%20driver.png)

Drives **Philips Hue bulbs as pixels of an effect**: make a small grid, run any effect, add a HueDriver, and each colour bulb in the driver's window becomes one pixel, pushed to the bridge over the Hue HTTP API. Paces itself to the bridge's ~10 command/s rate limit — smooth ambient colour, not fast strobing. Only colour-capable, reachable bulbs are driven.

- `bridgeIp` — the Hue bridge's LAN IPv4 (find it via the bridge app, the router, or `discovery.meethue.com`).
- `appKey` — the Hue app key (username); filled automatically by `pair`, persisted as the driver's credential.
- `pair` — a button: press it, then press the bridge's physical link button within ~30 s; the driver claims a key and learns the light list.
- `room` / `light` — two dropdowns narrowing which colour lights are driven (both default to `All`); pick a room, then optionally one bulb.

Detail: [HueDriver.md](HueDriver.md) — what makes Hue different (rate limit, bridge-smoothed transitions, pairing), the Hue v1 HTTP wire contract, and the Devices-module listing.

## Preview drivers

<a id="preview"></a>

### Preview

![PreviewDriver controls](../../../assets/light/drivers/PreviewDriver.png)

Streams a true-shape 3D preview to the web UI over WebSocket as a **point list** — only the real lights at their real positions, not a dense grid — so a sphere, ring, or arbitrary fixture map shows in its true shape. The one boot-wired driver.

- `fps` (default 24, range 1–60) — preview stream rate (independent of the render loop).

Detail: [PreviewDriver.md](PreviewDriver.md) — the binary WebSocket protocol (coordinate table + per-frame channels), sparse-layout handling, and the large-layout spatial downsample.

## Source

- [RmtLedDriver.h](../../../../src/light/drivers/RmtLedDriver.h)
- [LcdLedDriver.h](../../../../src/light/drivers/LcdLedDriver.h)
- [ParlioLedDriver.h](../../../../src/light/drivers/ParlioLedDriver.h)
- [NetworkSendDriver.h](../../../../src/light/drivers/NetworkSendDriver.h)
- [HueDriver.h](../../../../src/light/drivers/HueDriver.h)
- [PreviewDriver.h](../../../../src/light/drivers/PreviewDriver.h)
