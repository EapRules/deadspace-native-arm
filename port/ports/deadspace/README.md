# Dead Space — PortMaster port

Arcade ice hockey by Mountain Sheep (2012). This is **not an emulator**: the
game's own Android native library is loaded and run directly on Linux/ARM
through a NativeActivity loader, the same way GMLoader runs GameMaker games.

## Status: it boots and plays its menus. It is not finished.

Verified on real hardware: the game loads, renders on the Mali driver, plays its
sound effects and reaches its menus. A full match has **not** been played
through yet. Treat this as an early port, not a finished one — see *Known
issues* at the bottom for what is still open.

## Which APK — this one matters more than usual

Get the **OUYA build** of **`net.mountainsheep.deadspace`** — version **1.8**,
October 2013.

The Google Play build of this game is **touch-only, and this port cannot drive
it.** That is not a limitation that can be worked around with a better button
map: the engine in the controller build rejects touchscreen events outright,

```
[W/native-activity] WARNING: Unknown input source (4098)!
```

`4098` is `AINPUT_SOURCE_TOUCHSCREEN`. Feed it synthetic fingers and the menus
simply never respond, no matter what you press. The OUYA build expects a
controller, which is exactly what this handheld is.

1. Get the APK.
2. Rename it to **`deadspace.apk`**.
3. Put it in **`ports/deadspace/deadspace.apk`** in the same session in which you
   drop the zip into `autoinstall/`, while the card is still in your computer.

Nothing is downloaded and no game data ships in this zip.

## After installing: reboot, and stay out of Manage Ports

Two things that make a working install look broken:

**The game does not appear in Ports until you reboot the console.** PortMaster
installs it correctly and then says nothing: the autoinstall path never triggers
the frontend refresh, so EmulationStation keeps listing what it loaded at boot.
Reboot and it is there.

**Do not press *Reinstall* or *Uninstall* under Manage Ports.** Both re-download
from PortMaster's catalogue, where this port does not exist, so you get *"unable
to find a source for deadspace.zip"* — after the port has already been removed.
That turns a good install into an empty Ports menu, and it is a very easy thing
to do twice while trying to fix the first one. To reinstall, drop the zip into
`autoinstall/` again. *Uninstall* additionally deletes the port folder with
**your APK inside it**.

## Requirements

- **armhf userland with 32-bit GPU libraries.** The game ships only an
  `armeabi-v7a` library and no 64-bit build exists, so the loader is 32-bit.
  Your CFW needs `CONFIG_COMPAT` in the kernel and 32-bit Mali libraries — the
  same requirement as box86 and GMLoader. Devices without 32-bit GPU libraries
  (for example the TrimUI Smart Pro) cannot run this.
- **glibc 2.38 or newer.** The binary imports symbols up to `GLIBC_2.38`.

Tested on: R36S (G80CA-MB V1.2, RK3326, Mali-G31) running dArkOSRE. That is the
only device it has run on.

## Controls

The port talks to the game as a controller, which is what the OUYA build
expects. `DEADSPACE_INPUT=touch` switches to synthetic fingers, and is kept only
for experimenting with other builds of the game — on this one it does nothing
useful.

| Control | Does |
|---|---|
| Left stick | skate |
| Right stick | second axis pair, if the game asks for it |
| **A** (right-hand button) | primary action — shoot, check, accept |
| **B** (bottom button) | secondary action / back |
| **L1**, **R1** | shoulder buttons — these work in the menus |
| **Start** | open the game's own menu |
| X, Y, L2, R2, stick clicks | passed through as Android gamepad keys |
| Select | nothing — the engine has no entry for it |

The engine dispatches keys through a jump table covering Android keycodes 19 to
107, so anything outside that range is discarded without a trace. That rules out
`BACK` (4), `BUTTON_START` (108) and `BUTTON_SELECT` (109) — all three look like
reasonable choices and none of them reach the game. Start therefore sends
`MENU` (82), which does have an entry.

### If the buttons feel wrong

SDL names the face buttons by **position** — its "A" is always the bottom one —
while handhelds letter them however they like. These devices are lettered
Nintendo style, with A on the right, and the port assumes that. If yours is
lettered Xbox style (A at the bottom), edit `Dead Space.sh`:

```sh
export DEADSPACE_FACE_LAYOUT="${DEADSPACE_FACE_LAYOUT:-xbox}"
```

If a menu takes the face buttons in some other way entirely, there is a second
escape hatch that restores an older, more conservative mapping:

```sh
export DEADSPACE_FACE_KEYS="legacy"
```

`deadspace.gptk` leaves every button unbound on purpose. gptokeyb is present only
so PortMaster's standard exit combination can close the game — binding buttons
there would double every input.

## If it does not start

The port writes `ports/deadspace/log.txt` on every run. Useful lines:

| In the log | Meaning |
|---|---|
| `missing game file` on screen | the APK is not in place, see above |
| `Unknown input source (4098)` | you are running the Play build, not the OUYA one |
| `GL: no Mali blob found` | the port could not find 32-bit Mali libraries |
| `unresolved symbol` | the loader is missing a shim; please report it |
| `FATAL: SIGSEGV` | include the whole block, the addresses are the useful part |

## Known issues

- The log carries a warning that **the music will be silent**. Ignore it: audio
  works on the device, music included. The game asks OpenSL ES for a second
  player fed a compressed stream that the platform is meant to decode, and this
  port has no decoder for that path — but the game does not depend on it, and
  decodes its own Ogg elsewhere. The warning describes that unused player, not
  what you hear.
- **A full match has not been played through.** Everything past the menus is
  untested.
- **The game asks for 1920x1080** on a 640x480 panel (`Setting custom
  resolution`). It renders anyway, but the layout was not designed for this
  aspect ratio.
- **OUYA store integration is stubbed out** — purchases, receipts and the
  player-account lookups return empty. The game behaves as if everything is
  unlocked and there is no network. `getPlayerNumByDeviceId` is part of that
  stub, so anything relying on per-controller player numbers is unverified.
- A handful of cosmetic textures are missing from this build
  (`WARNING: Texture not found: ...Santa_Hat.png`); they belong to content this
  APK does not carry.

## Credits

Game by **Mountain Sheep**. This port bundles no game assets and does not
circumvent any protection: it loads an APK the user already owns.

The ELF loader, the JNI shim and the bionic libc thunks derive from
[gmloader-next](https://github.com/JohnnyonFlame/gmloader-next) by JohnnyonFlame
— see `LICENSE-gmloader.md`.
