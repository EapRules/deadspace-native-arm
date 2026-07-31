# Reddit release draft

## Title

`[Release] Dead Space Mobile running natively on the R36S — open-source PortMaster port (BYO game data)`

## Body

Dead Space Mobile running natively on the R36S.

No emulator and no Android runtime: the original Android ARM library runs
directly on Linux through a bionic/JNI compatibility loader. This one is very
different from the earlier Ice Rage and Minigore 2 ports: Dead Space uses a
Java/GLSurfaceView-style engine, GLES 1.1, PowerVR-compressed assets and an old
VFP audio mixer, so the port supplies the missing Android/JNI surface and
patches the hardware incompatibilities at load time.

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

Some backups label it **Dead Space Mobile Version 1.1.33** or **dead space vita
1.1.33**. The filename does not matter: the port identifies the donor by its
contents and rejects the wrong native library before applying any patches.

**HOW TO INSTALL**

With the SD card in your computer:

1. Put the release `deadspace.zip` in PortMaster's `autoinstall/` folder.
2. Create `ports/deadspace/` and put your own APK or donor ZIP there. Leave it
   compressed; the filename does not matter. If your source is a 7z, extract
   it on the computer under `ports/deadspace/gamedata/donor/` instead.

Same card, same trip. Put it back in the console, open PortMaster and let it
install, then **REBOOT**. Dead Space appears in Ports after the reboot.

The first launch extracts and validates about 303 MiB of game data. Keep at
least 500 MiB free and do not power off during that first extraction. Later
launches start normally.

**TWO GOTCHAS**

- Autoinstall does not refresh EmulationStation's list, so the game will not
  appear until you reboot.
- Do not use Reinstall or Uninstall in Manage Ports. This independent release
  is not in PortMaster's catalogue, so those buttons try to download a source
  that is not there and may delete the user's donor/extracted data. Update by
  putting the new ZIP in `autoinstall/` again.

**CONTROLS**

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

The menus are touch-only in the original game, so the software pointer is
intentional. Moving either stick hides it for gameplay; L3, R3 or Start brings
it back.

Requires armhf execution, 32-bit GPU libraries and glibc 2.38+, like
box86/GMLoader ports. Tested on R36S with dArkOSRE at 640x480. The output is
centred, the PowerVR textures are decoded for Mali-G31, and the complete 3D
scene and pad input work on the real device. Other handhelds/CFWs are untested,
not unsupported.

The README contains exact paths, diagnostics and technical details. If it
fails, attach both `ports/deadspace/log.txt` and `ports/deadspace/eapx.log`.
