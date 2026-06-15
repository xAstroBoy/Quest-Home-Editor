# Signs BOTH the unspoofed and the _haven2025 spoof APK for a given cook.
# Usage:  .\cooker\sign.ps1 aurora_v203        (expects cooker/out/<base>_unsigned.apk [+ _unsigned_haven2025.apk])
param([Parameter(Mandatory=$true)][string]$base)
$bt  = "C:\Android\build-tools\34.0.0"
$ks  = "cooker/debug.keystore"
$out = "cooker/out"
function SignOne($unsigned, $signed) {
    if (-not (Test-Path $unsigned)) { Write-Host "  (skip, missing: $unsigned)"; return }
    $tmp = "$signed.aligned"
    & "$bt\zipalign.exe" -f -p 4 $unsigned $tmp | Out-Null
    cmd /c "`"$bt\apksigner.bat`" sign --ks $ks --ks-pass pass:android --key-pass pass:android --ks-key-alias myhome --out $signed `"$tmp`"" | Out-Null
    Remove-Item $tmp -ErrorAction SilentlyContinue
    Remove-Item "$signed.idsig" -ErrorAction SilentlyContinue   # v4 sig file — not needed for sharing
    Remove-Item $unsigned -ErrorAction SilentlyContinue         # keep out/ clean: drop the unsigned source
    Write-Host ("  signed -> {0} ({1} KB)" -f $signed, [int]((Get-Item $signed).Length/1024))
}
SignOne "$out/${base}_unsigned.apk"           "$out/${base}_signed.apk"
SignOne "$out/${base}_unsigned_haven2025.apk" "$out/${base}_haven2025_signed.apk"
