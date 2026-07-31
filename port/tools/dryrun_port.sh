#!/usr/bin/env bash
#
# Dry run of the PortMaster launcher, inside the build container.
#
# M7's done_when is "validated by shellcheck and a dry run". This is the dry
# run: it builds a throwaway PortMaster installation around ports/Dead Space.sh
# and executes the real script — no copy, no edited variant — so that the
# things only the launcher can get wrong are actually exercised:
#
#   * the controlfolder probe finds control.txt
#   * "/$directory/ports/deadspace" resolves to where the port was installed
#   * cd, the log.txt redirect and mkdir of DEADSPACE_DATA_DIR all succeed
#   * $GPTOKEYB is backgrounded and $TASKSET starts the game with the APK
#   * pm_finish runs and gptokeyb is gone afterwards
#   * libs.armhf/ really does carry every library the console will not have
#
# That last one is the only claim a dry run inside the build image could fake,
# because the image has all those libraries installed system-wide: the game
# would start with an empty libs.armhf and prove nothing. So before the run,
# every library the port bundles is moved out of the image's own search paths
# (the container is a throwaway, so this destroys nothing). What is left is a
# machine that has glibc and SDL2 and nothing else of ours — which is the
# console. If libs.armhf is missing one entry, the game does not start.
#
# The two substitutions the container forces, both of the same shape the real
# thing has: $TASKSET becomes qemu-arm (a command prefix, like taskset -c) and
# $GPTOKEYB becomes a script that sleeps (a backgrounded daemon, like the real
# one). Everything else is the port as it will ship.
#
# Usage (from the repo root, on the host):
#   docker run --rm -v "$PWD":/src -v "/path/to/donor":/game:ro -w /src \
#       deadspace-build tools/dryrun_port.sh /game
set -uo pipefail

DONOR="${1:?usage: dryrun_port.sh <path to Dead Space APK, ZIP or extracted tree>}"
ROOT=/tmp/dryrun
PM="$ROOT/home/.local/share/PortMaster"
GAMEDIR="/roms/ports/deadspace"

rm -rf "$ROOT" /roms
mkdir -p "$PM" "$GAMEDIR/libs.armhf"

# --- the fake PortMaster install ------------------------------------------
cat > "$PM/control.txt" <<'EOF'
directory="roms"
CFW_NAME="dryrun"
ESUDO=""
sdl_controllerconfig=""
GPTOKEYB="/tmp/dryrun/gptokeyb"
get_controls() { echo "[dryrun] get_controls called"; }
pm_finish() {
    echo "[dryrun] pm_finish called"
    # The real pm_finish kills gptokeyb by name with pkill, which this image
    # does not have; the stand-in kills the pid the fake gptokeyb recorded.
    [ -f /tmp/dryrun/gptokeyb.pid ] && kill "$(cat /tmp/dryrun/gptokeyb.pid)" 2>/dev/null
    return 0
}
EOF

# tasksetter is the file that defines $TASKSET on a real install.
cat > "$PM/tasksetter" <<'EOF'
TASKSET="qemu-arm -L /usr/arm-linux-gnueabihf"
EOF

cat > "$PM/device_info.txt" <<'EOF'
DEVICE_ARCH="aarch64"
EOF

cat > "$ROOT/gptokeyb" <<'EOF'
#!/bin/sh
echo "[dryrun] gptokeyb started: $*"
echo $$ > /tmp/dryrun/gptokeyb.pid
exec sleep 600
EOF
chmod +x "$ROOT/gptokeyb"

# --- install the port the way the zip would --------------------------------
cp "build/deadspace"                  "$GAMEDIR/deadspace"
cp "ports/deadspace/deadspace.gptk"   "$GAMEDIR/deadspace.gptk"
cp "ports/deadspace/deadspace.eapx.json" "$GAMEDIR/deadspace.eapx.json"
cp "tools/eapx.py"                    "$GAMEDIR/eapx.py"
cp "ports/Dead Space.sh"              "/roms/ports/Dead Space.sh"
mkdir -p "$GAMEDIR/gamedata"
cp -R "$DONOR" "$GAMEDIR/gamedata/donor"

if [ ! -f "build/libs.armhf/MANIFEST.txt" ]; then
    echo "[dryrun] build/libs.armhf is not there — run 'make libs' first." >&2
    exit 1
