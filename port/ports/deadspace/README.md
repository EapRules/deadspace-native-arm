# Dead Space Mobile — native PortMaster port

Runs the 2011 **Dead Space Mobile** game by IronMonkey Studios natively on the
R36S and similar Linux/ARM handhelds. There is no Android runtime and no
emulator: the original ARM game library is loaded directly through a
bionic/JNI compatibility layer.

> **Bring your own game.** The release contains no EA game binary or asset.
> Supply a copy you own; nothing is downloaded and no protection is bypassed.

## Required game version

The supported donor is **Dead Space Mobile for Xperia Play v1.1.33**:

```text
Android package: com.eamobile.deadspace_sonyericsson
Library:         lib/armeabi/libEAMGameDeadSpace.so
SHA1:            0ed42b611415015807f759ec9b5457857143ce39
```

Backups of this release are sometimes labelled **Dead Space Mobile Version
1.1.33** or **dead space vita 1.1.33**. The label and filename are not trusted:
the first-boot extractor accepts the donor only when its contents and native
library match the supported build.

## Install with PortMaster autoinstall

Do not manually unzip the port release into `ports/`. Let PortMaster install
it so the executable bit and EmulationStation entry are created correctly.

1. Put the release file **`deadspace.zip`** in PortMaster's `autoinstall/`
   directory:

   | CFW | Autoinstall directory |
   |---|---|
   | ArkOS, dArkOS | `/roms/tools/PortMaster/autoinstall/` |
   | AmberELEC, ROCKNIX, JELOS, uOS | `/roms/ports/PortMaster/autoinstall/` |
   | muOS | `/mmc/MUOS/PortMaster/autoinstall/` |
   | Knulli | `/userdata/system/.local/share/PortMaster/autoinstall/` |

2. On the same SD card, create `ports/deadspace/` and put your donor there.
   It may be an APK, a ZIP, or a folder already extracted from an APK/7z. The
   filename does not matter.

   For APK or ZIP input:

   ```text
   ports/deadspace/my-dead-space-copy.zip
   ```

   If your source is a 7z, extract it on the computer first and use:

   ```text
   ports/deadspace/gamedata/donor/
   ├── assets/
   └── lib/armeabi/libEAMGameDeadSpace.so
   ```

   The port ZIP and a donor ZIP can both originally be named `deadspace.zip`;
   they go in different directories. Rename the donor if that makes the copy
   easier to follow.

3. Put the card back in the console and open PortMaster. It finds the release
   in `autoinstall/`, installs it and adds the menu metadata.

4. Close PortMaster and **reboot the console**. Autoinstall does not refresh
   the Ports list that EmulationStation loaded at boot.

5. Launch Dead Space from Ports. The first launch discovers the donor by
   content, extracts roughly 303 MiB into a temporary stage, validates every
   required output and publishes it atomically. Keep at least 500 MiB free and
   do not power off during this first extraction. Later launches start
   normally. Once the game has launched successfully, the original donor file
   is no longer required by the port.

The importer accepts these layouts without requiring the user to rearrange
them:

```text
assets/... + lib/armeabi/...
deadspace/assets/... + deadspace/lib/armeabi/...
data/deadspace/assets/... + data/deadspace/lib/armeabi/...
NeededFiles/data/deadspace/assets/... + NeededFiles/data/deadspace/lib/armeabi/...
```

### Two autoinstall gotchas

- The game does not appear in Ports until the console is rebooted.
- Do not use **Reinstall** or **Uninstall** under Manage Ports. This independent
  release is not in PortMaster's catalogue, so those actions try to download a
  source that does not exist there and may remove the installed folder,
  including the user's donor and extracted data. To update, put the new release
  ZIP in `autoinstall/` again.

A harmless “No internet connection” message may appear after installation
while PortMaster refreshes its own catalogue. The port and its screenshot are
already inside the ZIP.

## Controls

The original menus are touch-only. The port supplies a software pointer and
turns the handheld controls into the same touch/key events used by the game.

| R36S control | Action |
|---|---|
| D-pad | Move the menu pointer |
| A | Tap/click at the pointer; game A when the pointer is hidden |
| B | Back |
| X / Y | Xperia Play X / Y |
| L1 / R1 | Shoulder buttons |
| Start | Pause/menu and restore the pointer |
| L3 / R3 | Show or hide the pointer |
| Left stick | Walk through the virtual movement stick |
| Right stick | Continuous camera/aim through the virtual touchpad |

Moving either analogue stick hides the pointer for gameplay. Press L3, R3 or
Start to recover it for a menu.

## Requirements and current testing

- armhf execution and 32-bit GPU libraries, like box86/GMLoader ports;
- two analogue sticks;
- glibc 2.38 or newer;
- about 500 MiB free for first-boot extraction.

Tested on an R36S (RK3326/Mali-G31) with dArkOSRE at 640x480. Centred output,
the PVRTC software fallback, the complete 3D menu scene and pad input are
confirmed on that device. The release includes the later pointer-recovery,
continuous-camera and audio/VFP fixes; reports from other firmware and a full
play-through are welcome.

## Troubleshooting

Each launch replaces `ports/deadspace/log.txt`. First-boot extraction writes
`ports/deadspace/eapx.log` separately. If installation fails, include both
files plus the device/CFW when reporting it.

Common causes:

- `no game package was found`: donor is not inside `ports/deadspace/` or its
  `gamedata/` child;
- `no input matches this recipe`: wrong release or incomplete archive;
- `sha256 ... not in the accepted list`: native library is not Xperia Play
  v1.1.33;
- `GL: no compatible 32-bit Mali blob found`: the firmware lacks the required
  armhf GPU userspace.

## Credits and licence

Game by **IronMonkey Studios**, published by **Electronic Arts**. Port by
**EapRules**.

The bionic ELF loader and JNI/libc compatibility work derive from
[gmloader-next](https://github.com/JohnnyonFlame/gmloader-next), itself based on
Andy Nguyen's Vita so-loader. PVRTC decoding comes from Imagination
Technologies' PowerVR SDK, and the ARMv8 short-vector expansion is adapted from
Bythos14's VFPVector.

The port is GPL-3.0. Exact notices and the copyright terms for every bundled
shared library are shipped under `licenses/`.
