#!/bin/sh
# Install the Quest Env Control Magisk module on a ROOTED Quest over adb. Run from this folder:
#   sh install.sh            (uses `adb`; or set ADB=/path/to/adb and SERIAL=192.168.1.35:5555)
ADB="${ADB:-adb}"; S="${SERIAL:+-s $SERIAL}"; M=/data/adb/modules/quest_env_ctl
set -e
$ADB $S root >/dev/null 2>&1 || true
echo "[*] pushing module -> $M"
$ADB $S shell "su -c 'mkdir -p $M/system/bin'"
$ADB $S push module.prop            "$M/module.prop"
$ADB $S push service.sh             "$M/service.sh"
$ADB $S push system/bin/questctl    "$M/system/bin/questctl"
$ADB $S shell "su -c 'chmod 0755 $M/service.sh $M/system/bin/questctl; chcon u:object_r:system_file:s0 $M/system/bin/questctl 2>/dev/null; touch $M/auto_mount'"
# Apply LIVE (no reboot): run the boot script now + drop questctl on the default PATH.
$ADB $S shell "su -c 'sh $M/service.sh >/dev/null 2>&1 &'"
$ADB $S shell "su -c 'cp $M/system/bin/questctl /data/local/tmp/questctl; chmod 0755 /data/local/tmp/questctl'"
echo "[*] installed. live: 'adb shell /data/local/tmp/questctl status'  |  after a REBOOT: 'adb shell questctl status'"
$ADB $S shell "su -c '/data/local/tmp/questctl status'"
