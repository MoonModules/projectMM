#!/usr/bin/env python3
"""Read the host machine's currently-joined WiFi network — SSID + password.

Resolution order: local config file → OS auto-detect. The local file wins
because OS auto-detect is structurally unreliable across platforms (see
per-platform notes below); a checked file is a checked-in, predictable
fallback the user owns.

Cross-platform: macOS / Linux / Windows. Standalone — only stdlib (subprocess,
shutil, json, pathlib); no extra dependencies. Used by
`scripts/build/improv_provision.py` so a one-click MoonDeck button can
push the host's credentials to a fresh ESP32 over USB-serial; also
re-runnable on its own with `python3 host_wifi.py` for diagnosis.

Local file path: `scripts/build/wifi_credentials.json` (gitignored).
Schema is the same as `wifi_credentials.example.json` (committed):
`{"ssid": "...", "password": "..."}`. Copy the example file (drop the
`.example` suffix) and fill in.

Per-platform auto-detect notes (when the local file isn't present):

  macOS — `system_profiler SPAirPortDataType` is the modern path, but on
          Sonoma+ Apple Silicon non-sudo invocations get the SSID returned
          as the literal string "<redacted>" for privacy reasons. We detect
          that and treat it as not-found. Older macOS fell back to
          `networksetup -getairportnetwork`, which is unreliable on the same
          modern releases (returns "You are not associated" on a connected
          interface). When both fail, the local file is the resort.
          Password via `security find-generic-password -wa <ssid>`; pops a
          Keychain access prompt the first time per session.

  Linux — `nmcli --terse --fields active,ssid dev wifi` (NetworkManager;
          ships with every common desktop distro). Password from
          `nmcli --show-secrets connection show <ssid> | grep
          802-11-wireless-security.psk`. Requires sudo on most distros
          (NetworkManager stores secrets in /etc/NetworkManager/
          system-connections/*.nmconnection, root-owned). If sudo isn't
          available we still return the SSID — the caller can prompt for
          the password, or read the local file.

  Windows — `netsh wlan show interfaces` for the SSID, `netsh wlan show
          profile name="<ssid>" key=clear` for the password. Works without
          admin in most cases; sometimes localised strings interfere.

Returns the pair `(ssid, password)`; either field can be `None`.
"""

import json
import shutil
import subprocess
import sys
from pathlib import Path
from typing import Optional, Tuple

LOCAL_CREDENTIALS_FILE = Path(__file__).resolve().parent / "wifi_credentials.json"


# ---------------------------------------------------------------------------
# macOS
# ---------------------------------------------------------------------------

def _macos_wifi_interface() -> Optional[str]:
    """Find the Wi-Fi hardware port's interface name (e.g. 'en0').

    `networksetup -listallhardwareports` prints blocks like:
        Hardware Port: Wi-Fi
        Device: en0
        Ethernet Address: ...
    """
    try:
        out = subprocess.check_output(
            ["/usr/sbin/networksetup", "-listallhardwareports"],
            text=True,
            stderr=subprocess.DEVNULL,
        )
    except (subprocess.CalledProcessError, FileNotFoundError):
        return None
    wifi_seen = False
    for line in out.splitlines():
        if line.startswith("Hardware Port:"):
            wifi_seen = "Wi-Fi" in line or "AirPort" in line
        elif wifi_seen and line.startswith("Device:"):
            return line.split(":", 1)[1].strip()
    return None


def _macos_ssid_via_system_profiler() -> Optional[str]:
    """Read the current SSID from `system_profiler SPAirPortDataType`.

    The modern, sudoless macOS path. Works on Apple Silicon Sonoma+ where
    `networksetup -getairportnetwork` returns "You are not associated"
    even on a connected interface (Apple deprecated that codepath in
    practice, though it still exists). Output structure:

        Current Network Information:
          <SSID>:
            PHY Mode: 802.11ax
            ...
            Network Type: Infrastructure

    We look for the line after `Current Network Information:` — first
    non-empty line ending in ':' is the SSID line. There can be multiple
    `Current Network Information:` blocks (AWDL, hotspot, etc.); we take
    the first one that has an SSID line under it.
    """
    try:
        out = subprocess.check_output(
            ["/usr/sbin/system_profiler", "SPAirPortDataType"],
            text=True,
            stderr=subprocess.DEVNULL,
            timeout=10,
        )
    except (subprocess.CalledProcessError, FileNotFoundError, subprocess.TimeoutExpired):
        return None

    lines = out.splitlines()
    for i, line in enumerate(lines):
        if "Current Network Information:" not in line:
            continue
        # Walk forward looking for the SSID line — first non-empty line that
        # ends with ':' and isn't itself the header we just matched.
        for next_line in lines[i + 1:i + 4]:
            stripped = next_line.strip()
            if not stripped:
                continue
            if stripped.endswith(":") and "Network Information" not in stripped:
                ssid = stripped[:-1]  # strip trailing ':'
                # macOS Sonoma+ redacts SSIDs from non-sudo subprocesses for
                # privacy; the literal string `<redacted>` comes back. Treat
                # that as not-found so the caller falls through to the local
                # file path rather than pushing "<redacted>" as a real SSID.
                if ssid == "<redacted>":
                    return None
                return ssid
            # If we hit "Network Type:" or similar before finding an SSID
            # line, this block has no SSID (probably an unjoined entry).
            if ":" in stripped:
                break
    return None


