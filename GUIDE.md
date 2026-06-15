# Converter Guide — porting an old Quest Home onto a modern headset

This is the **step-by-step user guide** for turning an old (V79-era) Meta Quest *Home*
environment into an APK you can install on a current Quest 2 / 3 / Pro.

> New here? You only need three things: the **renderer exe**, **adb**, and the **old home file**
> (`.apk` / `.gltf.ovrscene` / `.opa`). Everything else (signing, choosing the right APK,
> installing) is automatic.

---

## 0. TL;DR (the happy path)

1. Put **`adb.exe`, `AdbWinApi.dll`, `AdbWinUsbApi.dll`** next to `hsr_renderer.exe` (see [§2](#2-adb-setup-once)).
2. Plug in the Quest over USB, accept the **"Allow USB debugging"** prompt in the headset.
3. Drag the old home (`env.apk` / `.gltf.ovrscene`) onto `hsr_renderer.exe`.
4. Open the **Cook** tab → leave the defaults → press **`COOK + SIGN + INSTALL`**.
5. Put the headset on — your env loads **in place of Haven 2025** (the tool relaunches the shell for you).
   An unrooted Quest **can't switch to a custom environment**, so the spoof *becomes* Haven 2025 instead.

That's it. The tool cooks, signs, picks the correct APK for your device, and installs it.

---

## 1. What this tool actually does

It is **two tools in one binary**:

- A **viewer/editor** — loads an old home, renders it, and lets you move/scale/hide objects,
  set the spawn point, add colliders, etc. (Blender-style editor.)
- A **converter ("cooker")** — repackages the (edited) scene into a **modern HSL environment APK**
  that the current Quest shell can load.

You do **not** have to edit anything. You can load → Cook → install and get the home as-is.

---

## 2. ADB setup (once)

The converter installs to the headset with Google's `adb`. You do **not** need the full Android
SDK / "platform-tools" install — just **three files**, placed **next to `hsr_renderer.exe`**:

```
hsr_renderer.exe
adb.exe
AdbWinApi.dll
AdbWinUsbApi.dll
```

The tool looks for `adb` in this order: `$HSR_ADB` → **beside the exe** → `C:\Android\platform-tools\adb.exe`
→ `adb` on your `PATH`. So dropping those three files beside the exe is enough.

> You can get the three files from Google's "SDK Platform-Tools" zip (just copy those three out of it),
> or from any existing platform-tools folder. On Linux/macOS only the single `adb` binary is needed.

**On the headset:** enable Developer Mode (Meta Quest mobile app → your headset → Developer Mode),
plug in USB, and **accept the "Allow USB debugging" dialog** that appears in the headset the first time.

To test the connection: `adb devices` should list your headset.

---

## 3. The two output APKs (important)

A cook produces up to **two** APKs, written **next to the loaded environment**:

| File | Package | Install on… | How it loads |
| --- | --- | --- | --- |
| **`<env>_NoRoot-Spoof.apk`** | masquerades as `…haven2025` | **any** Quest (no root) | **replaces** the Haven 2025 home in place; loads after the shell relaunches |
| **`<env>_Rooted-System.apk`** | the env's own package | **rooted / dev** headsets only | auto-selected via `adb` (`oculuspreferences`) — needs root |

**Why a spoof at all?** An unrooted Quest **can't switch to a custom home environment** — only Meta's
built-in ones, and only *root* can point the shell at an arbitrary env. The spoof gets around this by
**taking over the built-in Haven 2025 home**: it installs *as* Haven 2025, so the home the shell already
loads becomes your ported env. **There is nothing to "switch" to — and on an unrooted headset you can't.**

**Which one do I use?**

- **Normal retail Quest (not rooted): the `NoRoot-Spoof` APK.** The only one loadable without root. It
  replaces Haven 2025 in place; after install the tool relaunches the shell and your env loads where
  Haven 2025 was.
- **Rooted / userdebug headset: the `Rooted-System` APK** keeps your env under its own package name and
  is auto-selected for you (root can point the shell at any env).

**You normally don't choose manually** — the auto-installer does it for you (next section).

---

## 4. Cooking + installing (the Cook tab)

Hover any control in the Cook tab for an inline **tooltip**. The defaults are what most people want:

| Option | Default | Meaning |
| --- | --- | --- |
| **Package** | env's id | Package id for the *Rooted-System* APK only. |
| **Auto-sign** | ✅ on | Signs the APKs so the Quest accepts them. Keep on. |
| **Emit haven2025 spoof** | ✅ on | Builds the no-root APK. Keep on (it's the one most people install). |
| **Animate skinned meshes (HZANIM)** | ☐ off | Experimental, can crash the env. Leave off. |
| **Install to headset after cook (auto)** | ✅ on | Cook → sign → install automatically. |
| **Wi-Fi IP / Device** | blank | For wireless adb / picking a specific device. Blank = USB / default device. |

Press **`COOK + SIGN + INSTALL`**. The installer then:

1. **Detects root** on the connected headset (`adb root` + `getprop` / `id`).
2. **Rooted →** installs the **`Rooted-System`** APK and **auto-selects** it.
3. **Not rooted →** **backs up your real Haven 2025** (see [§6](#6-restoring-the-real-haven-2025)),
   installs the **`NoRoot-Spoof`** APK over it, and **relaunches the shell** so it reloads.

The status line at the bottom of the panel tells you exactly what happened.

> **Wi-Fi install:** type the headset IP in **Wi-Fi IP**, press **Connect**, then Cook. (Enable
> "Wireless debugging" on the headset first; over Wi-Fi the root check may briefly drop the link
> and reconnect.)

---

## 5. Manual install (if you don't use auto-install)

If you turn off auto-install (or want to install a file someone sent you):

**No-root (retail Quest):**
```
adb install -r -d <env>_NoRoot-Spoof.apk
adb shell kill $(adb shell pidof com.oculus.vrshell)    # relaunch the shell so it reloads
```
Your env now loads **in place of Haven 2025** after the relaunch — there's no env to switch to (an
unrooted Quest can't). If you don't see it, make sure **Haven 2025** is your selected home environment.

**Signing a file that won't install** (`INSTALL_PARSE_FAILED_NO_CERTIFICATES` = it's unsigned):
```
hsr_renderer.exe --sign <file>.apk        # writes <file>_signed.apk
hsr_renderer.exe --fetch-tools            # (optional) pre-download the signing toolchain
```

---

## 6. Restoring the real Haven 2025

Because the no-root path **overwrites** the Haven 2025 home, the tool first copies the original off
the device to a **single pristine backup beside the exe**: **`haven2025_ORIGINAL_backup.apk`**. It is
taken **once** (the first time you ever install a spoof) and is **never re-backed-up or overwritten** —
so even after you've cooked many homes, that file is still the *real* Haven 2025. (If it re-backed-up
later it would just save the spoof that's currently installed, destroying the only real copy.)

To put the real Haven 2025 back:
```
adb install -r -d haven2025_ORIGINAL_backup.apk
adb shell kill $(adb shell pidof com.oculus.vrshell)
```

> If Haven 2025 was a non-removable **system** app on your headset, the backup pull or the restore
> may need root. In that case keep using the `Rooted-System` APK, or factory-reset to restore the
> stock home.

---

## 7. Troubleshooting

**I still see the default foggy / blue home, not my env.**
The cook didn't get loaded — the shell fell back to its default. Checklist:
- Did you install the **`NoRoot-Spoof`** APK (not the `Rooted-System` one) on a non-rooted Quest?
- Is **Haven 2025** your headset's selected home environment? The spoof replaces it **in place**, so your
  env only appears where Haven 2025 would. (You don't switch to a custom env — an unrooted Quest can't.)
- Did the shell relaunch? Kill `com.oculus.vrshell` (see §5) or sleep/wake the headset to force a reload.
- Was the install actually accepted? Re-run with the headset connected and read the status line; a
  `spoof install FAILED` means Haven 2025 is a non-removable system app on your device (use the rooted path).

**Black void / nothing renders (often Quest 2).**
The env loaded but draws nothing. Quest 2 is a weaker/older GPU and some ported content does not yet
render there. Try the same cook on a Quest 3/Pro to confirm the env itself is good. (Rendering
faithfulness of ported content is still a work in progress — see the README disclaimer.)

**`INSTALL_PARSE_FAILED_NO_CERTIFICATES`.**
The APK is unsigned. Keep **Auto-sign** on, or sign manually (§5). On a clean PC with no Android SDK
and no Java, the first sign auto-downloads the toolchain beside the exe — this needs `curl` + a network
connection (run `--fetch-tools` to pre-fetch).

**`adb` not found / install does nothing.**
Put `adb.exe` + the two DLLs beside the exe (§2), or set `HSR_ADB` to your `adb.exe`. Verify with
`adb devices` and accept the USB-debugging prompt in the headset.

**Wrong device when several are attached.**
Put the serial from `adb devices` in the **Device** field.

---

## 8. Environment variables (advanced)

| Var | Effect |
| --- | --- |
| `HSR_ADB` | Path to `adb` (overrides the beside-the-exe / PATH search). |
| `HSR_BUILDTOOLS` | Android build-tools dir (apksigner + zipalign) if you don't want auto-detect/download. |
| `HSR_KEYSTORE` | Signing keystore (else a debug keystore is auto-generated beside the exe). |
| `HSR_COOK_OUT` | Override the output APK path. |
| `HSR_NOINSTALL` | Headless/CLI cooks: skip the auto-install step. |
