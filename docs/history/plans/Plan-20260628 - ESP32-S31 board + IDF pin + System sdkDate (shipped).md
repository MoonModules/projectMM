# Plan — Add the ESP32-S31 board + pin the S31-capable ESP-IDF + System sdkDate

## Context

The product owner has a new **Espressif ESP32-S31 Function-CoreBoard-1** (RISC-V dual-core ≤320 MHz,
Wi-Fi 6, BT 5.4, 1 Gbps Ethernet, 16 MB flash, PSRAM) connected on `/dev/tty.usbserial-20213420`.
It currently runs Espressif's factory demo (`esp32_s31_function_coreboard_fa…`, built May 26 2026 on
IDF `v6.1-dev-5123`) — the cycling RGB LED. We want projectMM on it, in the web installer, and
buildable by any developer.

**Toolchain prerequisite (already done by the PO):** the S31 is a *preview* target that only exists
in IDF ≥ the v6.1 S31 merge. The PO has updated `~/esp/esp-idf` to the `release/v6.1` branch
(`0d9287800812c95662921c2c5e812023939e3d58`, `v6.1-dev-5215-g0d928780081`) and run
`install.sh esp32s31`. `idf.py --list-targets --preview` now lists `esp32s31`. The old project pin
(`d1b91b79…`, Nov 2025) predated this, so the pin must move forward — and a *new* developer running
MoonDeck "Setup ESP-IDF" must land on the same commit, which today the setup script only *warns*
about, never enforces.

**Three coupled deliverables, one combined commit (PO commits):**
1. **Move the IDF pin** to the S31-capable `release/v6.1` commit, and make MoonDeck's "Setup ESP-IDF"
   *offer to check it out* (not just warn) so a new dev converges on it.
2. **Add the `esp32s31` firmware** (all-in-one WiFi+Eth, `ships:True` → appears in the installer),
   build → flash → test on the bench (including a MoonLive control script — RISC-V, already proven
   on the P4).
3. **Add `sdkDate` to the System module** — the IDF/app build date (`esp_app_get_description()->date`),
   shown next to `sdk` in the System card.

S31 is RISC-V, so our existing `moonlive_lower_riscv` backend (verified live on the P4) applies
unchanged — the risk is in the *board* layer (Ethernet PHY, pin map, PSRAM mode), not our codegen.

## Decisions locked with the product owner

1. **Firmware name: `esp32s31`** (no suffix). The S31 has WiFi 6 **and** Ethernet on-chip, so one
   binary does both — exactly like the classic `esp32` default. A suffix (`-eth`/`-eth-wifi`) marks a
   *non-default* variant (the P4 case); the S31 all-in-one is the default, so no suffix. A WiFi-less
   `esp32s31-eth` sibling is deferred until a tester needs the smaller build.
2. **`ships:True`** — the PO wants S31 in the web installer, and `ships` is exactly what gates installer
   presence (`FIRMWARES` → `generate_firmwares.py` → `firmwares.json` → installer + CI matrix).
   Accepted trade-off: a preview-target breakage turns PR CI red until fixed or flipped to `ships:False`
   (a one-line, reversible change). The PO chose to try it in CI.
3. **Re-verify the existing targets on the new IDF *first*** (before adding S31). The pin jumped 4816
   commits (Nov 2025 → Jun 2026); a drift regression in classic/S3/P4 must surface locally, not in CI.
4. **`sdkDate` = the IDF/app build date** (`esp_app_get_description()->date`, e.g. "May 26 2026"), not
   our firmware's `__DATE__`. Confirmed cheap: a one-line IDF call available in the new IDF
   (`esp_app_desc_t.date`), so it's worth doing. (PO: "the second, but not if it is hard" — it's easy.)
5. **MoonDeck setup converges a new dev on the pin.** Today `setup_esp_idf.py` only *warns* on drift.
   Make it *offer* to `git checkout <PINNED_IDF_COMMIT>` (prompt, default-yes; `--no-checkout` to keep
   the warn-only behaviour for a dev deliberately migrating). It still can't clone, but it can move an
   existing checkout onto the pin — closing the "new dev gets the wrong commit" gap.

## Part A — Move the IDF pin + MoonDeck convergence

The pin is named in several files (from the inventory); update **all** so they agree:

- **`scripts/build/setup_esp_idf.py`** — `PINNED_IDF_COMMIT` → `0d9287800812c95662921c2c5e812023939e3d58`,
  `PINNED_IDF_VERSION` → `v6.1-dev-5215-g0d928780081`. **Plus the convergence change:** after the drift
  warning, prompt "Check out the pinned commit now? [Y/n]" and run `git checkout <PINNED_IDF_COMMIT>` +
  `git submodule update --init --recursive` when accepted. Add a `--no-checkout` flag to opt out (the
  current warn-only path, for a dev mid-migration). `install.sh esp32` stays `esp32` (the S31 toolchain
  is fetched separately; a dev who builds S31 runs `install.sh esp32s31` once — note this in the doc).
