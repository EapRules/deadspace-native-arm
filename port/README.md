# Dead Space — native port for Linux ARM handhelds

Runs **Dead Space** (Mountain Sheep, 2012) natively on the R36S and
similar handhelds. **No emulator and no Android runtime**: the game's own
`armeabi-v7a` library is loaded straight into a Linux process by a bionic ELF
loader, and the pieces of Android it asks for are implemented here by hand.

> **Bring your own game.** This repository and its releases contain **zero game
> content**. You supply an APK you own; nothing is downloaded and no protection
> is circumvented.

Download the ready-to-use zip from the [Releases](../../releases) page.

## Which APK — read this first

You need the **OUYA build** of `net.mountainsheep.deadspace`, version **1.8**
(October 2013).

The **Google Play build will not work**, and not for a reason any amount of
button remapping can fix: it is touch-only, and the engine in the controller
build rejects touchscreen events outright —

```
[W/native-activity] WARNING: Unknown input source (4098)!
```

`4098` is `AINPUT_SOURCE_TOUCHSCREEN`. Feed it synthetic fingers and the menus
never respond no matter what you press. That cost a full round of testing to
work out, so it is here rather than buried in a troubleshooting section.

## Install

**Do not unzip this into `ports/` by hand.** PortMaster's own FAQ warns that
doing so can leave a port that never starts — it is how you end up without the
execute bit and without the menu entry. Let PortMaster install it:

1. **With the card in your computer**, drop `deadspace.zip` into PortMaster's
   `autoinstall/` folder, unchanged:

   | CFW | Folder |
   |---|---|
   | ArkOS, dArkOS | `/roms/tools/PortMaster/autoinstall/` |
   | AmberELEC, ROCKNIX, JELOS, uOS | `/roms/ports/PortMaster/autoinstall/` |
   | muOS | `/mmc/MUOS/PortMaster/autoinstall/` |
   | Knulli | `/userdata/system/.local/share/PortMaster/autoinstall/` |

2. **Put your APK at `ports/deadspace/deadspace.apk`** — same card, same trip,
   before it goes back in the console. Create the folder if it does not exist
   yet; the install will not remove it. That exact name, next to where the
   `deadspace` binary will land:

   ```
   ports/
   ├── Dead Space.sh
   └── deadspace/
       ├── deadspace            (the loader)
       ├── deadspace.apk        ← yours, you add this
       ├── deadspace.gptk
       ├── libs.armhf/
       └── assets/
   ```

   Watch the extension: some sites hand you `something.5apk` or `.apk.zip`, and
   it has to end in `.apk`.

3. **Put the card back in the console and open PortMaster.** It installs the
   port, sets the permissions and adds the menu entry. Close it when it is done.

4. **Reboot the console.** This step is not optional and it is the one everybody
   skips — see below.

5. **Launch Dead Space** from the Ports menu.

### Read this before deciding the port is broken

**The game will not appear in Ports until you reboot.** PortMaster installs it
correctly and then says nothing more: the autoinstall path never triggers the
frontend refresh, so EmulationStation keeps showing the list it loaded at boot.
Everything is on the card, it is simply not being listed yet. Reboot and it is
there.

**Do not use *Reinstall* or *Uninstall* under Manage Ports.** Both re-download
from PortMaster's own catalogue, and this port does not live there, so you get
*"unable to find a source for deadspace.zip"* — after the port has already been
removed. That is how a working install turns into an empty Ports menu, and it is
easy to do twice in a row while trying to fix the first one. To reinstall or
update, drop the zip into `autoinstall/` again.

*Uninstall* is also worth avoiding for a second reason: it deletes the whole
port folder, **your APK included**.

**"No internet connection"** right after installing is harmless. PortMaster is
trying to refresh its catalogue and fetch box art from its servers; this port is
not in that catalogue, so there is nothing to fetch — the artwork ships in the
zip. The install already succeeded.

If the APK is missing the port says so on screen, with the path it expects,
instead of dropping you back to the menu with no explanation.

## Controls

The OUYA build expects a controller, so the port hands it one — the pad goes
through as a real gamepad, not as synthetic fingers.

| Control | Does |
|---|---|
| Left stick | skate |
| **A** (right-hand button) | shoot, check, accept |
| **B** (bottom button) | secondary / back |
| **L1**, **R1** | shoulder buttons, also act in the menus |
| **Start** | the game's own menu |
| X, Y, L2, R2, stick clicks | passed through as Android gamepad keys |
| Select | nothing — the engine has no entry for it |