def _macos_ssid_via_networksetup(iface: str) -> Optional[str]:
    """Legacy fallback for older macOS / Intel Macs where networksetup works.

    Kept as a fallback because system_profiler can take 1-2 s and networksetup
    is faster when it works. On modern Apple Silicon Sonoma+ networksetup
    tends to return "not associated" even when WiFi is up — the bug this
    detector worked around when host_wifi.py was first written.
    """
    try:
        out = subprocess.check_output(
            ["/usr/sbin/networksetup", "-getairportnetwork", iface],
            text=True,
            stderr=subprocess.DEVNULL,
        )
    except (subprocess.CalledProcessError, FileNotFoundError):
        return None
    for line in out.splitlines():
        if ":" in line and "Network" in line:
            value = line.split(":", 1)[1].strip()
            if value and "not associated" not in value.lower():
                return value
    return None


def _macos_password(ssid: str) -> Optional[str]:
    # `security find-generic-password -wa <name>` returns the password to
    # stdout. The first call per session pops a Keychain access dialog —
    # that's the OS's policy, not something to circumvent.
    try:
        out = subprocess.check_output(
            ["/usr/bin/security", "find-generic-password", "-wa", ssid],
            text=True,
            stderr=subprocess.DEVNULL,
        )
        return out.strip() or None
    except (subprocess.CalledProcessError, FileNotFoundError):
        return None


def _macos_credentials() -> Tuple[Optional[str], Optional[str]]:
    # Try system_profiler first — works on modern Apple Silicon macOS where
    # networksetup is unreliable (was returning "not associated" on a
    # connected interface on Sonoma+). Fall back to networksetup for the
    # rare environments where system_profiler is locked down (corporate-
    # managed Macs) or older macOS where networksetup still works.
    ssid = _macos_ssid_via_system_profiler()
    if not ssid:
        iface = _macos_wifi_interface()
        if iface:
            ssid = _macos_ssid_via_networksetup(iface)
    if not ssid:
        return None, None
    return ssid, _macos_password(ssid)


# ---------------------------------------------------------------------------
# Linux — NetworkManager (`nmcli`)
# ---------------------------------------------------------------------------

def _linux_ssid_via_nmcli() -> Optional[str]:
    if shutil.which("nmcli") is None:
        return None
    try:
        out = subprocess.check_output(
            ["nmcli", "--terse", "--fields", "active,ssid", "dev", "wifi"],
            text=True,
            stderr=subprocess.DEVNULL,
        )
    except (subprocess.CalledProcessError, FileNotFoundError):
        return None
    # Output lines: "yes:MyAP" or "no:Other". We want the active one.
    for line in out.splitlines():
        if line.startswith("yes:"):
            return line.split(":", 1)[1] or None
    return None


def _linux_password_via_nmcli(ssid: str) -> Optional[str]:
    """Read the PSK out of NetworkManager. Needs root on most distros.

    Two paths tried in order:
      1. `nmcli --show-secrets connection show <ssid>` — works as root,
         polkit-authenticated user, or wherever the secret is stored with
         agent-owned permission.
      2. Direct read of `/etc/NetworkManager/system-connections/<ssid>.nmconnection`
         if we have read access (some distros use 0644 for non-secret
         profiles; secrets-bearing files are 0600 root-owned).
    Returns None if neither works — the caller falls back to prompting.
    """
    try:
        out = subprocess.check_output(
            ["nmcli", "--show-secrets", "--terse",
             "--fields", "802-11-wireless-security.psk",
             "connection", "show", ssid],
            text=True,
            stderr=subprocess.DEVNULL,
        )
        # Output: "802-11-wireless-security.psk:hunter2"
        for line in out.splitlines():
            if ":" in line:
                value = line.split(":", 1)[1]
                if value:
                    return value
    except (subprocess.CalledProcessError, FileNotFoundError):
        pass

    # File-system fallback (rare; mostly useful in containers / WSL where
    # the user owns NM's config directory).
    try:
        with open(f"/etc/NetworkManager/system-connections/{ssid}.nmconnection") as f:
            for line in f:
                if line.strip().startswith("psk="):
                    return line.split("=", 1)[1].strip() or None
    except (FileNotFoundError, PermissionError, OSError):
        pass
    return None


