#!/usr/bin/env python3
"""
build.py — build Quest Home Editor. If the .exe is locked by a running instance
(LNK1104), kill it (looping until none remain) and retry. Prints the FULL
compiler output — nothing trimmed.

Usage:
  python build.py          # build; auto-kill+retry only IF the exe is locked
  python build.py --kill   # kill any running instance FIRST, then build
"""
import os
import shutil
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
EXE  = "Quest Home Editor.exe"

def run_build():
    bat = os.path.join(HERE, "do_build.bat")
    return subprocess.run(["cmd", "/c", bat], cwd=HERE, capture_output=True, text=True)

def powershell():
    return shutil.which("pwsh") or shutil.which("powershell")

def count_instances():
    shell = powershell()
    if not shell:
        return -1
    r = subprocess.run([shell, "-NoProfile", "-Command",
        "(Get-Process 'Quest Home Editor' -ErrorAction SilentlyContinue | Measure-Object).Count"],
        capture_output=True, text=True)
    try:    return int((r.stdout or "0").strip().splitlines()[-1])
    except: return -1

def kill_instances():
    shell = powershell()
    if not shell:
        print("[build.py] WARNING: PowerShell not found; cannot stop a locked editor instance")
        return False
    for _ in range(6):
        if count_instances() == 0:
            return True
        subprocess.run([shell, "-NoProfile", "-Command",
            "Get-Process 'Quest Home Editor' -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue"],
            capture_output=True, text=True)
        time.sleep(0.4)
    n = count_instances()
    if n > 0:
        print(f"[build.py] WARNING: {n} Quest Home Editor instance(s) still alive after 6 kill attempts")
    return n == 0

def main():
    if ("--kill" in sys.argv) or ("-k" in sys.argv):
        print("[build.py] killing running instances first...")
        kill_instances()

    result = run_build()
    out = (result.stdout or "") + (result.stderr or "")
    if "LNK1104" in out and EXE in out:
        print(f"[build.py] {EXE} locked by a running instance -> killing it and retrying the link...")
        kill_instances()
        result = run_build()
        out = (result.stdout or "") + (result.stderr or "")

    # FULL build output, no trimming.
    sys.stdout.write(out)
    if not out.endswith("\n"):
        sys.stdout.write("\n")

    ok = result.returncode == 0
    print("[build.py] BUILD " + ("OK" if ok else "FAILED"))
    sys.exit(0 if ok else 1)

if __name__ == "__main__":
    main()