fi
cp build/libs.armhf/*.so* "$GAMEDIR/libs.armhf/"

# --- make the container look like the console -------------------------------
# What gets taken away is decided by the console's side of the contract, NOT by
# what the port happened to copy: keep glibc and SDL2 (and whatever SDL2 pulls
# in, since the device's SDL2 brings its own chain), take away everything else
# armhf in the image. Deriving the list from libs.armhf itself was the first
# attempt and it was worthless — a library missing from libs.armhf was also
# missing from the quarantine list, so the system copy answered and the dry run
# passed with an incomplete port.
ARMHF_DIRS="/usr/lib/arm-linux-gnueabihf /lib/arm-linux-gnueabihf /usr/arm-linux-gnueabihf/lib"
READELF=arm-linux-gnueabihf-readelf

resolve_so() {
    local d p
    for d in $ARMHF_DIRS; do [ -e "$d/$1" ] && { echo "$d/$1"; return 0; }; done
    # Plugins (pulseaudio, mesa's dri) sit a level down and are reached by
    # RPATH, so a flat search of the three dirs does not find them.
    # ARMHF_DIRS is a list and is meant to word-split here.
    # shellcheck disable=SC2086
    p=$(find $ARMHF_DIRS -name "$1" 2>/dev/null | head -1)
    [ -n "$p" ] && { echo "$p"; return 0; }
    return 1
}
needed_of() { "$READELF" -d "$1" 2>/dev/null | sed -n 's/.*Shared library: \[\(.*\)\]/\1/p'; }

# The three families the console provides and the port must not carry. This
# is deliberately a second, independent statement of the same policy
# tools/collect_libs.sh applies: producer and referee agreeing is the point.
is_device_provided() {
    case "$1" in
        ld-linux-armhf.so.3|libc.so.6|libm.so.6|libdl.so.2|libpthread.so.0|\
        librt.so.1|libresolv.so.2|libanl.so.1|libutil.so.1) return 0 ;;
        libSDL2-2.0.so.0) return 0 ;;
        libEGL.so.*|libGLESv2.so.*|libGLESv1_CM.so.*|libGL.so.*|libgbm.so.*|libdrm.so.*) return 0 ;;
    esac
    return 1
}

# Transitive closure of sonames, plus the real filename behind each SONAME
# symlink — moving that would break the symlink we meant to keep.
so_closure() {
    local seen=" " queue="$*" next s p
    while [ -n "${queue// /}" ]; do
        next=""
        for s in $queue; do
            case "$seen" in *" $s "*) continue ;; esac
            seen="$seen$s "
            p=$(resolve_so "$s") || continue
            seen="$seen$(basename "$(readlink -f "$p")") "
            next="$next $(needed_of "$p")"
        done
        queue=$next
    done
    echo "$seen"
}

# --- assertion 1: libs.armhf covers everything the console will not have ----
# Walk the port binary's own dependencies, descending only through libraries
# the console does not provide, and demand each one is in libs.armhf. This is
# derived from the binary, so a library left out of libs.armhf is named here
# even if some other part of the image happens to still have a copy.
MISSING=""
covq=$(needed_of "$GAMEDIR/deadspace"); covseen=" "
while [ -n "${covq// /}" ]; do
    covnext=""
    for s in $covq; do
        case "$covseen" in *" $s "*) continue ;; esac
        covseen="$covseen$s "
        is_device_provided "$s" && continue
        [ -e "$GAMEDIR/libs.armhf/$s" ] || { MISSING="$MISSING $s"; continue; }
        covnext="$covnext $(needed_of "$GAMEDIR/libs.armhf/$s")"
    done
    covq=$covnext
done
[ -n "$MISSING" ] && echo "[dryrun] libs.armhf is missing:$MISSING"

# --- assertion 2: and prove it by taking the system's copies away -----------
# What gets removed is decided by the console's side of the contract, not by
# what the port happened to copy: keep glibc, SDL2 and the GL stack, plus
# whatever those pull in (on the device they bring their own chain), and take
# away every other armhf library in the image. Deriving the list from
# libs.armhf itself was the first attempt and it was worthless — a library
# missing from libs.armhf was also missing from the removal list, so the
# system copy answered and the dry run passed with an incomplete port.
# The GL stack is not reachable by DT_NEEDED: glvnd dlopens its vendor library
# (libEGL_mesa) and mesa dlopens its driver out of dri/, so both have to be
# named. In emulation that stack is Mesa; on the console it is the Mali blob.
# Either way it belongs to the device, and so does everything it drags in —
# which is the one soft spot of this removal: a library Mesa happens to need
# too (libz, libzstd) survives here and is proven only by assertion 1 above.
PLUGIN_NEEDS=""
# shellcheck disable=SC2086
while IFS= read -r p; do
    PLUGIN_NEEDS="$PLUGIN_NEEDS $(needed_of "$p")"
done < <(find $ARMHF_DIRS -mindepth 2 -name '*.so*' 2>/dev/null)
GL_ROOTS=$(for d in $ARMHF_DIRS; do ls "$d" 2>/dev/null; done \
           | grep -E '^(libEGL|libGL|libGLX|libGLES|libglapi|libgallium|libgbm|libdrm|libvulkan|libxshmfence|libxcb-(dri|present|sync))' \
           | sort -u)
# shellcheck disable=SC2086
KEEP=$(so_closure ld-linux-armhf.so.3 libc.so.6 libm.so.6 libdl.so.2 libpthread.so.0 \
                  librt.so.1 libresolv.so.2 libSDL2-2.0.so.0 \
                  $GL_ROOTS $PLUGIN_NEEDS)

QUAR="$ROOT/quarantine"
mkdir -p "$QUAR"
MOVED=0
for d in $ARMHF_DIRS; do
    [ -d "$d" ] || continue
    for so in "$d"/*.so*; do
        [ -e "$so" ] || continue
        name=$(basename "$so")
        case "$KEEP" in *" $name "*) continue ;; esac
        mv -f "$so" "$QUAR/$name" 2>/dev/null && MOVED=$((MOVED + 1))
    done
done
echo "[dryrun] moved $MOVED armhf libraries out of the system; what is left is glibc, SDL2 and the GL stack — which is what the console has"
# The donor is the user's own copy. It deliberately has a meaningless name:
# eapx must identify it by content, not because a test handed it a hint.
chmod -x "$GAMEDIR/deadspace"   # prove the launcher's chmod +x is doing work

# --- run it ----------------------------------------------------------------
export HOME="$ROOT/home"
export XDG_DATA_HOME="$ROOT/home/.local/share"
export SDL_VIDEODRIVER=offscreen
export LIBGL_ALWAYS_SOFTWARE=1
export GALLIUM_DRIVER=llvmpipe
export EGL_PLATFORM=surfaceless
export LOADER_TRACE=1
export DEADSPACE_FRAME_LIMIT="${DEADSPACE_FRAME_LIMIT:-40}"

echo "[dryrun] executing the launcher"
bash "/roms/ports/Dead Space.sh"
RC=$?
echo "[dryrun] launcher exited rc=$RC"

# --- assert on what the launcher, and only the launcher, is responsible for -
FAIL=0

# True only if the fake gptokeyb is still a live process. A zombie does not
# count: once the launcher shell exits its background job is unreaped for a
# moment, and "kill -0 succeeds" would report that as still running.
# (Called indirectly, through check_not.)
# shellcheck disable=SC2329
gptokeyb_running() {
    local pid
    pid=$(cat "$ROOT/gptokeyb.pid" 2>/dev/null) || return 1
    [ -n "$pid" ] && [ -r "/proc/$pid/stat" ] || return 1
    ! grep -qE '^[0-9]+ \(.*\) Z ' "/proc/$pid/stat"
}

check()     { local d="$1"; shift; if "$@";     then echo "[dryrun] ok:   $d"; else echo "[dryrun] FAIL: $d"; FAIL=1; fi; }
check_not() { local d="$1"; shift; if ! "$@";   then echo "[dryrun] ok:   $d"; else echo "[dryrun] FAIL: $d"; FAIL=1; fi; }

check "libs.armhf covers every non-system library" test -z "$MISSING"
# Guard the guard: if nothing was moved, the run above proved nothing about
# libs.armhf, because the system copies were still there to be found.
check "system copies of the bundled libs removed"  test "$MOVED" -gt 0
check_not "system libzip absent; no accidental fallback" test -e /usr/lib/arm-linux-gnueabihf/libzip.so.5

LOG="$GAMEDIR/log.txt"
check "log.txt written by the launcher"          test -s "$LOG"
check "writable var directory created"            test -d "$GAMEDIR/var"
check "eapx marker written"                       test -f "$GAMEDIR/.eapx-deadspace-data.json"
check "eapx import log written"                   test -s "$GAMEDIR/eapx.log"
check "donor native library imported"             test -f "$GAMEDIR/lib/armeabi/libEAMGameDeadSpace.so"
check "donor content tree imported"               test -d "$GAMEDIR/assets/published"
check "binary made executable"                   test -x "$GAMEDIR/deadspace"
check "gptokeyb launched with the gptk"          grep -q "gptokeyb started" "$LOG"
check "game library loaded through the launcher" grep -q "TRACE: module loaded" "$LOG"
check "game ran to its own summary"              grep -q "TRACE: summary" "$LOG"
check "pm_finish reached"                        grep -q "pm_finish called" "$LOG"
check_not "gptokeyb not left running"            gptokeyb_running
check "launcher exit status 0"                   test "$RC" -eq 0

echo "[dryrun] === $( [ $FAIL -eq 0 ] && echo PASS || echo FAIL ) ==="
exit $FAIL
