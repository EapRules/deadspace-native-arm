# Dead Space — PortMaster test build

This runs the original Android ARM library directly on Linux/ARM. It is the
2011 EA/Visceral Dead Space mobile game, using the **Xperia Play v1.1.33**
binary. It is not an emulator and it is not the unrelated Mountain Sheep game
that an early scaffold in this repository described.

## Status

The immutable headless verifier reaches **M7/7**:

- 600 frames without a crash;
- 84 successful content opens;
- 11 texture uploads;
- more than 35,000 draw calls;
- a non-black framebuffer;
- synthetic JNI keys causing at least two measured scene changes.

Real R36S/Mali, audio, save data and a complete play-through still require
device testing.

## Your game files

No game binary or asset ships with this port. Extract your own Xperia Play
v1.1.33 copy and place these inside `ports/deadspace/`:

```text
deadspace/
├── assets/
│   ├── EAMCore.ini
│   └── published/
└── lib/
    └── armeabi/
        └── libEAMGameDeadSpace.so
```

The supported native library SHA1 is:

```text
0ed42b611415015807f759ec9b5457857143ce39
```

The launcher checks it before starting because the loader contains
build-specific patches.

## Controls

The loader uses the game's exported JNI input functions, matching the working
Vita port:

| R36S control | Android/game input |
|---|---|
| D-pad | D-pad |
| A | D-pad center / accept |
| B | Back |
| X / Y | Xperia Play X / Y |
| L1 / R1 | Shoulder buttons |
| Start | Start |
| Select | Select |
| Left stick | Virtual touchscreen movement stick |
| Right stick | Virtual touchpad aiming stick |

`deadspace.gptk` intentionally leaves game buttons unbound. It runs only so
PortMaster's standard exit combination can terminate the process.

## Diagnostics

Every launch replaces `ports/deadspace/log.txt`. Useful lines include:

- `TRACE: module loaded`
- `TRACE: mounted extracted content at /published`
- `TRACE: framebuffer non-black`
- `FATAL:` followed by registers and module-relative addresses

If the launcher rejects the game files, confirm both the directory layout and
the SHA1 above.

## Legal

This package contains only the loader, compatibility code and required
redistributable libraries. Supply game files from your own copy. Do not
redistribute `libEAMGameDeadSpace.so` or the extracted assets.
