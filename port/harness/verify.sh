#!/usr/bin/env bash
#
# Autonomous verification harness for the Dead Space port.
#
# This is what lets the loop run without a human: it builds the armhf loader,
# runs it under qemu-arm with software GLES2, and decides objectively which
# milestone the port currently reaches. No console, no SD card, no eyeballs.
#
# Exit code = highest milestone passed (0 = nothing builds). The loop reads it
# to know whether the last iteration moved forward, stalled, or regressed.
#
# Usage:  harness/verify.sh [--milestone N] [--timeout SECS]
set -uo pipefail

PORT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
IMAGE="deadspace-build"
APK="${DEADSPACE_APK:-$PORT_DIR/../deadspace_play.apk}"
TIMEOUT="${TIMEOUT:-90}"
RESULTS="$PORT_DIR/harness/results"
mkdir -p "$RESULTS"

log()  { echo "[verify] $*"; }
fail() { echo "[verify] FAIL: $*"; }

# Milestones, in order. Each one is a hard, observable fact — not an opinion.
#   1 build      loader links into an armhf binary
#   2 load       libdeadspace.so loads with every symbol resolved
#   3 oncreate   ANativeActivity_onCreate returns without crashing
#   4 gl         EGL context created and the game's shaders compile
#   5 frames     at least MIN_FRAMES eglSwapBuffers calls
#   6 stable     survives TIMEOUT seconds and draws a non-black frame
MIN_FRAMES="${MIN_FRAMES:-30}"

REACHED=0

# ---------------------------------------------------------------- 1. build
log "M1 build"
if docker run --rm -v "$PORT_DIR":/src -w /src "$IMAGE" make -j"$(nproc 2>/dev/null || echo 4)" \
     > "$RESULTS/01-build.log" 2>&1; then
    if docker run --rm -v "$PORT_DIR":/src -w /src "$IMAGE" \
         file build/deadspace 2>/dev/null | grep -q "ELF 32-bit.*ARM"; then
        REACHED=1
        log "M1 ok"
    else
        fail "build produced no armhf binary"
    fi
else
    fail "compile error, see 01-build.log"
    tail -20 "$RESULTS/01-build.log"
    exit 0
fi

# The APK is the user's own copy and never ships with the port.
if [ ! -f "$APK" ]; then
    fail "APK not found at $APK (set DEADSPACE_APK)"
    exit $REACHED
fi

# ------------------------------------------------- 2-6. run under emulation
# LOADER_TRACE makes the loader print one line per milestone so the harness can
# assert on facts rather than on the absence of a crash.
log "M2-M6 running under qemu-arm (timeout ${TIMEOUT}s)"

docker run --rm \
    -v "$PORT_DIR":/src \
    -v "$(dirname "$APK")":/apk:ro \
    -w /src \
    -e SDL_VIDEODRIVER=offscreen \
    -e LIBGL_ALWAYS_SOFTWARE=1 \
    -e GALLIUM_DRIVER=llvmpipe \
    -e EGL_PLATFORM=surfaceless \
    -e LOADER_TRACE=1 \
    -e DEADSPACE_FRAME_LIMIT="$((MIN_FRAMES * 4))" \
    "$IMAGE" \
    timeout --signal=INT "$TIMEOUT" \
    qemu-arm -L /usr/arm-linux-gnueabihf \
        ./build/deadspace "/apk/$(basename "$APK")" \
    > "$RESULTS/02-run.log" 2>&1
RUN_RC=$?

RUN_LOG="$RESULTS/02-run.log"

# --- M2: every import resolved. An unresolved symbol means a missing shim,
#         which is the single most common failure mode for this kind of port.
if grep -q "TRACE: module loaded" "$RUN_LOG"; then
    UNRESOLVED=$(grep -c "unresolved symbol" "$RUN_LOG" || true)
    if [ "$UNRESOLVED" -eq 0 ]; then
        REACHED=2; log "M2 ok (0 unresolved symbols)"
    else
        fail "M2: $UNRESOLVED unresolved symbols"
        grep "unresolved symbol" "$RUN_LOG" | sort -u | head -20
    fi
else
    fail "M2: module never loaded"
fi

# --- M3
if [ $REACHED -ge 2 ] && grep -q "TRACE: onCreate returned" "$RUN_LOG"; then
    REACHED=3; log "M3 ok"
elif [ $REACHED -ge 2 ]; then
    fail "M3: onCreate did not return (crash or hang)"
