#!/usr/bin/env python3
"""
build.py — build hsr_renderer. If the .exe is locked by a running instance
(LNK1104), kill it (looping until none remain) and retry. Prints the FULL
compiler output — nothing trimmed.

Usage:
  python build.py          # build; auto-kill+retry only IF the exe is locked
  python build.py --kill   # kill any running instance FIRST, then build
"""
import subprocess, sys, os, time

HERE = os.path.dirname(os.path.abspath(__file__))
EXE  = "hsr_renderer.exe"

def run_build():
    bat = os.path.join(HERE, "do_build.bat")
    r = subprocess.run(["cmd", "/c", bat], cwd=HERE, capture_output=True, text=True)
    return (r.stdout or "") + (r.stderr or "")

def count_instances():
    r = subprocess.run(["powershell", "-NoProfile", "-Command",
        "(Get-Process hsr_renderer -ErrorAction SilentlyContinue | Measure-Object).Count"],
        capture_output=True, text=True)
    try:    return int((r.stdout or "0").strip().splitlines()[-1])
    except: return -1

def kill_instances():
    for _ in range(6):
        if count_instances() == 0:
            return True
        subprocess.run(["powershell", "-NoProfile", "-Command",
            "Get-Process hsr_renderer -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue"],
            capture_output=True, text=True)
        time.sleep(0.4)
    n = count_instances()
    if n > 0:
        print(f"[build.py] WARNING: {n} hsr_renderer instance(s) still alive after 6 kill attempts")
    return n == 0

def main():
    if ("--kill" in sys.argv) or ("-k" in sys.argv):
        print("[build.py] killing running instances first...")
        kill_instances()

    out = run_build()
    if "LNK1104" in out and EXE in out:
        print(f"[build.py] {EXE} locked by a running instance -> killing it and retrying the link...")
        kill_instances()
        out = run_build()

    # FULL build output, no trimming.
    sys.stdout.write(out)
    if not out.endswith("\n"):
        sys.stdout.write("\n")

    ok = ("NINJA_EXIT=0" in out) and ("error LNK" not in out) and (": error" not in out) and ("FAILED:" not in out)
    print("[build.py] BUILD " + ("OK" if ok else "FAILED"))
    sys.exit(0 if ok else 1)

if __name__ == "__main__":
    main()
