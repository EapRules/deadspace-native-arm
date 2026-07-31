# Dead Space — PortMaster test build

This runs the original Android ARM library directly on Linux/ARM. It is the
2011 EA/Visceral Dead Space mobile game, using the **Xperia Play v1.1.33**
binary. It is not an emulator and it is not the unrelated Mountain Sheep game
that an early scaffold in this repository described.

## Status

The immutable headless verifier reaches **M7/7**:

- 570 frames without a crash in the latest run;
- 84 successful content opens;
- 151 texture uploads;
- 35,049 draw calls;
- a non-black framebuffer;
- synthetic JNI keys causing three measured scene changes.

Real R36S testing confirms centred 640x480 output, a working menu cursor and
working pad input. The white 3D scene was traced to PVRTC textures unsupported
by Mali-G31. This build decodes them to RGBA8888 in software; the corrected
3D scene is now confirmed on the real R36S. This candidate also replaces the
cross cursor with an arrow, restores it with L3/R3 or Start, sustains camera
movement while the right stick is held and repairs the previously unopened
SDL audio path. Audio, these new input changes, save data and a complete
play-through still require testing on the device.

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
| D-pad | Move visible cursor while it is shown |
| A | Touch/click at cursor; game A when cursor is hidden |
| B | Back |
| X / Y | Xperia Play X / Y |
| L1 / R1 | Shoulder buttons |
| Start | Start |
| Select | Select |
| L3 / R3 | Show/hide menu cursor |
| Left stick | Virtual touchscreen movement stick |
| Right stick | Virtual touchpad aiming stick |

Dead Space's menus are touch-only even in the working Vita port. The cursor
starts at the centre of the title screen. Move it with the D-pad and press A
to tap. Moving either analog stick hides it for gameplay; press L3 or R3 to
bring it back. Start also restores it while opening the pause menu. The right
stick is translated into a fresh touchpad camera gesture every frame so a held
direction keeps turning continuously.

`deadspace.gptk` intentionally leaves game buttons unbound. It runs only so
PortMaster's standard exit combination can terminate the process.

## Diagnostics

Every launch replaces `ports/deadspace/log.txt`. Useful lines include:

- `TRACE: module loaded`
- `TRACE: mounted extracted content at /published`
- `TRACE: VFP short vectors: expanded 40/40 audio instructions`
- `TRACE: AudioTrack: SDL audio ready driver=...`
- `TRACE: AudioTrack: opened device=...`
- `TRACE: AudioTrack: PCM write=...`
- `TRACE: framebuffer non-black`
- `FATAL:` followed by registers and module-relative addresses

If the launcher rejects the game files, confirm both the directory layout and
the SHA1 above.

## Legal

This package contains only the loader, compatibility code and required
redistributable libraries. Supply game files from your own copy. Do not
redistribute `libEAMGameDeadSpace.so` or the extracted assets.
