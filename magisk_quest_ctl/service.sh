#!/system/bin/sh
# Magisk late_start service — runs as root every boot. Keeps the HSR test headset usable headless.
# Wait for the framework so setprop/am stick.
until [ "$(getprop sys.boot_completed)" = "1" ]; do sleep 2; done
sleep 5

# 1) Disable the proximity sensor -> the headset is ALWAYS "worn": it never idle-sleeps and the universal
#    menu/dock stops auto-popping over the env (the screencap-blocker). (Toggle live with `questctl awake|sleep`.)
setprop persist.device_config.mros_vendor.hmd_disable_prox_sensor true
am broadcast -a com.oculus.vrpowermanager.prox_close >/dev/null 2>&1

# 2) Verbose ShellHsr logging so `questctl sniff` sees asset/shader/scene-load detail (no frida needed).
setprop debug.oculus.shell.logLevel Verbose
setprop debug.logLevel Verbose

log -t QuestEnvCtl "boot: prox disabled (always-worn), ShellHsr verbose ON"
