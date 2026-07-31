# Dead Space — native ARM port

Runs the 2011 EA/IronMonkey **Dead Space mobile** game on Linux/ARM handhelds by
loading its original Android native library into a bionic/JNI compatibility
layer. No emulator or Android runtime is involved.

**Port and project by [EapRules](https://github.com/EapRules).**

This directory targets the **Xperia Play v1.1.33** build:

```text
lib/armeabi/libEAMGameDeadSpace.so
SHA1 0ed42b611415015807f759ec9b5457857143ce39
```

It is not the unrelated Mountain Sheep/OUYA game described by an early
scaffold that used to occupy some of this repository.

## Current status

The immutable harness reaches **M7/7** under qemu-arm + llvmpipe. The latest
run, with real-time dummy audio consumption and the ARMv8 VFP compatibility
patch enabled, reported:

```text
570 frames
84 successful content opens
151 texture uploads
35,511 draw calls
non-black framebuffer
11 synthetic JNI keys
4 measured scene changes
```

That proves startup, content loading, rendering and input-driven progression in
the harness. The PVRTC fallback is also confirmed on the real R36S Mali-G31:
the previously white characters, objects and backgrounds now render correctly.
The current candidate also initializes SDL audio, uses a bounded hardware
period and expands the obsolete VFP short-vector mixer operations for ARMv8.
Audio output, the new cursor/camera behavior, L2/R2 accelerometer gestures,
saves and a complete play-through still require device testing.

The full investigation history and explicit split between Claude's work and
ChatGPT/Codex's M4→M7 work is in [`../TRASPASO.md`](../TRASPASO.md).

## Bring your own game

This repository and its packages contain no EA binary or asset. The supported
donor is **Dead Space Mobile for Xperia Play v1.1.33**, Android package
`com.eamobile.deadspace_sonyericsson`. Local developer runs give the loader an
already-extracted directory with:

```text
deadspace/
├── assets/
│   ├── EAMCore.ini
│   └── published/
└── lib/
    └── armeabi/
        └── libEAMGameDeadSpace.so
```

Run locally as:

```bash
./build/deadspace /path/to/extracted/deadspace
```

The PortMaster release also carries `eapx`, a content-based transactional
first-boot extractor. Release users may place the supported APK, ZIP or an
extracted folder in `ports/deadspace/`; its filename does not matter. eapx
stages and validates the complete payload before publishing it, and the
launcher still checks the complete library SHA1 before applying patches. The
known 145,903,794-byte Vita-ready `deadspace.zip` is accepted explicitly as a
reduced donor: it keeps the campaign but omits the optional `~2x` UI, Survival
maps and Burst Rifle files. A complete Xperia Play tree remains supported.

## Build and verify

```bash
docker run --rm -v "$PWD":/src -w /src deadspace-build make -j4
timeout 400 harness/verify.sh
```

`harness/verify.sh` is the read-only arbiter. Do not modify it or lower its
thresholds.

To collect the redistributable ARM dependencies and make the game-data-free
PortMaster zip:

```bash
docker run --rm -v "$PWD":/src -w /src deadspace-build make libs
./package_portmaster.sh
```

The zip is written to `build/deadspace.zip`. It intentionally
contains neither `libEAMGameDeadSpace.so` nor `assets/published`.

## PortMaster install

Use the same independent-autoinstall workflow as the Ice Rage and Minigore 2
releases:

1. put our `deadspace-portmaster.zip` in PortMaster's `autoinstall/` directory;
2. put the user's Xperia Play v1.1.33 APK/ZIP in `ports/deadspace/` (or an
   extracted donor under `ports/deadspace/gamedata/`);
3. open PortMaster and let autoinstall finish;
4. reboot, then launch Dead Space from Ports.

The first launch extracts and validates the donor automatically. Complete
CFW-specific paths, the two autoinstall gotchas and accepted donor layouts are
in [`ports/deadspace/README.md`](ports/deadspace/README.md). The resulting
layout is:

```text
ports/
├── Dead Space.sh
└── deadspace/
    ├── deadspace
    ├── deadspace.gptk
    ├── eapx.py
    ├── deadspace.eapx.json
    ├── libs.armhf/
    ├── assets/                # your copy
    ├── lib/armeabi/           # your copy
    └── var/                   # saves/settings
```

The launcher follows PortMaster's `directory` variable, so it works from
either `/roms` or `/roms2`. It also refreshes only Dead Space's normalized
ArkOS artwork at `ports/images/Dead Space.png` when a direct update left an old
APK icon cached there; the canonical portable cover remains
`deadspace/cover.png` as required by PortMaster's `gameinfo.xml` format.