fi

# --- M4
if [ $REACHED -ge 3 ] && grep -q "TRACE: gl ready" "$RUN_LOG"; then
    REACHED=4; log "M4 ok"
elif [ $REACHED -ge 3 ]; then
    fail "M4: no GL context / shader compile failed"
    grep -iE "shader|egl error|glGetError" "$RUN_LOG" | head -10
fi

# --- M5
FRAMES=$(grep -c "TRACE: frame" "$RUN_LOG" || true)
if [ $REACHED -ge 4 ] && [ "$FRAMES" -ge "$MIN_FRAMES" ]; then
    REACHED=5; log "M5 ok ($FRAMES frames)"
elif [ $REACHED -ge 4 ]; then
    fail "M5: only $FRAMES frames (need $MIN_FRAMES)"
fi

# --- M6: the game is actually running its content, not just holding a surface.
#
# The first version of this check accepted "survived + non-black framebuffer".
# That was too weak and it passed a port that was not working: the engine booted
# and cleared the screen to a colour, but opened 0 assets, linked 0 GLSL programs
# and issued 0 draw calls. A solid-colour clear is indistinguishable from a
# rendered game by the pixel test alone.
#
# So M6 now demands evidence that the engine got past initialisation and is
# drawing its own content. These thresholds are deliberately far above what an
# init path can produce by accident:
#   assets   - the game has 1180 of them; loading a level touches dozens
#   programs - the engine builds a shader program per material
#   draws    - one frame of gameplay is many draw calls
#
# The loader must emit, at the end of the run:
#   TRACE: summary assets=<n> programs=<n> draws=<n>
MIN_ASSETS="${MIN_ASSETS:-50}"
MIN_PROGRAMS="${MIN_PROGRAMS:-5}"
MIN_DRAWS="${MIN_DRAWS:-100}"

if [ $REACHED -ge 5 ]; then
    NONBLACK=$(grep -c "TRACE: framebuffer non-black" "$RUN_LOG" || true)
    SUMMARY=$(grep -oE "TRACE: summary assets=[0-9]+ programs=[0-9]+ draws=[0-9]+" "$RUN_LOG" | tail -1)

    if [ -z "$SUMMARY" ]; then
        fail "M6: loader emitted no summary line (need: TRACE: summary assets=N programs=N draws=N)"
    else
        A=$(echo "$SUMMARY" | grep -oE "assets=[0-9]+"   | cut -d= -f2)
        P=$(echo "$SUMMARY" | grep -oE "programs=[0-9]+" | cut -d= -f2)
        D=$(echo "$SUMMARY" | grep -oE "draws=[0-9]+"    | cut -d= -f2)
        log "M6 counters: assets=$A programs=$P draws=$D nonblack=$NONBLACK"

        # 124 == timeout killed it, which is success for a game loop
        ALIVE=0
        { [ $RUN_RC -eq 124 ] || [ $RUN_RC -eq 0 ]; } && ALIVE=1

        if [ $ALIVE -eq 1 ] && [ "$NONBLACK" -gt 0 ] \
           && [ "$A" -ge "$MIN_ASSETS" ] && [ "$P" -ge "$MIN_PROGRAMS" ] && [ "$D" -ge "$MIN_DRAWS" ]; then
            REACHED=6
            log "M6 ok (assets=$A programs=$P draws=$D, survived)"
        else
            [ $ALIVE -eq 0 ]             && { fail "M6: exited early rc=$RUN_RC"; grep -iE "segfault|abort|fatal|signal" "$RUN_LOG" | head -10; }
            [ "$NONBLACK" -eq 0 ]        && fail "M6: renders only black frames"
            [ "$A" -lt "$MIN_ASSETS" ]   && fail "M6: only $A assets opened (need $MIN_ASSETS) - the engine never loaded content"
            [ "$P" -lt "$MIN_PROGRAMS" ] && fail "M6: only $P GLSL programs linked (need $MIN_PROGRAMS) - not rendering its own materials"
            [ "$D" -lt "$MIN_DRAWS" ]    && fail "M6: only $D draw calls (need $MIN_DRAWS) - nothing is being drawn"
            grep -iE "missing|no class|not found" "$RUN_LOG" | sort -u | head -8
        fi
    fi
fi

echo "$REACHED" > "$RESULTS/milestone"
log "=== milestone reached: $REACHED / 6 ==="
exit $REACHED
