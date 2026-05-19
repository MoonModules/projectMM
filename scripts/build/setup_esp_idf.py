#!/usr/bin/env python3
"""Check and set up ESP-IDF Python environment."""

import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from build_esp32 import find_idf, IDF_SEARCH_PATHS

def main():
    idf_path = find_idf()
    if not idf_path:
        print("ESP-IDF not found.")
        print("Install it from https://docs.espressif.com/projects/esp-idf/en/stable/esp32/get-started/")
        print("Searched: " + ", ".join(str(p) for p in IDF_SEARCH_PATHS))
        sys.exit(1)

    print(f"ESP-IDF found at {idf_path}")

    # Check version
    version_file = idf_path / "version.txt"
    if version_file.exists():
        print(f"Version: {version_file.read_text().strip()}")

    # Run install.sh to set up Python venv
    install_script = idf_path / "install.sh"
    if not install_script.exists():
        print(f"install.sh not found at {install_script}")
        sys.exit(1)

    print("Running ESP-IDF install (creates Python venv)...")
    r = subprocess.run([str(install_script), "esp32"],
                       cwd=str(idf_path))
    if r.returncode != 0:
        print("Install failed.")
        sys.exit(r.returncode)

    print("\nESP-IDF setup complete. You can now build for ESP32.")

if __name__ == "__main__":
    main()
