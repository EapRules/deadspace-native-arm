#!/usr/bin/env bash
#
# Autonomous verification harness for the Dead Space port.
#
# This is what lets the loop run without a human: it builds the armhf loader,
# runs it under qemu-arm with software GLES, and decides objectively which
# milestone the port currently reaches. No console, no SD card, no eyeballs.
#
# Exit code = highest milestone passed (0 = nothing builds). The loop reads it
# to know whether the last iteration moved forward, stalled, or regressed.
#
# ---------------------------------------------------------------------------
# Why this file is NOT the one from Minigore 2 / Ice Rage
#
# Those two are NativeActivity games: the engine imports libandroid.so, runs
# its own frame loop on its own thread, and the loader only feeds it lifecycle
# commands. Dead Space is the other model entirely - it imports no libandroid,
# exports JNI_OnLoad and 68 Java_* entry points, and expects a Java layer to
# drive it frame by frame. Here that Java layer is us.
#
# So three of the old milestones asserted on facts that can never be true:
#   - ANativeActivity_onCreate: the symbol does not exist in this .so
#   - an APK to load from: the game ships as a directory tree, not a zip
#   - GLSL programs linked: this is GLES 1.1 fixed function, zero shaders
#
# A harness copied across without that adjustment does not fail loudly - it
# fails *quietly*, pinning the port at M2 forever while the loop burns
# iterations trying to satisfy a check about a different game.
# ---------------------------------------------------------------------------
#
# Usage:  harness/verify.sh [--timeout SECS]
set -uo pipefail

PORT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
IMAGE="deadspace-build"

# The game is the user's own copy and never ships with the port. It is a
# directory, not an APK: EA stopped serving Dead Space years ago and what
# circulates is the extracted tree (lib/ + assets/published/).
GAMEDIR="${DEADSPACE_GAMEDIR:-$PORT_DIR/../NeededFiles/data/deadspace}"

TIMEOUT="${TIMEOUT:-120}"
RESULTS="$PORT_DIR/harness/results"
mkdir -p "$RESULTS"

log()  { echo "[verify] $*"; }
fail() { echo "[verify] FAIL: $*"; }

# Milestones, in order. Each one is a hard, observable fact - not an opinion.
#   1 build     loader links into an armhf binary
#   2 load      libEAMGameDeadSpace.so maps with every symbol resolved
#   3 jni       JNI_OnLoad and NativeOnCreate both return
#   4 surface   NativeOnSurfaceCreated returns on a live GLES 1.1 context
#   5 frames    at least MIN_FRAMES NativeOnDrawFrame calls, presented
#   6 content   the engine loaded its own data and is drawing it
#   7 autopilot the game *advances* when fed input, it is not parked in a menu
MIN_FRAMES="${MIN_FRAMES:-60}"

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
    echo "$REACHED" > "$RESULTS/milestone"
    exit 0
fi

if [ ! -d "$GAMEDIR" ]; then
    fail "game directory not found at $GAMEDIR (set DEADSPACE_GAMEDIR)"
    echo "$REACHED" > "$RESULTS/milestone"
    exit $REACHED
fi

# The sha1 the working Vita port pins. A different build has different function
# offsets, so every binary patch in the loader would land in the wrong place -
# and the resulting crash would say nothing about why.
SO_FILE="$GAMEDIR/lib/armeabi/libEAMGameDeadSpace.so"
EXPECT_SHA1="0ed42b611415015807f759ec9b5457857143ce39"
if [ -f "$SO_FILE" ]; then
    GOT_SHA1=$(shasum -a 1 "$SO_FILE" 2>/dev/null | cut -d' ' -f1 | tr 'A-Z' 'a-z')
    if [ "$GOT_SHA1" != "$EXPECT_SHA1" ]; then
        fail "wrong game build: sha1 $GOT_SHA1, expected $EXPECT_SHA1 (Xperia Play v1.1.33)"
        echo "$REACHED" > "$RESULTS/milestone"
        exit $REACHED
    fi
else
    fail "no $SO_FILE - this does not look like the extracted Dead Space tree"
    echo "$REACHED" > "$RESULTS/milestone"
    exit $REACHED
fi

# ------------------------------------------------- 2-7. run under emulation
# LOADER_TRACE makes the loader print one line per milestone so the harness can
# assert on facts rather than on the absence of a crash.
#
# DEADSPACE_AUTOPILOT feeds synthetic input so M7 has something to measure:
# a game that only *draws* proves rendering, not input-driven progression.
log "M2-M7 running under qemu-arm (timeout ${TIMEOUT}s)"

