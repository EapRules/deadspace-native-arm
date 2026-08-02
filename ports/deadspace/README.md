# Dead Space Mobile — native PortMaster port

Runs the 2011 **Dead Space Mobile** game by IronMonkey Studios natively on the
R36S and similar Linux/ARM handhelds. There is no Android runtime and no
emulator: the original ARM game library is loaded directly through a
bionic/JNI compatibility layer.

**Port and project by [EapRules](https://github.com/EapRules).**

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
the first-boot extractor accepts the donor only when its contents, native
library and several campaign/UI files match a supported build.

Two content variants are currently supported:

- the complete Xperia Play tree, with roughly 303 MiB under
  `assets/published/`;
- the Vita-ready `deadspace.zip` commonly distributed under the second label,
  whose ZIP is 145,903,794 bytes (SHA1
  `61d51d8ba5374f97b5a4971a2d9d7da31baf840c`). It keeps the main campaign but
  omits the optional `~2x` UI set, Survival maps and Burst Rifle content.

The complete tree is preferred when available. Both contain the same required
native library and are validated as explicit known variants; merely lowering a
file-count check is not enough to make an arbitrary partial archive install.

A third tested archive must not be confused with the complete donor:
`deadspace-full.zip` with SHA-256
`48d12821638d168ab4009571f4fb6fb8af29de11f51f52f88a842c58055bfe5a`
uses ETC1 textures. It is not the original Full PVRTC tree and is not currently
supported: it reaches gameplay with black 3D models and scenery. The current
extractor can still recognise/import this diagnostic profile, so users must
not install that archive until the launcher rejects it early or ETC1 support
is completed. See [`../../DONOR_COMPATIBILITY.md`](../../DONOR_COMPATIBILITY.md).

## Install with PortMaster autoinstall

Do not manually unzip the port release into `ports/`. Let PortMaster install
it so the executable bit and EmulationStation entry are created correctly.

1. Put the port release in PortMaster's `autoinstall/` directory. The release
   asset is named **`deadspace-portmaster.zip`** so it cannot be confused with
   the Vita RIP donor's own `deadspace.zip`. Keep that exact filename: the root
   launcher already carries the matching PortMaster signature, which avoids an
   unnecessary in-place rewrite after extraction.

   | CFW | Autoinstall directory |
   |---|---|
   | ArkOS, dArkOS | `/roms/tools/PortMaster/autoinstall/` |
   | AmberELEC, ROCKNIX, JELOS, uOS | `/roms/ports/PortMaster/autoinstall/` |
   | muOS | `/mmc/MUOS/PortMaster/autoinstall/` |
   | Knulli | `/userdata/system/.local/share/PortMaster/autoinstall/` |

2. On the same SD card, create `ports/deadspace/` and put one supported donor
   there.
   It may be an APK, a ZIP, or a folder already extracted from an APK/7z. The
   filename does not identify the donor. Do not use the unsupported ETC1
   `deadspace-full.zip` described above.

   For APK or ZIP input:

   ```text
   ports/deadspace/deadspace.zip
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
   in `autoinstall/`, installs it and adds the menu metadata. Wait for the exact
   **Finished running autoinstall** dialog, acknowledge it and let PortMaster
   return or close normally. Do not power off while its progress/file list is
   still visible.

4. **Reboot the console through the firmware menu**, not by cutting power.
   Autoinstall does not refresh the Ports list that EmulationStation loaded at
   boot.

5. Launch Dead Space from Ports. The first launch discovers the donor by
   content, extracts roughly 243-303 MiB into a temporary stage, validates
   every required output and publishes it atomically. Keep at least 500 MiB
   free and do not power off during this first extraction. Later launches
   start normally. Once the game has launched successfully, the original donor
   file is no longer required by the port.

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
- PortMaster rewrites unsigned launchers and the gamelist in place. An abrupt
  power cut can leave either one at 0 bytes on an exFAT SD card. Wait for the
  final autoinstall dialog and use the normal reboot/shutdown path.
- Do not use **Reinstall** or **Uninstall** under Manage Ports. This independent
  release is not in PortMaster's catalogue, so those actions try to download a
  source that does not exist there and may remove the installed folder,
  including the user's donor and extracted data. To update, put the new release
  ZIP in `autoinstall/` again.

A harmless “No internet connection” message may appear after installation
while PortMaster refreshes its own catalogue. The port and its screenshot are
already inside the ZIP.

On ArkOS/dArkOS, the launcher also refreshes the firmware's normalized
`ports/images/Dead Space.png` from the canonical `deadspace/cover.png` when an
older direct installation left an APK icon there. Restart EmulationStation (or
reboot once) after the first updated launch to clear its in-memory artwork.

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
| L2 | Rotate/switch the weapon fire mode (simulated tilt) |
| R2 | Jump in Zero-G (simulated accelerometer motion) |
| Start | Pause/menu and restore the pointer |
| L3 / R3 | Show or hide the pointer |
| Left stick | Walk through the virtual movement stick |
| Right stick | Continuous camera/aim through the virtual touchpad |

Moving either analogue stick hides the pointer for gameplay. Press L3, R3 or
Start to recover it for a menu.

## Requirements and current testing

- armhf execution and 32-bit GPU libraries, like box86/GMLoader ports;
- two analogue sticks;
- glibc 2.29 or newer (runs on ArkOS/AeolusUX and newer firmwares);
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
Bythos14's VFPVector. The trigger-driven accelerometer samples are adapted from
the MIT-licensed deadspace-vita port by v-atamanenko.

The port is GPL-3.0. Exact notices and the copyright terms for every bundled
shared library are shipped under `licenses/`; the complete project attribution
is also preserved in `CREDITS.md`.
