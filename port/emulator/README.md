# Dead Space local emulator control

This is the interactive companion to `harness/verify.sh`. The verifier remains
immutable and decides M1-M7; this runner keeps the same qemu-arm + Mesa process
alive and exposes deterministic host control.

Start it:

```bash
./emulator/run.sh
```

The default control directory is `emulator/runtime/`. It contains:

- `commands`: append-only input protocol;
- `status.json`: current state and frame number;
- `screenshots/*.png`: framebuffer captures;
- `emulator.log`: created by the MCP wrapper.

Manual examples:

```bash
./emulator/send.sh cursor 320 240
./emulator/send.sh click down
./emulator/send.sh click up
./emulator/send.sh button start down
./emulator/send.sh button start up
./emulator/send.sh button l2 down  # weapon-tilt gesture
./emulator/send.sh button l2 up
./emulator/send.sh button r2 down  # Zero-G motion gesture
./emulator/send.sh button r2 up
./emulator/send.sh stick right 1 0
./emulator/send.sh stick right 0 0
./emulator/send.sh screenshot menu
./emulator/send.sh quit
```

The loader consumes at most one command per frame. A down/up pair therefore
cannot collapse into a zero-duration input event even when a host tool appends
both lines immediately.

`DEADSPACE_CONTROL_DIR` and `DEADSPACE_GAMEDIR` can override both paths. The
MCP server uses this protocol; it does not need keyboard focus, X11, VNC or a
physical controller.

The local container uses SDL's dummy audio output. It consumes the exact queued
PCM at real time without requiring access to the Mac audio device; the log
reports the obtained format and bounded PCM signal metrics for audio debugging.
`DEADSPACE_VFP_SELFTEST=1` makes qemu execute each original short-vector opcode
and its scalar expansion from identical VFP register state. It must report
40/40 exact. `analysis/vfp_coverage.py` independently proves that the patch
list covers every arithmetic opcode in all 20 LEN regions. The
`DEADSPACE_NO_VFP_PATCH=1` switch is diagnostic only; PCM digests are useful
for queue telemetry but diverge with elapsed game time after startup silence,
so they are not the arithmetic oracle. The R36S requires the patch.

For per-call GLES error attribution:

```bash
DEADSPACE_GL_DIAG=1 ./emulator/run.sh
```

Diagnostic mode routes every typed GLES entry through an observation hook and
checks `glGetError` immediately around the real driver call. It is opt-in
because reading the error queue is observable to the game. This mode identified
the rejected PVRTC uploads that caused the white 3D scene.

## MCP tools

`mcp_server.py` is a dependency-free stdio MCP server. It exposes:

- `start_emulator` / `stop_emulator`
- `emulator_status`
- `move_cursor` / `click` / `press_control` / `set_stick`
- `capture_screen`
- `read_emulator_log`

Register it in any MCP client as a stdio server:

```bash
python3 port/emulator/mcp_server.py
```

End-to-end validation:

```bash
./emulator/smoke_test_mcp.py
```

That smoke test initializes the protocol, rebuilds/starts qemu, obtains a real
640x480 PNG and stops only the runner it created.
