#!/bin/bash
set -e
BT="/c/Android/build-tools/34.0.0"
python cooker/cook.py
"$BT/zipalign.exe" -f -p 4 cooker/out/myhome.apk cooker/out/myhome_aligned.apk
"$BT/apksigner.bat" sign --ks cooker/debug.keystore --ks-pass pass:android --key-pass pass:android \
  --ks-key-alias myhome --out cooker/out/myhome_signed.apk cooker/out/myhome_aligned.apk
echo "BAKED + SIGNED -> cooker/out/myhome_signed.apk"
