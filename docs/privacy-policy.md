# Privacy policy

**Last updated: 2026-09-10**

Covers the projectMM software (firmware, desktop application and the web interface they serve), the [web installer](https://moonmodules.org/projectMM/install/), and this documentation site.

## The rule

**projectMM sends nothing to us unless you switch it on.** Every feature that transmits anything is opt-in, off by default, and asks in plain words before its first transmission. Declining is one click, is remembered, and sends nothing at all: not even a record that you declined.

Everything else stays on the machine you run it on: your layouts, effects, drivers, pin assignments, device name, and any credentials you entered. `%LOCALAPPDATA%\projectMM` on Windows, `~/Library/Application Support/projectMM` on macOS, `$XDG_DATA_HOME/projectMM` on Linux, the device's own flash on a board. Nothing there is uploaded, synchronized or backed up by us.

## What we never collect

Whatever you switch on, projectMM does not transmit **your name, email or postal address, your Wi-Fi or MQTT credentials, your IP or MAC address, or the contents of files you made**.

## What you can switch on

Each of these is a separate choice with its own setting. Turning one on says nothing about the others.

### MoonCloud Stats

One report when the firmware is installed or upgraded, describing hardware and configuration: chip family, flash and PSRAM size, SDK version, board model, which modules are enabled, whether this is an install or an upgrade, the version running and the one it replaced, and whether the firmware was built locally rather than released. Plus an installation id (below) and a country derived from the connection.

The report carries no device name, no addresses, no credentials and no text you typed. A unit test asserts those cannot appear in it.

### MoonCloud Talk

A public message board between devices. Messages you send are readable by anyone, permanently, and cannot be withdrawn. Sharing your device name alongside them is a **separate setting, off by default**; without it your messages show the first 8 characters of your installation id.

Reading the board transmits nothing about your device.

## The installation id

The one part of a MoonCloud transmission that is about you rather than your hardware.

It is a SHA-256 hash of an internal address together with a fixed salt. On a device that address is the chip's MAC; on a desktop or in Docker it is a random value generated on first run and stored with your configuration, derived from nothing about your machine. The address itself is never sent, and the salt is unique to MoonCloud, so the id cannot be matched against the device name or MQTT topics that same address produces on your own network.

**We call it pseudonymous rather than anonymous.** It is the same each time, so reports carrying it came from one installation. That is deliberate: it is what distinguishes an upgrade from a new install. It also means the id is personal data under the GDPR, and **your consent is the lawful basis**: nothing is generated, stored or sent until you answer yes.

What follows from it being stable:

- On a device it survives a factory reset and a reflash. Our salt is published in our source code and a MAC address is a short number, so someone determined could work backwards from an id to a MAC. We do not do this and store nothing that would help, but we will not claim it is impossible.
- On a desktop or in Docker it cannot be worked backwards to anything. Deleting your configuration folder gives you a new one.
- It is kept indefinitely, alongside the aggregates.
- **A transmission already sent cannot practically be withdrawn**, because finding it would mean you supplying your id. To ask anyway, use the contact details below.

## The IP address

Any server receiving a request sees the address it came from. Ours derives a country from it at the network edge and **never writes the address to storage**. What is stored is a country name beside a chip family and a version number.

## What reaches the internet without you switching anything on

Four things, each either something you asked for or something your browser does, and none of them reach a server of ours:

- **The update check**: your browser asks GitHub's public release API whether a newer version exists.
- **Downloading a MoonLive script**: your browser fetches it from `raw.githubusercontent.com`.
- **A firmware update**: your device downloads it from GitHub, when you start one.
- **The web installer**: served by GitHub Pages; flashing happens over USB, directly between your browser and the board.

These go to GitHub under [its privacy statement](https://docs.github.com/en/site-policy/privacy-policies/github-general-privacy-statement). This documentation site and the release downloads are hosted by GitHub too, which receives the requests your browser makes, including your IP address. We add no analytics, no cookies of our own and no tracking.

## Your browser's local storage

The web interface stores a few conveniences in your browser: a cached update-check result, the release you last selected. This never leaves your browser and holds nothing personal.

## Systems you connect projectMM to

An MQTT broker, Home Assistant, Art-Net or E1.31 consoles: projectMM speaks to these when you configure it to, and those connections go where you point them. **Whatever you connect it to is governed by that service's own privacy policy**, which is worth knowing if you point it at a cloud-hosted broker.

## Changes to this policy

**A new feature that transmits anything is documented here before it ships, and asks for its own consent.** We may later offer to collect things this policy does not describe today, such as which effects are used or automatic crash reports; if we do, each will be a separate opt-in choice, described here first, and switched off until you turn it on.

Revisions are made in the open: this page lives in the [project repository](https://github.com/MoonModules/projectMM/blob/main/docs/privacy-policy.md), so every change is a commit you can read.

## Contact

Questions, or anything here you would like to verify: an [issue on the repository](https://github.com/MoonModules/projectMM/issues) or the [Discord](https://discord.gg/TC8NSUSCdV). projectMM is free and open-source software, and the network calls described above are the only ones in it.
