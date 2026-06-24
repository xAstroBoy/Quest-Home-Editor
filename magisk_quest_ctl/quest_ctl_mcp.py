#!/usr/bin/env python
"""Quest-Ctl MCP server — drives the Magisk `questctl` CLI over adb (sniff + control the headset, dismiss the
idle menu/dock). Stdio JSON-RPC, no deps. Register in .mcp.json:

  "quest-ctl": { "command": "python", "args": ["magisk_quest_ctl/quest_ctl_mcp.py"],
                 "env": { "QUESTCTL_SERIAL": "192.168.1.35:5555" } }

Requires the Magisk module installed (sh magisk_quest_ctl/install.sh)."""
import sys, json, os, subprocess

ADB = os.environ.get("ADB", "adb")
SER = os.environ.get("QUESTCTL_SERIAL", "")
QCTL = "sh /data/adb/modules/quest_env_ctl/system/bin/questctl"   # via `sh` = works now (noexec /data) + after reboot

def adb(*a, timeout=30):
    cmd = [ADB] + (["-s", SER] if SER else []) + list(a)
    try:
        r = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
        return (r.stdout or "") + (("\n[stderr] " + r.stderr) if r.stderr.strip() else "")
    except Exception as e:
        return "[adb error] %s" % e

def qctl(*a): return adb("shell", "su", "-c", QCTL + " " + " ".join(str(x) for x in a))

TOOLS = [
    {"name": "quest_stay_awake", "description": "Keep the headset 'worn' (disable proximity) so it never idle-sleeps and the menu/dock stops auto-popping over the env.", "inputSchema": {"type": "object", "properties": {}}},
    {"name": "quest_allow_sleep", "description": "Restore normal proximity/sleep behaviour.", "inputSchema": {"type": "object", "properties": {}}},
    {"name": "quest_menu_dismiss", "description": "Best-effort dismiss of the universal menu/dock overlay (worn-spoof + back).", "inputSchema": {"type": "object", "properties": {}}},
    {"name": "quest_sniff", "description": "Dump recent device log for a tag (default ShellHsr) + all errors — the sniff feed.", "inputSchema": {"type": "object", "properties": {"tag": {"type": "string"}, "lines": {"type": "integer"}}}},
    {"name": "quest_tap", "description": "Tap a screen coordinate.", "inputSchema": {"type": "object", "properties": {"x": {"type": "integer"}, "y": {"type": "integer"}}, "required": ["x", "y"]}},
    {"name": "quest_key", "description": "Send a keyevent (e.g. KEYCODE_BACK, 3).", "inputSchema": {"type": "object", "properties": {"key": {"type": "string"}}, "required": ["key"]}},
    {"name": "quest_screencap", "description": "Capture the screen and pull it to a local PNG path.", "inputSchema": {"type": "object", "properties": {"out": {"type": "string"}}}},
    {"name": "quest_reload_shell", "description": "Kill com.oculus.vrshell so it reloads (re-applies the selected env).", "inputSchema": {"type": "object", "properties": {}}},
    {"name": "quest_status", "description": "vrshell pid + prox/loglevel + selected env.", "inputSchema": {"type": "object", "properties": {}}},
    {"name": "quest_apps", "description": "List installed user app packages (or all=true for system too).", "inputSchema": {"type": "object", "properties": {"all": {"type": "boolean"}}}},
    {"name": "quest_launch", "description": "Launch an app by package (LAUNCHER intent; or pass component pkg/.Activity).", "inputSchema": {"type": "object", "properties": {"pkg": {"type": "string"}}, "required": ["pkg"]}},
    {"name": "quest_kill", "description": "Force-stop an app by package (toggle off / close).", "inputSchema": {"type": "object", "properties": {"pkg": {"type": "string"}}, "required": ["pkg"]}},
    {"name": "quest_current", "description": "Current foreground activity/focus.", "inputSchema": {"type": "object", "properties": {}}},
    {"name": "quest_button", "description": "Inject a controller button KEY on both controllers (304=A 305=B 307=X 308=Y; getevent -p for codes).", "inputSchema": {"type": "object", "properties": {"code": {"type": "integer"}}, "required": ["code"]}},
    {"name": "quest_player_enable", "description": "Arm (on=1) or idle (on=0) the questpctl zygisk player-control hook. Must be armed before teleport/walk.", "inputSchema": {"type": "object", "properties": {"on": {"type": "integer"}}}},
    {"name": "quest_teleport", "description": "Teleport the player to absolute world x y z (questpctl hook must be armed). Does not fall (it's a position set, not a physics jump).", "inputSchema": {"type": "object", "properties": {"x": {"type": "number"}, "y": {"type": "number"}, "z": {"type": "number"}}, "required": ["x", "y", "z"]}},
    {"name": "quest_walk", "description": "Walk the player: fwd + strafe step size (0 0 = stop). Steps via throttled teleport ~2.5Hz. Hook must be armed.", "inputSchema": {"type": "object", "properties": {"fwd": {"type": "number"}, "strafe": {"type": "number"}}, "required": ["fwd", "strafe"]}},
    {"name": "quest_rotate", "description": "Rotate the player to face yaw (in place). Hook must be armed.", "inputSchema": {"type": "object", "properties": {"yaw": {"type": "number"}}, "required": ["yaw"]}},
]

