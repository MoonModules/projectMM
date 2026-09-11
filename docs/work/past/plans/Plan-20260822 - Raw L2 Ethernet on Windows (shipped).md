# Plan — Raw L2 Ethernet on Windows (drive ColorLight cards from a PC)

## Context

A ColorLight 5A-75 receiving card is connected to this Windows machine's Ethernet port. `PanelCardDriver` already speaks its wire format (`ColorLight5A75Packet.h`) and drives it from Linux (AF_PACKET) and macOS (BPF), but `platform::ethBindRawInterface` returns `false` on `_WIN32` — "No raw-L2 send without a third-party driver" — so the driver silently falls back to capture mode and nothing reaches the card.

ColorLight's own LEDVision does this from Windows, so the capability is not in question, only the code path. Npcap is already installed here (`C:\WINDOWS\System32\wpcap.dll`, `npf` service running), which is the same mechanism.

Outcome: a Windows PC becomes a panel controller, matching what a Pi or a Mac already does. Linux and macOS paths are untouched.

## Approach

**Runtime-load `wpcap.dll` via `LoadLibrary`/`GetProcAddress`, do not link it.** Npcap cannot be vendored or assumed, and a link-time dependency would propagate through `mm_platform` → `mm_core` → `mm_tests`, so CI and every contributor would need the SDK to build. Loading at bind time means the binary compiles and runs unchanged without Npcap, and simply reports it is missing. This introduces the repo's first `LoadLibrary` use, so it carries its reason at the call site (CLAUDE.md Principle 2).

Only four symbols are needed: `pcap_open_live`, `pcap_sendpacket`, `pcap_close`, `pcap_findalldevs` + `pcap_freealldevs`.

**Adapter naming: substring match.** A pcap device is `\Device\NPF_{GUID}`, 45+ chars, against a 16-byte `interface` control. Enumerate with `pcap_findalldevs` and pick the first device whose name *or* description contains the user's string, case-insensitively — so `Ethernet` or `HA-External` works. An exact device name still matches, and Linux/macOS keep their existing exact-name semantics.

**Link state via IPHLPAPI.** `ethLinkUp()` is hardcoded `false` and `ethLinkSpeedMbps()` returns a fake `1000` on desktop, which makes `PanelCardDriver`'s gigabit warning inert — the one safeguard against the real failure (a 100 Mbit link tears the panel with no error). Query `GetAdaptersAddresses` for `OperStatus` and `TransmitLinkSpeed` on the bound adapter. `iphlpapi` is a Windows system lib, same class as the `ws2_32` already linked.

## Files

- **`src/platform/desktop/platform_desktop.cpp`** — the whole change. A `_WIN32` branch in `ethBindRawInterface` (load wpcap, resolve symbols, enumerate, match, `pcap_open_live`) and in `ethSendRaw` (`pcap_sendpacket`), plus real `ethLinkUp`/`ethLinkSpeedMbps` for Windows. Existing POSIX branches unchanged; the capture fallback stays exactly as-is and remains what every test exercises.
- **`CMakeLists.txt:124`** — add `iphlpapi` beside `ws2_32` in the existing Windows generator expression. No wpcap link.
- **`src/light/drivers/PanelCardDriver.h`** — documentation only: the "Running this on a host" section says Linux and macOS; add Windows and the Npcap prerequisite. No behavior change, no control change (16 bytes stays sufficient because of the substring match).
- **`test/unit/light/unit_PanelCardDriver.cpp`** — one test that binding a name matching nothing fails cleanly and stays in capture mode. Existing tests are unaffected: none sets `interface`, so they never leave capture mode.

## Constraints to honor

- `ethSendRaw` is `MM_NONBLOCKING` and hot-path-checked transitively, so `pcap_sendpacket` enters that graph. It is a report, not a gate (`docs/metrics/hotpath-baseline.txt`), and the POSIX `sendto`/`write` are already there — note the new entry rather than suppress it.
- Every `#ifdef` stays inside `src/platform/`, per the boundary rule `check_platform_boundary.py` enforces.
- Degrade visibly: no Npcap, no match, or no permission each return `false` from the bind, which `PanelCardDriver::prepare` already turns into a Warning status while continuing to run.

## Verification

1. `cmake --build build/windows --config Release` — zero warnings; `ctest` still 1377/1377 (capture-mode tests must be untouched).
2. Run the desktop, add a `PanelCardDriver`, set `interface` to the adapter the card is on, and confirm the status line reports a real link speed instead of "no ethernet link".
3. **The card is the measurement**: a 128×128 layout, an effect running, and the panel lighting. That is the only proof the frames are correct on the wire — the unit tests pin the format, not the delivery.
4. Cross-platform regression is by inspection plus CI: the POSIX branches are not edited, and CI's Linux sanitizer jobs keep exercising them.

## Note on branching

This is a different feature from `moonlive-on-windows`, which has an open PR (#72) awaiting a bench run. Branch choice is the PO's call (CLAUDE.md § Branch); flagging it rather than assuming.