That last row is not an omission. The engine dispatches keys through a jump
table covering Android keycodes 19 to 107, so anything outside that range is
discarded without a trace — which rules out `BACK` (4), `BUTTON_START` (108)
and `BUTTON_SELECT` (109). All three look like the obvious choice for Start and
none of them reach the game; it sends `MENU` (82) instead.

**If the buttons land wrong**, your handheld is lettered Xbox style
(A at the bottom) rather than Nintendo style (A on the right). SDL names buttons
by position and the silkscreen disagrees, so set this in `Dead Space.sh`:

```sh
export DEADSPACE_FACE_LAYOUT="${DEADSPACE_FACE_LAYOUT:-xbox}"
```

## Requirements

- **armhf userland with 32-bit GPU libraries.** The game ships only an
  `armeabi-v7a` library, so the loader is 32-bit. Your CFW needs `CONFIG_COMPAT`
  in the kernel and 32-bit Mali libraries — the same bar as box86 and GMLoader.
  Devices without them (the TrimUI Smart Pro, for instance) cannot run this.
- **glibc 2.38+.**

Tested on an **R36S** (G80CA-MB V1.2, RK3326, Mali-G31) running dArkOSRE. That
is the only device this has run on, so anything else is unknown rather than
unsupported — reports welcome either way.

## Reporting a problem

The port writes **`ports/deadspace/log.txt`** on every run, and it is the only
diagnostic anyone gets off a handheld. Open an issue with:

- your device and CFW (e.g. "RG40XXH, muOS 2410")
- `log.txt` from the failed run
- what you saw on screen

Lines worth knowing about:

| In the log | Meaning |
|---|---|
| `missing game file` on screen | the APK is not where the port expects it |
| `GL: no Mali blob found` | no 32-bit GPU libraries on this CFW — the port cannot run here |
| `unresolved symbol` | the loader is missing a shim; please report it, it is fixable |
| `stopped presenting after N frames` | the game froze; include N |

## Building

Everything builds in a container, so the only dependency is Docker (or
[colima](https://github.com/abiosoft/colima) on macOS). The image is Debian
arm64 — native and fast on Apple Silicon — cross-compiling to
`arm-linux-gnueabihf`.

```bash
docker build -f Dockerfile.build -t deadspace-build .
docker run --rm -v "$PWD":/src -w /src deadspace-build make -j4
```

The binary lands at `build/deadspace`. To assemble the libraries that travel in
the zip:

```bash
docker run --rm -v "$PWD":/src -w /src deadspace-build tools/collect_libs.sh
```

`collect_libs.sh` is deliberately picky about what ships: glibc, the dynamic
linker and anything GPU-related must come from the device, never from the zip.

## Verifying without the console

`harness/verify.sh` runs the armhf binary under `qemu-arm` with Mesa llvmpipe
and reports which milestone it reaches — loads, starts, gets a GL context,
compiles shaders, opens assets, draws frames. It makes most iteration possible
without touching the SD card.

**It is not a substitute for the device.** It has no joystick, no Mali driver
and no KMSDRM, and it has produced convincing false results in both directions:
a freeze that existed only under software rendering, and a framebuffer change
that passed there and gave a black screen on Mali. Test on hardware as soon as
the game draws anything.

## Known issues

- The log carries a warning that **the music will be silent**. Ignore it: audio
  works on the device, music included. The game asks OpenSL ES for a second
  player fed a compressed stream that the platform is meant to decode, and this
  port has no decoder for that path — but the game does not depend on it, and
  decodes its own Ogg elsewhere. The warning describes that unused player, not
  what you hear.
- **The game asks for 1920x1080** on a 640x480 panel. It renders anyway, but the
  layout was not drawn for this aspect ratio. The port ships a 4:3 loading
  screen because the game's own is 16:9 and sat letterboxed.
- **OUYA store integration is stubbed** — purchases, receipts and the
  player-account lookups return empty, so the game behaves as if everything is
  unlocked and there is no network. `getPlayerNumByDeviceId` is part of that
  stub, so anything relying on per-controller player numbers is unverified.
- A few cosmetic textures are missing from this build of the game
  (`Texture not found: ...Santa_Hat.png`); they belong to content the APK does
  not carry.

## Credits and licence

Game by **Mountain Sheep**. Port by **EapRules**.

The ELF loader, the fake JNI and the bionic libc thunks derive from
[gmloader-next](https://github.com/JohnnyonFlame/gmloader-next) by JohnnyonFlame,
itself descended from Andy Nguyen's Vita so-loader.

Released under **GPL-3.0** — see [`LICENSE`](LICENSE), and [`NOTICE.md`](NOTICE.md)
for the full provenance of every borrowed part.
