#!/usr/bin/env python3
"""Attach frida to the V79 env renderer (vrshell) and run the anim-capture hook for N seconds."""
import frida, sys, time, os

PID = int(sys.argv[1]) if len(sys.argv) > 1 else 20748          # vrshell
SECS = int(sys.argv[2]) if len(sys.argv) > 2 else 15
SCRIPT = sys.argv[3] if len(sys.argv) > 3 else "frida_v79_anim_capture.js"
HERE = os.path.dirname(os.path.abspath(__file__))

dev = frida.get_usb_device(timeout=10)
print(f"[frida_run] device={dev.name} attaching pid={PID} for {SECS}s")
session = dev.attach(PID)
src = open(os.path.join(HERE, SCRIPT), "r", encoding="utf-8").read()
script = session.create_script(src)

def on_msg(message, data):
    if message.get("type") == "log":
        print(message["payload"])
    elif message.get("type") == "send":
        p = message.get("payload", {})
        if isinstance(p, dict) and p.get("kind") == "shader":
            d = os.path.join(HERE, "v203_shaders")
            os.makedirs(d, exist_ok=True)
            fn = os.path.join(d, f"sh_{int(p['n']):03d}_{p['type']}.glsl")
            with open(fn, "w", encoding="utf-8") as f:
                f.write(p["src"])
            print(f"[frida_run] wrote {fn} ({len(p['src'])} bytes)")
        else:
            print("[SEND]", p)
    elif message.get("type") == "error":
        print("[JS ERROR]", message.get("description"), message.get("stack", ""))
    else:
        print("[MSG]", message)

script.on("message", on_msg)
script.load()
time.sleep(SECS)
try: session.detach()
except Exception: pass
print("[frida_run] done")
