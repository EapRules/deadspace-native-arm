# Dead Space — native ARM port

Runs the 2011 EA/Visceral **Dead Space mobile** game on Linux/ARM handhelds by
loading its original Android native library into a bionic/JNI compatibility
layer. No emulator or Android runtime is involved.

This directory targets the **Xperia Play v1.1.33** build:

```text
lib/armeabi/libEAMGameDeadSpace.so
SHA1 0ed42b611415015807f759ec9b5457857143ce39
```

It is not the unrelated Mountain Sheep/OUYA game described by an early
scaffold that used to occupy some of this repository.

## Current status

The immutable harness reaches **M7/7** under qemu-arm + llvmpipe. The latest
PVRTC-fallback run reported:

```text
570 frames
84 successful content opens
151 texture uploads
35,049 draw calls
non-black framebuffer
11 synthetic JNI keys
3 measured scene changes
```

That proves startup, content loading, rendering and input-driven progression in
the harness. The PVRTC fallback is also confirmed on the real R36S Mali-G31:
the previously white characters, objects and backgrounds now render correctly.
Audio, saves and a complete play-through still require device testing.

The full investigation history and explicit split between Claude's work and
ChatGPT/Codex's M4→M7 work is in [`../TRASPASO.md`](../TRASPASO.md).

## Bring your own game

This repository and its packages contain no EA binary or asset. Extract your
own supported copy and give the loader a directory with:

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

The loader's runtime patches validate their expected instructions, and the
PortMaster launcher also checks the complete library SHA1.

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

The zip is written to `build/deadspace-portmaster.zip`. It intentionally
contains neither `libEAMGameDeadSpace.so` nor `assets/published`.

## PortMaster install

Install the package, then copy your extracted `assets/`, `lib/` and optional
`var/` into `ports/deadspace/`. The resulting layout is:

```text
ports/
├── Dead Space.sh
└── deadspace/
    ├── deadspace
    ├── deadspace.gptk
    ├── libs.armhf/
    ├── assets/                # your copy
    ├── lib/armeabi/           # your copy
    └── var/                   # saves/settings
```

The launcher follows PortMaster's `directory` variable, so it works from
either `/roms` or `/roms2`.

## Controls

This binary has no `AInputQueue` imports. Input is delivered through its
exported JNI entry points, matching the working Vita port:

- title/menus: D-pad moves a visible software cursor, A taps it
- L3 or R3 toggles the menu cursor after it has been dismissed
- Start restores the cursor while opening the pause menu
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
per-frame right-touchpad gesture. These changes are locally verified and await
the next R36S test. Audio remains the outstanding subsystem.

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
TRACE: framebuffer non-black
TRACE: summary assets=N textures=N draws=N
FATAL: ...
```

Never redistribute the game `.so` or extracted assets.
