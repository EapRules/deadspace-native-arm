# Reddit release draft

## Title

`[Release] Dead Space Mobile running natively on the R36S — open-source PortMaster port (BYO game data)`

## Body

Dead Space Mobile running natively on the R36S.

No emulator and no Android runtime: the original Android ARM library runs
directly on Linux through a bionic/JNI compatibility loader. Dead Space uses a
Java/GLSurfaceView-style layer, GLES 1.1, compressed 3D textures and an old VFP
audio mixer. The port supplies the missing Android/JNI surface and handles the
graphics and ARM compatibility issues at load time.

Open source, GPL-3.0, free. The release contains zero EA game content.

Port and project by **EapRules**.

Download: https://github.com/EapRules/deadspace-portmaster/releases

**READ THIS FIRST: you need the Xperia Play v1.1.33 release.**

```text
Dead Space Mobile for Xperia Play v1.1.33
package: com.eamobile.deadspace_sonyericsson
lib/armeabi/libEAMGameDeadSpace.so
SHA1: 0ed42b611415015807f759ec9b5457857143ce39
```

### ZIPs and donor choice

`deadspace-portmaster.zip` is our free PortMaster port. It contains no EA game
data and goes in `tools/PortMaster/autoinstall/`.

**Choose ONLY ONE of the following donor ZIPs:**

- **Option A: Complete / Full Xperia Play donor ZIP**
  - The complete Xperia Play data tree, about 303 MiB after extraction.
  - Uses the original PVRTC texture assets (`0x8c00` / `0x8c02`).
  - Includes the optional `~2x` UI, Survival maps and Burst Rifle data.
  - The port keeps native PVRTC when available and otherwise decodes it to
    RGBA for the R36S GPU.

- **Option B: Vita RIP donor `deadspace.zip`**
  - The reduced public Vita RIP donor, about 247 MiB after extraction.
  - Uses ATC texture assets (`0x8c92` / `0x8c93`), not PVRTC.
  - Keeps the campaign but omits optional `~2x` UI, Survival maps and Burst
    Rifle data.
  - The port detects ATC and decodes it in software to RGBA, which prevents
    the white 3D textures seen when the R36S driver rejects ATC.

The donor filename does not matter: the first launch identifies the donor by
its contents, validates the exact native library, and rejects the wrong game
version. Do not mix files from different game releases.

The public Vita RIP is available here:

https://archive.org/details/deadspace_202504

The Complete donor is not included in the port release. Supply your own legal
copy of the required Xperia Play data.

## HOW TO INSTALL

With the SD card in your computer:

1. Put our `deadspace-portmaster.zip` in PortMaster's `autoinstall/` folder.
2. Create `ports/deadspace/` and put **one** donor ZIP there. Leave it
   compressed. The donor can be the Complete/Full ZIP or the Vita RIP
   `deadspace.zip`.

The clean layout is:

```text
tools/PortMaster/autoinstall/deadspace-portmaster.zip  # our free port
ports/deadspace/deadspace.zip                          # one donor ZIP
```

Put the card back in the console, open PortMaster and wait for the exact
**Finished running autoinstall** dialog. Let PortMaster close normally, then
**reboot through the firmware menu**. Dead Space appears in Ports after the
reboot.

The first launch extracts and validates roughly 247–303 MiB of game data,
depending on the donor. Keep at least 500 MiB free and do not power off during
that first extraction. Later launches start normally.

## TWO GOTCHAS

- Autoinstall does not refresh EmulationStation's list. Reboot after the final
  autoinstall dialog.
- Do not use Reinstall or Uninstall in Manage Ports. This independent release
  is not in PortMaster's catalogue, so those buttons may try to download a
  missing catalogue entry and delete the local donor. Update by putting the
  new ZIP in `autoinstall/` again.

## CONTROLS

- D-pad: move the menu pointer
- A: click/tap; game A while the pointer is hidden
- B: back
- L2: rotate/switch weapon fire mode (simulated tilt)
- R2: Zero-G jump (simulated motion gesture)
- L3/R3: show or hide the pointer
- Start: pause/menu and restore the pointer
- Left stick: walk
- Right stick: continuous camera/aim
- X/Y/L1/R1: Xperia Play buttons

The original menus are touch-only, so the software pointer is intentional.
Moving either stick hides it for gameplay; L3, R3 or Start brings it back.

Requires armhf execution, 32-bit GPU libraries and glibc 2.38+, like
box86/GMLoader ports. Tested on R36S with dArkOSRE at 640x480. Other
handhelds/CFWs are untested, not unsupported.

The README contains exact paths, diagnostics and technical details. If it
fails, attach both `ports/deadspace/log.txt` and `ports/deadspace/eapx.log`.