- **`.github/workflows/release.yml`** — `esp_idf_version: v6.1-dev` → `release-v6.1` (the stable Docker
  tag built from the same `release/v6.1` branch, confirmed to exist on `espressif/idf`); cache key
  `esp-idf-v6.1-dev-…` → `esp-idf-release-v6.1-…`; update the explanatory comments (present-tense, name
  the new commit).
- **`docs/building.md`** — the clone instructions (`--branch v6.1-dev` → `--branch release/v6.1`), the
  "Tested IDF version" line, the pinned-commit line, and the "moving to a different release" procedure.
  Add a one-liner that S31 needs `install.sh esp32s31`.
- **`scripts/MoonDeck.md`** — the `setup_esp_idf` section: document the new checkout-offer behaviour +
  the `install.sh esp32s31` note for S31 builders.

**Re-verify gate (do this before Part B):** with the new IDF active, build the existing targets
`esp32`, `esp32s3-n16r8`, `esp32p4-eth` (the three the local gate already builds) and confirm
zero-warning green. A failure here is an IDF-drift regression to fix *before* adding S31.

## Part B — Add the `esp32s31` firmware

Follow the established new-firmware pattern (inventory mapped every touch point). The S31 is a
*preview* target with possibly-incomplete/renamed Kconfig symbols, so the sdkconfig fragment is
**derived empirically at implementation time** (`idf.py set-target esp32s31` then `menuconfig` to find
the real PSRAM-mode / flash / EMAC symbol names), starting from the P4 (RISC-V) fragment as the closest
analog. Touch points:

- **`scripts/build/build_esp32.py`** — add the `FIRMWARES["esp32s31"]` entry: `"chip": "esp32s31"`,
  `"fragments": ["sdkconfig.defaults", "sdkconfig.defaults.esp32s31"]`, `"eth_only": False`,
  `"ships": True`, a one-line description. Add `"esp32s31": "ESP32-S31"` to `TARGET_TO_FAMILY` (line
  153). Confirm `set-target` (line 505) passes the chip through; the preview target may need
  `IDF_TARGET` set or a preview opt-in — handle in the build invocation if `set-target esp32s31` warns.
