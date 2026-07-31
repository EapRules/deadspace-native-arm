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

The immutable harness reaches **M7/7** under qemu-arm + llvmpipe:

```text
600 frames
84 successful content opens
11 texture uploads
more than 35,000 draw calls
non-black framebuffer
12 synthetic JNI keys
2 measured scene changes
```

That proves startup, content loading, rendering and input-driven progression in
the harness. R36S/Mali, audio, saves and a complete play-through still require
device testing.

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
- L3 toggles the menu cursor after it has been dismissed
- buttons outside cursor mode → `KeyboardAndroid.NativeOnKeyDown/Up`
- left stick → virtual touchscreen movement stick
- right stick → virtual touchpad aiming stick

The original game's menus do not support gamepad navigation; the Vita port
uses its physical touchscreen. The cursor is therefore a required input
bridge on non-touch PortMaster handhelds, not optional decoration. Moving
either analog stick dismisses it so the same controls can drive gameplay.

The pointer callback uses base AAPCS because the game is softfp and the loader
is hardfp.

## Real-device status

The `d4ca229` build was tested on an R36S with its Mali-G31 driver:

- the image is correctly centred at 640x480;
- the D-pad cursor and physical controls work and can advance through menus;
- menu UI is visible;
- 3D characters, objects and backgrounds render mostly white, sometimes with
  only an edge, shadow or silhouette visible;
- audio is not working yet.

The next graphics milestone is therefore correct GLES1 material/texturing on
Mali, not window geometry or input. Audio remains intentionally deferred.

## Interactive local emulator

`emulator/run.sh` keeps the qemu-arm + Mesa build alive and exposes cursor,
touch, controls, screenshots and logs through a shared control directory.
`emulator/mcp_server.py` publishes the same operations as an MCP server for
Claude Code. See `emulator/README.md`.

This path already reproduces the real-device graphics failure locally: menu
UI renders correctly while the 3D background/model is mostly white with only
edges and shadows. The immutable M1-M7 harness remains separate and unchanged.

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