def call(name, args):
    if name == "quest_stay_awake":   return qctl("awake")
    if name == "quest_allow_sleep":  return qctl("sleep")
    if name == "quest_menu_dismiss": return qctl("menu")
    if name == "quest_sniff":
        tag = args.get("tag", "ShellHsr"); n = int(args.get("lines", 200))
        return adb("shell", "logcat", "-d", "-v", "time", "-t", str(n), "-s", tag + ":V", "*:E")
    if name == "quest_tap":   return qctl("tap", args["x"], args["y"])
    if name == "quest_key":   return qctl("key", args["key"])
    if name == "quest_screencap":
        dev = "/sdcard/_questctl_shot.png"; out = args.get("out", "questctl_shot.png")
        adb("shell", "screencap", "-p", dev); adb("pull", dev, out); adb("shell", "rm", dev)
        return "saved " + out
    if name == "quest_apps":    return qctl("allapps" if args.get("all") else "apps")
    if name == "quest_launch":  return qctl("launch", args["pkg"])
    if name == "quest_kill":    return qctl("kill", args["pkg"])
    if name == "quest_current": return qctl("current")
    if name == "quest_button":  return qctl("button", int(args["code"]))
    if name == "quest_player_enable": return qctl("playerctl", int(args.get("on", 1)))
    if name == "quest_teleport":      return qctl("teleport", args["x"], args["y"], args["z"])
    if name == "quest_walk":          return qctl("walk", args["fwd"], args["strafe"])
    if name == "quest_rotate":        return qctl("rotate", args["yaw"])
    if name == "quest_reload_shell": return qctl("reloadshell")
    if name == "quest_status":
        env = adb("shell", "su", "-c", "dumpsys oemprefs --user 0 get environment_selected").strip() or adb("shell", "settings", "get", "secure", "environment_selected").strip()
        return qctl("status") + ("\nenv=" + env if env else "")
    return "unknown tool " + name

def main():
    for line in sys.stdin:
        line = line.strip()
        if not line: continue
        try: req = json.loads(line)
        except Exception: continue
        m = req.get("method"); rid = req.get("id")
        if m == "initialize":
            res = {"protocolVersion": "2024-11-05", "capabilities": {"tools": {}}, "serverInfo": {"name": "quest-ctl", "version": "1.0.0"}}
        elif m == "tools/list":
            res = {"tools": TOOLS}
        elif m == "tools/call":
            p = req.get("params", {})
            res = {"content": [{"type": "text", "text": call(p.get("name"), p.get("arguments", {}) or {})}]}
        elif m in ("notifications/initialized", "notifications/cancelled"):
            continue
        else:
            res = None
        if rid is not None:
            sys.stdout.write(json.dumps({"jsonrpc": "2.0", "id": rid, "result": res}) + "\n"); sys.stdout.flush()

if __name__ == "__main__":
    main()