docker run --rm \
    -v "$PORT_DIR":/src \
    -v "$GAMEDIR":/game:ro \
    -w /src \
    -e SDL_VIDEODRIVER=offscreen \
    -e LIBGL_ALWAYS_SOFTWARE=1 \
    -e GALLIUM_DRIVER=llvmpipe \
    -e EGL_PLATFORM=surfaceless \
    -e LOADER_TRACE=1 \
    -e DEADSPACE_AUTOPILOT=1 \
    -e DEADSPACE_FRAME_LIMIT="$((MIN_FRAMES * 10))" \
    "$IMAGE" \
    timeout --signal=INT "$TIMEOUT" \
    qemu-arm -L /usr/arm-linux-gnueabihf \
        ./build/deadspace /game \
    > "$RESULTS/02-run.log" 2>&1
RUN_RC=$?

RUN_LOG="$RESULTS/02-run.log"

# --- M2: every import resolved. An unresolved symbol means a missing shim,
#         which is the single most common failure mode for this kind of port.
#         Dead Space declares 393 of them, 190 of which are gl*.
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

# --- M3: the JNI handshake. Both calls have to *return*, not merely be made:
#         JNI_OnLoad walks the fake class registry and a missing class shows up
#         as a hang or a fault inside it, never as a diagnostic.
if [ $REACHED -ge 2 ]; then
    if grep -q "TRACE: JNI_OnLoad returned" "$RUN_LOG" \
       && grep -q "TRACE: NativeOnCreate returned" "$RUN_LOG"; then
        REACHED=3; log "M3 ok"
    else
        grep -q "TRACE: JNI_OnLoad returned" "$RUN_LOG" \
            || fail "M3: JNI_OnLoad did not return (missing class in the fake registry?)"
        grep -q "TRACE: NativeOnCreate returned" "$RUN_LOG" \
            || fail "M3: NativeOnCreate did not return"
        grep -iE "no class|no method|no field|jni" "$RUN_LOG" | sort -u | head -10
    fi
fi

# --- M4: a live GLES 1.1 context the engine accepted.
#
# Not GLES2. The game imports 190 fixed-function entry points and zero shader
# ones; a GLES2-only context resolves glMatrixMode to null and the first
# NativeOnDrawFrame jumps to address 0.
if [ $REACHED -ge 3 ]; then
    if grep -q "TRACE: surface created" "$RUN_LOG"; then
        REACHED=4; log "M4 ok"
    else
        fail "M4: NativeOnSurfaceCreated never returned - no usable GLES 1.1 context"
        grep -iE "GL_VERSION|GL_RENDERER|egl error|glGetError|GLESv1" "$RUN_LOG" | head -10
    fi
fi

# --- M5: it is presenting. The loader owns this loop, so a frame count that
#         stays at zero means our driver never called in, not that the engine
#         stalled - the two are worth telling apart in the log.
FRAMES=$(grep -oE "TRACE: frames=[0-9]+" "$RUN_LOG" | tail -1 | cut -d= -f2)
FRAMES="${FRAMES:-0}"
if [ $REACHED -ge 4 ] && [ "$FRAMES" -ge "$MIN_FRAMES" ]; then
    REACHED=5; log "M5 ok ($FRAMES frames)"
elif [ $REACHED -ge 4 ]; then
    fail "M5: only $FRAMES frames (need $MIN_FRAMES)"
    grep -iE "segfault|abort|fatal|signal|SIGSEGV" "$RUN_LOG" | head -10
fi

# --- M6: the engine is running its own content, not just holding a surface.
#
# Inherited straight from Ice Rage, where the first version of this check
# accepted "survived + non-black framebuffer" and passed a port that was not
# working: the engine booted, cleared the screen to a colour, and opened zero
# assets. A solid-colour clear is indistinguishable from a rendered game by the
# pixel test alone.
#
# One threshold changes for this game. Ice Rage counted linked GLSL programs as
# the proof that materials were real; Dead Space has no shaders at all, so the
# fixed-function equivalent is texture uploads - glTexImage2D is where a
# material stops being a filename and becomes something the GPU holds.
#
# The loader must emit, at the end of the run:
#   TRACE: summary assets=<n> textures=<n> draws=<n>
MIN_ASSETS="${MIN_ASSETS:-50}"
MIN_TEXTURES="${MIN_TEXTURES:-10}"
MIN_DRAWS="${MIN_DRAWS:-100}"

