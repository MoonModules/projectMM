#!/usr/bin/env python3
"""Read the host machine's currently-joined WiFi network — SSID + password.

Cross-platform: macOS / Linux / Windows. Standalone — only `subprocess` and
`shutil` from the stdlib; no extra dependencies. Used by
`scripts/build/improv_provision.py` (so a one-click MoonDeck button can
push the host's credentials to a fresh ESP32 over USB-serial) and is
re-runnable on its own with `python3 host_wifi.py` for diagnosis.

Per-platform notes:

  macOS — `networksetup -getairportnetwork <iface>` for the SSID,
          `security find-generic-password -wa <ssid>` for the password.
          The latter triggers a Keychain access prompt the first time
          per session (TouchID / login password), which is the OS doing
          its job; we don't try to bypass it.

  Linux — `nmcli --terse --fields active,ssid dev wifi` (NetworkManager;
          ships with every common desktop distro). Password from
          `nmcli --show-secrets connection show <ssid> | grep
          802-11-wireless-security.psk`. Requires sudo on most distros
          (NetworkManager stores secrets in /etc/NetworkManager/
          system-connections/*.nmconnection, root-owned). If sudo
          isn't available we still return the SSID — the caller can
          prompt for the password.

  Windows — `netsh wlan show interfaces` for the SSID,
          `netsh wlan show profile name="<ssid>" key=clear` for the
          password. No prompt; the password is in plaintext in the
          profile XML and is recovered without privilege escalation.

Returns the pair `(ssid, password)`; either field can be `None` if the
host isn't joined to a network or the password couldn't be recovered
(open networks, sudo refused, Keychain locked, etc.). The caller is
expected to fall back to a prompt in that case.

If you need to test detection without joining a network: run with
`--ssid override` to skip detection entirely.
"""

import shutil
import subprocess
import sys
from typing import Optional, Tuple


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


def _macos_ssid(iface: str) -> Optional[str]:
    try:
        out = subprocess.check_output(
            ["/usr/sbin/networksetup", "-getairportnetwork", iface],
            text=True,
            stderr=subprocess.DEVNULL,
        )
    except (subprocess.CalledProcessError, FileNotFoundError):
        return None
    # Output: "Current Wi-Fi Network: <ssid>"  OR  "You are not associated ..."
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
    iface = _macos_wifi_interface()
    if not iface:
        return None, None
    ssid = _macos_ssid(iface)
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

def get_host_wifi() -> Tuple[Optional[str], Optional[str]]:
    """Return `(ssid, password)` for the host's currently-joined WiFi.

    Either field may be None. Never raises — detection failures are normal
    (no WiFi adapter, not joined, locked Keychain, no nmcli, etc.). The
    caller is expected to treat None as "fall back to a prompt."
    """
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
