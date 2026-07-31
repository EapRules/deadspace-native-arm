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
./emulator/send.sh screenshot menu
./emulator/send.sh quit
```

The loader consumes at most one command per frame. A down/up pair therefore
cannot collapse into a zero-duration input event even when a host tool appends
both lines immediately.

`DEADSPACE_CONTROL_DIR` and `DEADSPACE_GAMEDIR` can override both paths. The
MCP server uses this protocol; it does not need keyboard focus, X11, VNC or a
physical controller.

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
- `move_cursor` / `click` / `press_control`
- `capture_screen`
- `read_emulator_log`

Register the same project server in Claude Code with:

```bash
claude mcp add --scope user deadspace-emulator -- \
  python3 /Users/eaprules/Projects/Others/r36s-modding/deadspace/port/emulator/mcp_server.py
```

End-to-end validation:

```bash
./emulator/smoke_test_mcp.py
```

That smoke test initializes the protocol, rebuilds/starts qemu, obtains a real
640x480 PNG and stops only the runner it created.