def _linux_credentials() -> Tuple[Optional[str], Optional[str]]:
    ssid = _linux_ssid_via_nmcli()
    if not ssid:
        return None, None
    return ssid, _linux_password_via_nmcli(ssid)


# ---------------------------------------------------------------------------
# Windows — `netsh`
# ---------------------------------------------------------------------------

def _windows_ssid() -> Optional[str]:
    try:
        out = subprocess.check_output(
            ["netsh", "wlan", "show", "interfaces"],
            text=True,
            stderr=subprocess.DEVNULL,
        )
    except (subprocess.CalledProcessError, FileNotFoundError):
        return None
    # netsh prints localised output; we look for the SSID field non-case-
    # sensitively and skip BSSID lines (which start with " BSSID").
    for line in out.splitlines():
        stripped = line.strip()
        if stripped.lower().startswith("ssid") and not stripped.lower().startswith("bssid"):
            if ":" in stripped:
                value = stripped.split(":", 1)[1].strip()
                if value:
                    return value
    return None


def _windows_password(ssid: str) -> Optional[str]:
    try:
        out = subprocess.check_output(
            ["netsh", "wlan", "show", "profile",
             f"name={ssid}", "key=clear"],
            text=True,
            stderr=subprocess.DEVNULL,
        )
    except (subprocess.CalledProcessError, FileNotFoundError):
        return None
    # Look for "Key Content" — localised systems may use a translated label;
    # the english one covers all en-* locales and the common deployments.
    for line in out.splitlines():
        if "Key Content" in line and ":" in line:
            return line.split(":", 1)[1].strip() or None
    return None


def _windows_credentials() -> Tuple[Optional[str], Optional[str]]:
    ssid = _windows_ssid()
    if not ssid:
        return None, None
    return ssid, _windows_password(ssid)


# ---------------------------------------------------------------------------
# Public entry point
# ---------------------------------------------------------------------------

def _local_file_credentials() -> Tuple[Optional[str], Optional[str]]:
    """Read SSID + password from scripts/build/wifi_credentials.json.

    This is the gitignored fallback the user maintains themselves. Schema:
    `{"ssid": "...", "password": "..."}`. Used when OS auto-detect fails
    (modern macOS redacts SSIDs from non-sudo subprocesses; Linux NM needs
    sudo for the PSK in many distros) — and as a deliberate override for
    users who don't want the auto-detect path at all.
    """
    if not LOCAL_CREDENTIALS_FILE.exists():
        return None, None
    try:
        data = json.loads(LOCAL_CREDENTIALS_FILE.read_text())
    except (json.JSONDecodeError, OSError):
        return None, None
    ssid = data.get("ssid") or None
    password = data.get("password") or None
    return ssid, password


def get_host_wifi() -> Tuple[Optional[str], Optional[str]]:
    """Return `(ssid, password)` for the host's currently-joined WiFi.

    Resolution order:
      1. `scripts/build/wifi_credentials.local.json` if present (gitignored
         user-maintained override; primary path on macOS Sonoma+ where OS
         detection is structurally limited).
      2. OS auto-detect (macOS / Linux / Windows).

    Either field may be None. Never raises — detection failures are normal
    (no WiFi adapter, not joined, locked Keychain, no nmcli, etc.). The
    caller is expected to treat None as "fall back to a prompt."
    """
    # Local file first. If it exists and gives a real SSID, use it.
    ssid, password = _local_file_credentials()
    if ssid:
        return ssid, password

    # OS auto-detect path.
    if sys.platform == "darwin":
        return _macos_credentials()
    if sys.platform.startswith("linux"):
        return _linux_credentials()
    if sys.platform.startswith("win"):
        return _windows_credentials()
    return None, None


def main() -> int:
    """Print detected SSID + password to stdout. Useful for diagnosing
    why a MoonDeck-launched provision job didn't autofill credentials."""
    ssid, password = get_host_wifi()
    if ssid is None:
        print("ssid: <not detected>")
    else:
        print(f"ssid: {ssid}")
    if password is None:
        print("password: <not detected>")
    else:
        # Print the password verbatim — this script is meant to be run by
        # the host user on the host machine. Anyone with shell access here
        # already has Keychain / NM secret access by other paths.
        print(f"password: {password}")
    return 0 if ssid else 1


if __name__ == "__main__":
    sys.exit(main())