if [ $REACHED -ge 5 ]; then
    NONBLACK=$(grep -c "TRACE: framebuffer non-black" "$RUN_LOG" || true)
    SUMMARY=$(grep -oE "TRACE: summary assets=[0-9]+ textures=[0-9]+ draws=[0-9]+" "$RUN_LOG" | tail -1)

    if [ -z "$SUMMARY" ]; then
        fail "M6: loader emitted no summary line (need: TRACE: summary assets=N textures=N draws=N)"
    else
        A=$(echo "$SUMMARY" | grep -oE "assets=[0-9]+"   | cut -d= -f2)
        T=$(echo "$SUMMARY" | grep -oE "textures=[0-9]+" | cut -d= -f2)
        D=$(echo "$SUMMARY" | grep -oE "draws=[0-9]+"    | cut -d= -f2)
        log "M6 counters: assets=$A textures=$T draws=$D nonblack=$NONBLACK"

        # 124 == timeout killed it, which is success for a game loop
        ALIVE=0
        { [ $RUN_RC -eq 124 ] || [ $RUN_RC -eq 0 ]; } && ALIVE=1

        if [ $ALIVE -eq 1 ] && [ "$NONBLACK" -gt 0 ] \
           && [ "$A" -ge "$MIN_ASSETS" ] && [ "$T" -ge "$MIN_TEXTURES" ] && [ "$D" -ge "$MIN_DRAWS" ]; then
            REACHED=6
            log "M6 ok (assets=$A textures=$T draws=$D, survived)"
        else
            [ $ALIVE -eq 0 ]             && { fail "M6: exited early rc=$RUN_RC"; grep -iE "segfault|abort|fatal|signal" "$RUN_LOG" | head -10; }
            [ "$NONBLACK" -eq 0 ]        && fail "M6: renders only black frames"
            [ "$A" -lt "$MIN_ASSETS" ]   && fail "M6: only $A assets opened (need $MIN_ASSETS) - check fix_path(), the engine asks for appbundle:/ and Android/data/... paths"
            [ "$T" -lt "$MIN_TEXTURES" ] && fail "M6: only $T textures uploaded (need $MIN_TEXTURES) - not rendering its own materials"
            [ "$D" -lt "$MIN_DRAWS" ]    && fail "M6: only $D draw calls (need $MIN_DRAWS) - nothing is being drawn"
            grep -iE "missing|no class|not found|ENOENT" "$RUN_LOG" | sort -u | head -8
        fi
    fi
fi

# --- M7: it *advances*, not just draws.
#
# M6 says "the engine is rendering". It does not say what. A game parked in its
# main menu waiting for someone to press A renders its 60 fps happily, opens its
# assets, uploads its textures and passes M6 whole. For the harness that is
# indistinguishable from a game being played.
#
# That question costs three SD-card trips to answer on the console, and it is
# the exact failure Ice Rage shipped with. So the loader takes DEADSPACE_AUTOPILOT
# and injects synthetic keys, hashes a strip of the framebuffer every N frames,
# and reports each time the scene actually changed:
#   TRACE: autopilot keys=<n> scenes=<n>
MIN_SCENES="${MIN_SCENES:-2}"

if [ $REACHED -ge 6 ]; then
    AUTO=$(grep -oE "TRACE: autopilot keys=[0-9]+ scenes=[0-9]+" "$RUN_LOG" | tail -1)
    if [ -z "$AUTO" ]; then
        fail "M7: loader emitted no autopilot line (need: TRACE: autopilot keys=N scenes=N)"
    else
        K=$(echo "$AUTO" | grep -oE "keys=[0-9]+"   | cut -d= -f2)
        S=$(echo "$AUTO" | grep -oE "scenes=[0-9]+" | cut -d= -f2)
        log "M7 autopilot: injected $K keys over $FRAMES frames, scene changed $S time(s)"
        if [ "$S" -ge "$MIN_SCENES" ]; then
            REACHED=7; log "M7 ok"
        else
            fail "M7: the scene changed $S time(s) (need $MIN_SCENES) - the game draws but does not advance"
            fail "     check the input plumbing: which keycode the engine accepts, and whether it reads keys at all"
        fi
    fi
fi

echo "$REACHED" > "$RESULTS/milestone"
log "=== milestone reached: $REACHED / 7 ==="
exit $REACHED
