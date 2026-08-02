#!/usr/bin/env bash
#
# Interactive, host-controlled Dead Space emulator.
#
# The game still runs through qemu-arm and Mesa/llvmpipe, as in the immutable
# verifier, but it has no fixed frame limit and mounts a bidirectional control
# directory. emulator_control.cpp consumes commands from that directory and
# writes PNG screenshots/status back to it.
set -euo pipefail

PORT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
GAME_DIR="${DEADSPACE_GAMEDIR:-$PORT_DIR/../NeededFiles/data/deadspace}"
CONTROL_DIR="${DEADSPACE_CONTROL_DIR:-$PORT_DIR/emulator/runtime}"
IMAGE="${DEADSPACE_BUILD_IMAGE:-deadspace-build}"

while [ "$#" -gt 0 ]; do
    case "$1" in
        --game-dir)
            GAME_DIR="${2:?--game-dir needs a path}"
            shift 2
            ;;
        --control-dir)
            CONTROL_DIR="${2:?--control-dir needs a path}"
            shift 2
            ;;
        *)
            echo "unknown argument: $1" >&2
            exit 2
            ;;
    esac
done

if [ ! -f "$GAME_DIR/lib/armeabi/libEAMGameDeadSpace.so" ]; then
    echo "game tree not found at $GAME_DIR" >&2
    exit 2
fi

mkdir -p "$CONTROL_DIR/screenshots"
: > "$CONTROL_DIR/commands"
rm -f "$CONTROL_DIR/status.json" "$CONTROL_DIR/status.json.tmp"

if [ "${DEADSPACE_EMULATOR_SKIP_BUILD:-0}" != "1" ]; then
    docker run --rm \
        -v "$PORT_DIR":/src \
        -w /src \
        "$IMAGE" make -j4
fi

exec docker run --rm \
    -v "$PORT_DIR":/src \
    -v "$GAME_DIR":/game:ro \
    -v "$CONTROL_DIR":/control \
    -w /src \
    -e SDL_VIDEODRIVER=offscreen \
    -e SDL_AUDIODRIVER=dummy \
    -e LIBGL_ALWAYS_SOFTWARE=1 \
    -e GALLIUM_DRIVER=llvmpipe \
    -e EGL_PLATFORM=surfaceless \
    -e LOADER_TRACE=1 \
    -e DEADSPACE_AUTOPILOT="${DEADSPACE_AUTOPILOT:-}" \
    -e DEADSPACE_AUTOPILOT_FAST="${DEADSPACE_AUTOPILOT_FAST:-}" \
    -e DEADSPACE_AUTOPILOT_VISUAL="${DEADSPACE_AUTOPILOT_VISUAL:-}" \
    -e DEADSPACE_FRAME_LIMIT="${DEADSPACE_FRAME_LIMIT:-}" \
    -e DEADSPACE_FAST_FORWARD="${DEADSPACE_FAST_FORWARD:-}" \
    -e DEADSPACE_NO_VFP_PATCH="${DEADSPACE_NO_VFP_PATCH:-}" \
    -e DEADSPACE_VFP_SELFTEST="${DEADSPACE_VFP_SELFTEST:-}" \
    -e DEADSPACE_CONTROL_DIR=/control \
    -e DEADSPACE_GL_DIAG="${DEADSPACE_GL_DIAG:-}" \
    -e DEADSPACE_DONOR_PROFILE="${DEADSPACE_DONOR_PROFILE:-}" \
    -e DEADSPACE_MALI_COMPAT="${DEADSPACE_MALI_COMPAT:-}" \
    -e DEADSPACE_PVRTC_SOFTWARE="${DEADSPACE_PVRTC_SOFTWARE:-}" \
    -e DEADSPACE_TEST_TIME_SCALE="${DEADSPACE_TEST_TIME_SCALE:-}" \
    -e DEADSPACE_AUTO_SCREENSHOT_FRAME="${DEADSPACE_AUTO_SCREENSHOT_FRAME:-}" \
    -e DEADSPACE_AUTO_SCREENSHOT_FRAMES="${DEADSPACE_AUTO_SCREENSHOT_FRAMES:-}" \
    "$IMAGE" \
    qemu-arm -L /usr/arm-linux-gnueabihf \
        ./build/deadspace /game
