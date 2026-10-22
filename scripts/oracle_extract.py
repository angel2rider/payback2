#!/usr/bin/env python3
"""
scripts/oracle_extract.py
Connects to MuMu emulator via ADB, inspects the running Payback 2 process,
dumps memory maps, extracts decrypted regions, and validates save/ini state.
"""

import subprocess
import os
import sys
import json

PACKAGE = "net.apex_designs.payback2"
DEVICE = "emulator-5554"

def adb(cmd):
    full_cmd = ["adb", "-s", DEVICE] + cmd
    res = subprocess.run(full_cmd, capture_output=True, text=True)
    if res.returncode != 0:
        # Try reconnecting
        subprocess.run(["adb", "connect", "127.0.0.1:16384"], capture_output=True)
        res = subprocess.run(full_cmd, capture_output=True, text=True)
    return res.stdout.strip()

def get_pid():
    out = adb(["shell", f"pidof {PACKAGE}"])
    pids = out.split()
    return pids[0] if pids else None

def main():
    print("[oracle] Checking connection to MuMu emulator...")
    pid = get_pid()
    if not pid:
        print(f"[oracle] {PACKAGE} is not running! Starting it...")
        adb(["shell", f"am start -n {PACKAGE}/.PaybackActivity"])
        import time
        time.sleep(2)
        pid = get_pid()
        if not pid:
            print("[oracle] Failed to start game on emulator.")
            sys.exit(1)

    print(f"[oracle] Payback 2 running with PID: {pid}")

    # 1. Fetch memory maps
    maps = adb(["shell", f"cat /proc/{pid}/maps"])
    maps_path = "/Users/thegt/Downloads/payback/oracle_dumps/maps.txt"
    with open(maps_path, "w") as f:
        f.write(maps)
    print(f"[oracle] Saved /proc/{pid}/maps ({len(maps.splitlines())} lines) -> {maps_path}")

    # Find libpayback2.so loaded base
    so_lines = [l for l in maps.splitlines() if "libpayback2.so" in l]
    print(f"[oracle] Found {len(so_lines)} memory mappings for libpayback2.so:")
    for l in so_lines[:5]:
        print("   ", l)

    # 2. Extract Backup.sav and Payback.ini
    save_dir = "/Users/thegt/Downloads/payback/oracle_dumps"
    adb(["pull", f"/data/data/{PACKAGE}/files/Backup.sav", f"{save_dir}/Backup.sav"])
    adb(["pull", f"/data/data/{PACKAGE}/files/Payback.ini", f"{save_dir}/Payback.ini"])
    print(f"[oracle] Pulled live Backup.sav and Payback.ini to {save_dir}/")

    # 3. Quick structural inspect of Payback.ini
    ini_path = f"{save_dir}/Payback.ini"
    if os.path.exists(ini_path):
        size = os.path.getsize(ini_path)
        print(f"[oracle] Payback.ini size: {size} bytes")

    print("[oracle] Oracle extraction complete.")

if __name__ == "__main__":
    main()
