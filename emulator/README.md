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

`start_emulator` also accepts `game_dir`, `gl_diag` and `mali_compat`. This lets
the MCP run an extracted Full or RIP donor directly and collect per-upload
diagnostics. `mali_compat: true` enables the same GLES1 upload diagnostics used
to validate the R36S Mali path, while screenshots remain real 640x480 frames.

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

## Donor compatibility matrix

The compatibility runner starts from each immutable donor on every repetition;
it never restores an already-extracted tree. The original 7z is unpacked on the
host, while ZIP donors are handed directly to eapx. All three are classified by
correlated content hashes before qemu starts.

The current matrix intentionally includes one negative compatibility case:
the donor still named `full-pvrtc-repacked` internally is actually ETC1. It
must fail C2/C4 until ETC1 support is implemented; its presence prevents the
known black-model archive from being mistaken for a working Full PVRTC donor.
See [`../DONOR_COMPATIBILITY.md`](../DONOR_COMPATIBILITY.md).

To keep navigation deterministic, the visual runner copies a local save fixture
into the otherwise-clean temporary install. Place the console's known-good
`var` files in `emulator/fixtures/save/`, or pass `--save-fixture PATH`. This
fixture is ignored by Git and is never included in the port. Assets and the
native library are still extracted afresh from the selected donor every run.

```bash
./emulator/compatibility_test.py --all --repeat 1 --frame-limit 3800
./emulator/compatibility_test.py --all --release-gate
```

The first command is the development loop. `--release-gate` performs three
clean repetitions of every profile. Reports and framebuffer captures live
under `emulator/runtime/compatibility/<run-id>/`. C1 checks the donor profile,
C2 checks per-format texture telemetry, C3 requires all configured frames, and C4/C5
require Isaac to match three local visual oracles.

Golden PNGs live under ignored `emulator/goldens/<profile>/`; they must never be
committed or shipped because they contain game imagery. To bootstrap a locally
confirmed donor:

```bash
./emulator/compatibility_test.py --profile vita-rip-atc \
  --record-golden vita-rip-atc
```

The MCP server exposes the same workflow through `run_compatibility_matrix`,
`compatibility_status` and `read_compatibility_report`.