## Controls

This binary has no `AInputQueue` imports. Input is delivered through its
exported JNI entry points, matching the working Vita port:

- title/menus: D-pad moves a visible software cursor, A taps it
- L3 or R3 toggles the menu cursor after it has been dismissed
- Start restores the cursor while opening the pause menu
- L2 simulates the accelerometer tilt used to rotate/switch weapon fire mode
- R2 simulates the accelerometer motion required for a Zero-G jump
- buttons outside cursor mode → `KeyboardAndroid.NativeOnKeyDown/Up`
- left stick → virtual touchscreen movement stick
- right stick → virtual touchpad aiming stick

The original game's menus do not support gamepad navigation; the Vita port
uses its physical touchscreen. The cursor is therefore a required input
bridge on non-touch PortMaster handhelds, not optional decoration. Moving
either analog stick dismisses it so the same controls can drive gameplay.

The pointer callback uses base AAPCS because the game is softfp and the loader
is hardfp.

## Graphics and real-device status

The first `d4ca229` build was tested on an R36S with its Mali-G31 driver:

- the image is correctly centred at 640x480;
- the D-pad cursor and physical controls work and can advance through menus;
- menu UI is visible;
- 3D characters, objects and backgrounds render mostly white, sometimes with
  only an edge, shadow or silhouette visible;
- audio is not working yet.

The interactive emulator reproduced that exact visual failure. Per-call GL
diagnostics identified rejected `glCompressedTexImage2D` uploads:
`0x8c00/0x8c02` are PVRTC1 4bpp RGB/RGBA, formats unsupported by both llvmpipe
and Mali-G31. The loader now uses Imagination's MIT-licensed decoder and
uploads RGBA8888 when the driver does not advertise native PVRTC.

A subsequent local capture rendered the complete menu environment with its
textures, lighting and materials, and the immutable harness remained M7/7. The
candidate with SHA-256
`9199544a9db9113e20facac61fb518dfc892beff35f17156ff3e313924a015da`
was then tested on the real R36S and the user confirmed that the full 3D scene
also renders correctly there.

That hardware pass exposed two isolated input issues in the otherwise working
controls: the provisional cross cursor could not be recovered after analog
input, and a held right stick produced only one finite camera gesture. The next
candidate replaces the cross with a high-contrast arrow, restores it with
L3/R3 or Start, refreshes sticks every frame and reproduces the Vita port's
per-frame right-touchpad gesture.

The same candidate addresses the silent audio path. `AudioTrack` used to call
`SDL_OpenAudioDevice` without ever initializing `SDL_INIT_AUDIO`, then silently
kept device ID zero. It also confused the engine's 1 MiB producer ring with a
hardware period and requested a roughly six-second buffer. Audio now has
explicit initialization, device enumeration/fallback and a 1024-frame period.
The game's 40 obsolete VFP short-vector mixer instructions are expanded into
validated scalar A32 trampolines for the Cortex-A35, which ignores FPSCR
LEN/STRIDE. A qemu register-level self-test proves all 40 expansions reproduce
the original vector operations, while a disassembly audit proves the list
covers all 40 arithmetic opcodes in all 20 LEN regions. The local dummy device
confirms the queue is consumed in real time; audible speaker output awaits the
R36S test.

Current device-test binary:

```text
SHA256 d0ba9983a13a1cf7dbd7a7c5c26d57d544cd9e435ffe80308a03799ea20390de
size   7215740 bytes
```

## Interactive local emulator

`emulator/run.sh` keeps the qemu-arm + Mesa build alive and exposes cursor,
touch, controls, screenshots and logs through a shared control directory.
`emulator/mcp_server.py` publishes the same operations as an MCP server for
Claude Code. See `emulator/README.md`.

This path reproduced the real-device graphics failure locally and then
verified the PVRTC software fallback visually: the same menu now has a fully
textured 3D environment. The immutable M1-M7 harness remains separate and
unchanged.

## Diagnostics

Every device launch writes `ports/deadspace/log.txt`. Important lines:

```text
TRACE: module loaded
TRACE: mounted extracted content at /published
TRACE: VFP short vectors: expanded 40/40 audio instructions
TRACE: AudioTrack: SDL audio ready driver=...
TRACE: AudioTrack: opened device=...
TRACE: AudioTrack: PCM write=...
TRACE: framebuffer non-black
TRACE: summary assets=N textures=N draws=N
FATAL: ...
```

Never redistribute the game `.so` or extracted assets.
