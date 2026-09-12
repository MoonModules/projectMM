# Running projectMM on a Linux machine

projectMM runs as an ordinary Linux application: the same effect pipeline, web UI and network drivers as on a board, with a real CPU behind them. A small always-on machine makes a good installation controller, whether a server, a Raspberry Pi or a NanoPi.

Deploying is covered here. Building and developing on Linux is in [building.md](../building.md).

> Windows, with screenshots: [Installing projectMM on a desktop](installing-to-desktop.md). Flashing a board: [Install & first light](../gettingstarted.md).

## Which route applies to your machine

Check the CPU first:

```sh
uname -m
```

| `uname -m` says | Machine | Route |
|---|---|---|
| `x86_64` | Intel or AMD PC, server or VM | Install the package |
| `aarch64` | arm64 board: Pi, NanoPi, most SBCs | Build from source |

The released Linux binaries are x86-64 only, so an arm64 board builds from source and ends up with the identical program. A Pi 4 or 5 has ample headroom. A NanoPi R28S has two Gigabit ports, so it can sit between the house network and the lighting network, and 1 GB of RAM, which runs projectMM comfortably and compiles it tightly (see the swap note in step 6). Both routes assume a Debian-based system (Debian, Ubuntu, Raspberry Pi OS, Armbian); on another distribution, translate the package names in step 5.

> `x64` and `amd64` are two names for the same thing. `arm64` is different machine code.

## x86-64: install the package

The [releases page](https://github.com/MoonModules/projectMM/releases/latest) carries `projectmm_X.Y.Z_amd64.deb`:

```sh
sudo apt install ./projectmm_X.Y.Z_amd64.deb
projectMM
```

Open `http://<machine>:8080`. A `.tar.gz` to unpack anywhere is on the same page.

## arm64: build from source

Allow an hour the first time, most of it waiting.

### 1. Write an OS image to the SD card

Take a Debian-based image. For a Raspberry Pi, [Raspberry Pi OS Lite](https://www.raspberrypi.com/software/): the desktop build leaves less memory for the compile. For a NanoPi, the Debian image from the board's [FriendlyELEC wiki page](https://wiki.friendlyelec.com/wiki/index.php/NanoPi_R28S#Flashing_the_OS_to_the_microSD_card), under `01_Official images/01_SD card images`. Skip FriendlyWrt: it is router firmware with a different package manager. [Armbian](https://www.armbian.com/download/) covers many boards from one project, if it lists yours.

Use an 8 GB card or larger. Write it with [Raspberry Pi Imager](https://www.raspberrypi.com/software/) or [balenaEtcher](https://etcher.balena.io/); both take the compressed download directly.

On a Raspberry Pi, open Imager's settings before writing: set the username, hostname and WiFi, and enable SSH. Raspberry Pi OS ships with SSH off and no default user, so a card written without those boots to a machine you cannot reach.

### 2. First boot

Insert the card, connect the network cable, power on. Give it 10 to 20 minutes: the first boot resizes the filesystem and may reboot itself.

Find it on the network:

```sh
ping raspberrypi.local          # or NanoPi-R28S.local
arp -a                          # everything the network has seen
```

Your router's client list is the fallback when mDNS does not resolve.

### 3. Log in

```sh
ssh <you>@<hostname>.local      # Raspberry Pi OS: the user you set in Imager
ssh pi@NanoPi-R28S              # FriendlyELEC Debian: user pi, password pi
```

Change a default password at once:

```sh
passwd
```

Without network access, attach a keyboard and monitor and configure it there: `sudo nmtui` for WiFi and addresses, `ip ad` to see what the board has. On a NanoPi, `sudo nmtui` also configures the second port.

### 4. Update the system

```sh
sudo apt update
sudo apt upgrade -y
```

### 5. Install the prerequisites

```sh
sudo apt install -y python3-pip cmake build-essential git
pip install uv --break-system-packages
```

`--break-system-packages` is routine on Debian 12 and later, where the system Python is marked externally managed; it affects pip's own environment only.

If `uv` is then not found, add its directory to your path:

```sh
echo 'export PATH="$HOME/.local/bin:$PATH"' >> ~/.bashrc && source ~/.bashrc
```

### 6. Build and run

```sh
git clone https://github.com/MoonModules/projectMM.git
cd projectMM
uv run moondeck/build/build_desktop.py
uv run moondeck/run/run_desktop.py
```

The build takes a few minutes on a Pi 4 or a NanoPi, longer on older boards. `run_desktop.py` detaches, so the program outlives the ssh session. Open `http://<board>:8080`.

That is a running system. Everything below is optional.

> A board with 1 GB of RAM or less can run out of memory while compiling; the compiler is killed rather than reporting an error. Add swap: edit `/etc/dphys-swapfile` to raise `CONF_SWAPSIZE`, then `sudo dphys-swapfile swapoff && sudo dphys-swapfile setup && sudo dphys-swapfile swapon`. Editing alone changes nothing; `setup` regenerates the file.

## Keeping it running after a reboot

Give it a systemd unit at `/etc/systemd/system/projectmm.service`:

```ini
[Unit]
Description=projectMM
After=network-online.target
Wants=network-online.target

[Service]
ExecStart=/home/pi/projectMM/build/projectMM
Restart=always
RestartSec=5
User=pi

[Install]
WantedBy=multi-user.target
```

```sh
sudo systemctl enable --now projectmm
systemctl status projectmm
```

`Restart=always` covers a crash as well as a reboot. Adjust `User` and the path to where you built.

## Shutting down

Shut down cleanly; an SD card interrupted mid-write can corrupt the filesystem:

```sh
sudo shutdown now     # or: sudo reboot
```

projectMM writes to disk only when settings change, so the card is a fine home for it. The risk is the operating system's own writes.

## Containers

Docker runs a full instance on anything with an amd64 kernel; the command is in the [README](https://github.com/MoonModules/projectMM#readme). A container shares the host kernel and runs native instructions, so an amd64 image needs an amd64 host. On an arm64 board, build from source as above.

## Where to go next

- [Install & first light](../gettingstarted.md): the same program on an ESP32.
- [How projectMM works](how-projectmm-works.md): layouts, layers, effects and drivers.
- [building.md](../building.md): building, testing and packaging in depth.
