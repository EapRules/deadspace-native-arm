# Dead Space Mobile — native ARM Linux port

**Dead Space Mobile (2011, IronMonkey/EA) running natively on ARM Linux. No
emulator, no Android runtime.**

The original Android ARM library is loaded directly on Linux through a
bionic/JNI compatibility layer. The port supplies the Java/Android surface the
game expects — JNI classes, audio, input, filesystem — and fixes the graphics
and ARM compatibility problems at load time: PVRTC and ATC textures are
decoded in software when the GPU driver lacks them, and the game's obsolete
VFP short-vector audio mixer is expanded into scalar code for ARMv8.

**Port and project by [EapRules](https://github.com/EapRules).**

- Open source, GPL-3.0, free.
- The release contains **zero EA game content** — bring your own game data.
- Not tied to one device: it targets ARMv8 Linux handhelds through
  PortMaster. Reference device for verification: R36S (ArkOS) — menus,
  gameplay, textures, audio and controls confirmed on hardware.

**How does a 2011 Android binary run natively on modern ARM Linux?** The
bionic ELF loading, the fake JVM, fixed-function GL on shader-era drivers,
VFP short-vector expansion and the verification method are documented in
[`TECHNICAL.md`](TECHNICAL.md).

## Download

Grab `deadspace-portmaster.zip` from the
[latest release](https://github.com/EapRules/deadspace-portmaster/releases).

## You need the game data

The supported donor is **Dead Space Mobile for Xperia Play v1.1.33**, Android
package `com.eamobile.deadspace_sonyericsson`:

```text
lib/armeabi/libEAMGameDeadSpace.so
SHA1 0ed42b611415015807f759ec9b5457857143ce39
```

Two donor archives are verified compatible — the original Full PVRTC tree and
the smaller Vita RIP (`deadspace.zip`, campaign only). A third archive that
circulates as `deadspace-full.zip` uses ETC1 textures and is **not supported
yet**. Exact hashes, evidence and installation layout per donor:
[`port/DONOR_COMPATIBILITY.md`](port/DONOR_COMPATIBILITY.md).

## Install

1. Copy `deadspace-portmaster.zip` (unrenamed) into PortMaster's
   `autoinstall/` directory.
2. Put your donor (APK, ZIP or extracted folder) in `ports/deadspace/` — the
   file name does not matter, it is identified by content.
3. Open PortMaster and wait for the **Finished running autoinstall** dialog.
4. Reboot through the firmware menu, then launch **Dead Space** from Ports.

The first launch extracts and validates the game data automatically. Detailed
CFW paths and gotchas: [`port/ports/deadspace/README.md`](port/ports/deadspace/README.md).

## Controls

| Input | Action |
|---|---|
| D-pad + A | Move the software cursor and tap (title/menus) |
| L3 / R3 / Start | Restore the menu cursor |
| Left stick | Movement (virtual touchscreen stick) |
| Right stick | Aim (virtual touchpad stick) |
| L2 | Accelerometer tilt — rotate / switch fire mode |
| R2 | Accelerometer motion — Zero-G jump |

The original game has touch-only menus; the cursor is the input bridge that
makes them work on non-touch handhelds.

## Building from source

```bash
cd port
docker build -f Dockerfile.build -t deadspace-build .
docker run --rm -v "$PWD":/src -w /src deadspace-build make -j4
timeout 400 harness/verify.sh          # immutable verification harness
docker run --rm -v "$PWD":/src -w /src deadspace-build make libs
./package_portmaster.sh                # game-data-free PortMaster zip
```

Developer documentation — architecture, harness, interactive emulator and
diagnostics — lives in [`port/README.md`](port/README.md).

## Credits

- Port, project direction, device testing and eapx:
  [EapRules](https://github.com/EapRules)
- bionic ELF loading: gmloader-next by JohnnyonFlame, derived from Andy
  Nguyen's Vita so-loader
- PVRTC decompression: Imagination Technologies PowerVR SDK
- ARMv8 short-vector expansion: adapted from VFPVector by Bythos14
- Accelerometer gesture reference: deadspace-vita by v-atamanenko

Full credits and licences: [`port/ports/deadspace/CREDITS.md`](port/ports/deadspace/CREDITS.md),
[`port/NOTICE.md`](port/NOTICE.md).

## Legal

Dead Space Mobile was developed by IronMonkey Studios and published by
Electronic Arts. This repository and its releases distribute **no game binary
or asset**; you must own the game data. Never redistribute the game `.so` or
extracted assets.