- **`esp32/sdkconfig.defaults.esp32s31`** (new) — modeled on `sdkconfig.defaults.esp32p4-eth`: 16 MB
  flash (`CONFIG_ESPTOOLPY_FLASHSIZE_16MB=y`), PSRAM (mode per the S31's controller — verify symbol),
  the `partitions/ota_16mb.csv` custom partition (reuse — it's chip-independent), on-chip EMAC if the
  board exposes Ethernet (verify the S31 has `CONFIG_ETH_USE_ESP32_EMAC` or the S31 equivalent), and
  **the MoonLive exec-heap pair** (`CONFIG_ESP_SYSTEM_MEMPROT_FEATURE=n` + `CONFIG_HEAP_HAS_EXEC_HEAP=y`)
  — same JIT requirement as every other target; verify the symbol names exist on S31.
- **`esp32/partitions/ota_16mb.csv`** — reuse as-is (the existing shared 16 MB layout; no new file).
- **Ethernet pins** — per the platform pattern, RMII/PHY pins live in C
  (`src/platform/esp32/platform_config.h` `ethPins` struct), not sdkconfig. Add the S31 board's pin map
  there once the board's schematic/PHY is confirmed (read from the Espressif S31 CoreBoard docs at
  implementation time). If the board's Ethernet is wired like an existing target this is a small struct
  add; otherwise it's a board-specific entry. The firmware still builds without eth wired (WiFi-first).
- **`docs/install/deviceModels.json`** — add the board entry: `name` "Espressif ESP32-S31
  Function-CoreBoard-1", `chip` "ESP32-S31", `firmwares: ["esp32s31"]`, an `image`
  (`assets/boards/…` — needs a board photo added), `supported: ["LEDs","WiFi","Ethernet"]`, the System
  `deviceModel` control = the entry name, an LED-driver module with `pins`, and a NetworkModule with the
  S31 Ethernet config (mirroring the P4 entry's `ethType`/`ethPhyAddr`/`eth*Gpio`). Passes
  `check_devices.py`.
- **Regenerate the projection** — `uv run scripts/build/generate_firmwares.py --out
  docs/install/firmwares.json` (so `esp32s31` lands in the installer-read JSON; `check_firmwares.py`
  guards drift). `generate_manifest.py` derives chip family from `TARGET_TO_FAMILY` automatically — no
  change.

## Part C — System `sdkDate` (IDF/app build date)

Mirror the existing `sdkVersion()` platform-helper pattern exactly (a domain-neutral platform call, no
IDF outside `src/platform/`):

- **`src/platform/platform.h`** — declare `const char* sdkDate();` next to `sdkVersion()` (line 70).
- **`src/platform/esp32/platform_esp32.cpp`** — `const char* sdkDate() { return
  esp_app_get_description()->date; }` (the app descriptor's compile date; API confirmed present in the
  new IDF — `esp_app_desc.h:33`).
- **`src/platform/desktop/platform_desktop.cpp`** — `const char* sdkDate() { return __DATE__; }` (the
  desktop stub, mirroring how `sdkVersion()` returns the compiler there).
- **`src/core/SystemModule.h`** — add `char sdkDateInfo_[16] = {};` (next to `sdkInfo_`), populate in
  `setup()` (`std::snprintf(sdkDateInfo_, sizeof(sdkDateInfo_), "%s", platform::sdkDate());`), and add
  `controls_.addReadOnly("sdkDate", sdkDateInfo_, sizeof(sdkDateInfo_));` right after the `sdk` control
  (line 120).
- **`docs/moonmodules/core/SystemModule.md`** — add `sdkDate` to the "Static (set at boot)" list (the
  `check_specs.py` gate requires every control name appear in the spec).

## Files (summary)

- IDF pin + convergence: `scripts/build/setup_esp_idf.py`, `.github/workflows/release.yml`,
  `docs/building.md`, `scripts/MoonDeck.md`.
- S31 firmware: `scripts/build/build_esp32.py`, `esp32/sdkconfig.defaults.esp32s31` (new),
  `src/platform/esp32/platform_config.h` (S31 eth pins), `docs/install/deviceModels.json`,
  `docs/install/firmwares.json` (regenerated), `assets/boards/<s31 photo>` (new).
- System sdkDate: `src/platform/platform.h`, `src/platform/{esp32,desktop}/platform_{esp32,desktop}.cpp`,
  `src/core/SystemModule.h`, `docs/moonmodules/core/SystemModule.md`.

## Riskiest parts

1. **S31 is a *preview* target.** Kconfig symbol names (PSRAM mode, EMAC, flash) may differ from S3/P4
   or be incomplete; derive the fragment via `menuconfig` against the real target rather than copying P4
   blindly. If a symbol is missing, the build warns/errors — fix at the fragment, don't suppress.
2. **The Ethernet PHY + pin map** for the S31 CoreBoard is board-specific and not yet confirmed; read
   the Espressif S31 CoreBoard schematic/docs before filling the `ethPins` struct + the deviceModels
   NetworkModule config. If Ethernet is flaky, ship LEDs+WiFi first and add Eth once the PHY is confirmed.
3. **The 4816-commit IDF jump** could regress the existing targets — hence the Part-A re-verify gate
   before touching S31.
4. **CI on a preview target** can go red as IDF preview support churns; the reversible `ships:False`
   escape hatch is one line.

## Verification

- **Part A:** `uv run scripts/build/setup_esp_idf.py` shows no drift warning (installed == new pin);
  the checkout-offer path moves a deliberately-stale checkout onto the pin. Re-verify gate: `esp32`,
  `esp32s3-n16r8`, `esp32p4-eth` build zero-warning green on the new IDF.
- **Part B:** `uv run scripts/build/build_esp32.py --firmware esp32s31` builds clean →
  `flash_esp32.py` to `/dev/tty.usbserial-20213420` → device boots, joins WiFi/Eth, serves the UI.
  Author a MoonLive control script (`uint8_t speed = 50; // @control 0..99  setRGB(speed,0,0,255);`),
  confirm the slider appears + moves the pixel live with no recompile (RISC-V codegen, P4-proven).
  `check_devices.py` + `check_firmwares.py` green; `esp32s31` appears in the local installer preview.
- **Part C:** desktop build shows `sdkDate` in the System card (compiler date); on the S31, `sdkDate`
  shows the IDF app build date next to `sdk`. `check_specs.py` green.
- **Full gate set** (build/unit/scenario/ESP32/KPI + the device-catalog/firmware checks) green before
  the PO's single combined commit. Save this plan to `docs/history/plans/Plan-20260628 - ESP32-S31
  board + IDF pin + System sdkDate.md` as the first implementation step.

## Out of scope (deferred)

- `esp32s31-eth` (WiFi-less) sibling — add when a tester needs the smaller build.
- Auto-*cloning* the IDF in `setup_esp_idf.py` (it converges an existing checkout but still won't clone
  — the dev does the initial clone per `docs/building.md`).
- S31 BT 5.4 / 802.15.4 / Thread-Zigbee features — projectMM doesn't use them.
