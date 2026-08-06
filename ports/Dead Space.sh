#!/bin/bash
# PORTMASTER: deadspace-portmaster.zip, Dead Space.sh
#
# Dead Space (Android/Xperia Play 1.1.33) — PortMaster launcher.
# Port and project by EapRules: https://github.com/EapRules
#
# The port never ships EA's files. The user's extracted game tree lives next
# to the loader and must contain:
#
#   assets/EAMCore.ini
#   assets/published/
#   lib/armeabi/libEAMGameDeadSpace.so
#
# This is a Java-driven JNI game, not NativeActivity and not the unrelated
# Mountain Sheep/OUYA game an early scaffold for this directory described.

# shellcheck disable=SC1090,SC1091,SC2154

XDG_DATA_HOME=${XDG_DATA_HOME:-$HOME/.local/share}

if [ -d "/opt/system/Tools/PortMaster/" ]; then
  controlfolder="/opt/system/Tools/PortMaster"
elif [ -d "/opt/tools/PortMaster/" ]; then
  controlfolder="/opt/tools/PortMaster"
elif [ -d "$XDG_DATA_HOME/PortMaster/" ]; then
  controlfolder="$XDG_DATA_HOME/PortMaster"
else
  controlfolder="/roms/ports/PortMaster"
fi

source "$controlfolder/control.txt"

export PORT_32BIT="Y"
[ -f "$controlfolder/tasksetter" ]          && source "$controlfolder/tasksetter"
[ -f "$controlfolder/device_info.txt" ]     && source "$controlfolder/device_info.txt"
[ -f "$controlfolder/mod_${CFW_NAME}.txt" ] && source "$controlfolder/mod_${CFW_NAME}.txt"

get_controls

GAMEDIR="/$directory/ports/deadspace"
cd "$GAMEDIR" || exit 1

: > "$GAMEDIR/log.txt"
exec > "$GAMEDIR/log.txt" 2>&1

# PortMaster's portable metadata points at deadspace/cover.png, which is the
# canonical source shipped in the release. ArkOS/dArkOS additionally keeps a
# normalized EmulationStation copy beside the other port artwork. A direct
# update does not rerun PortMaster's metadata importer, so that copy can remain
# an old APK icon indefinitely. Refresh only Dead Space's own image when its
# bytes differ; never rewrite gamelist.xml or touch another port's metadata.
ES_PORT_IMAGE="/$directory/ports/images/Dead Space.png"
if [ -f "$GAMEDIR/cover.png" ] && \
   { [ ! -f "$ES_PORT_IMAGE" ] || ! cmp -s "$GAMEDIR/cover.png" "$ES_PORT_IMAGE"; }; then
  mkdir -p "$(dirname "$ES_PORT_IMAGE")"
  if cp -f "$GAMEDIR/cover.png" "$ES_PORT_IMAGE"; then
    echo "Port artwork normalized at $ES_PORT_IMAGE (visible after frontend restart)"
  else
    echo "Warning: could not refresh $ES_PORT_IMAGE; continuing without artwork update"
  fi
fi

export LD_LIBRARY_PATH="$GAMEDIR/libs.armhf${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export SDL_GAMECONTROLLERCONFIG="$sdl_controllerconfig"
export DEADSPACE_FACE_LAYOUT="${DEADSPACE_FACE_LAYOUT:-nintendo}"
# Aspect-correct scaling for panels that are not the tested 640x480 (e.g. the
# R36H Pro Max's 1024x768, where the engine's fixed viewport otherwise lands in
# the bottom-left corner): fit (default) keeps the aspect and letterboxes,
# stretch fills the panel, integer scales by a whole number. The loader detects
# the panel from the GL drawable; on a CFW that reports the wrong size, set
# DEADSPACE_PANEL_W / DEADSPACE_PANEL_H here to force it. On a real 640x480
# panel this is identity - nothing is scaled.
export DEADSPACE_SCALE="${DEADSPACE_SCALE:-fit}"
export LOADER_TRACE=1
# Audio routing is decided by what the device actually runs, never by CFW name -
# the same principle as the display scaling above: detect the capability, adapt
# to it. If a user audio server is present (PipeWire, or a PulseAudio socket),
# the 32-bit game must route through it or it grabs a PCM nobody is listening to
# and plays silence. If none is found, fall back to ALSA dmix, which is what a
# bare-ALSA CFW provides. No device or firmware is named.
_DS_PW=""
for _pw in /usr/lib32/pipewire-0.3 /usr/lib/arm-linux-gnueabihf/pipewire-0.3; do
  [ -d "$_pw" ] && { _DS_PW="$_pw"; break; }
done
for _xrd in "${XDG_RUNTIME_DIR:-}" /run/user/0 /var/run/user/0; do
  [ -n "$_xrd" ] && [ -d "$_xrd" ] && { export XDG_RUNTIME_DIR="$_xrd"; break; }
done
_DS_PULSE=""
for _pulse in "${XDG_RUNTIME_DIR:-}/pulse/native" /run/pulse/native /var/run/pulse/native; do
  [ -n "$_pulse" ] && [ -S "$_pulse" ] && { _DS_PULSE="$_pulse"; break; }
done
if [ -n "$_DS_PW" ] || [ -n "$_DS_PULSE" ]; then
  # A running audio server: route through it, never grab the PCM exclusively.
  unset AUDIODEV ALSA_CONFIG_PATH SDL_AUDIO_DEVICE_NAME ALSA_CARD
  export SDL_AUDIODRIVER=alsa
  export ALSOFT_DRIVERS=alsa
  export SDL_AUDIO_ALSA_SET_BUFFER_SIZE=1
  for _spa in /usr/lib32/spa-0.2 /usr/lib/arm-linux-gnueabihf/spa-0.2; do
    [ -d "$_spa" ] && { export SPA_PLUGIN_DIR="$_spa"; break; }
  done
  [ -n "$_DS_PW" ] && export PIPEWIRE_MODULE_DIR="$_DS_PW"
  if [ -n "$_DS_PULSE" ]; then
    export PULSE_SERVER="unix:$_DS_PULSE"
  else
    unset PULSE_SERVER
  fi
  echo "Audio: routing through the device's audio server (PipeWire/Pulse), dmix bypassed"
else
  export AUDIODEV="${AUDIODEV:-plug:dmix}"
  export SDL_AUDIODRIVER="${SDL_AUDIODRIVER:-alsa}"
  echo "Audio: ALSA dmix (no audio server detected)"
fi

CUR_TTY=/dev/tty0
[ -w "$CUR_TTY" ] || CUR_TTY=/dev/tty1

show_screen() {
  $ESUDO chmod 666 "$CUR_TTY" 2>/dev/null
  printf "\033c" > "$CUR_TTY"
  cat > "$CUR_TTY"
  sleep "${1:-10}"
  printf "\033c" > "$CUR_TTY"
}

GAME_SO="$GAMEDIR/lib/armeabi/libEAMGameDeadSpace.so"

# A release user should not have to unpack an Android package by hand. eapx
# discovers APK/ZIP/folder donors by their contents, stages the complete game
# tree away from the live install, validates the exact native library and only
# then publishes assets/ and lib/. Existing manually-extracted installs skip
# this path and continue to work exactly as before.
if [ ! -f "$GAME_SO" ] || [ ! -f "$GAMEDIR/assets/EAMCore.ini" ] \
   || [ ! -d "$GAMEDIR/assets/published" ]; then
  if ! command -v python3 >/dev/null 2>&1; then
    echo "Game-data import failed: python3 is unavailable"
    show_screen 12 <<EOF

  Dead Space - Python 3 missing

  Automatic game-data import needs
  Python 3 from the CFW.

  Update PortMaster/your firmware, or
  extract the donor on a computer into:
    ports/deadspace/

EOF
    pm_finish
    exit 1
  fi

  if [ ! -f "$GAMEDIR/eapx.py" ] || [ ! -f "$GAMEDIR/deadspace.eapx.json" ]; then
    echo "Game-data import failed: eapx runtime or recipe is missing"
    show_screen 12 <<EOF

  Dead Space - incomplete port

  eapx.py or deadspace.eapx.json
  is missing. Reinstall the release ZIP
  through PortMaster/autoinstall.

EOF
    pm_finish
    exit 1
  fi

  echo "Game data is absent; starting content-based first-boot import"
  if ! python3 "$GAMEDIR/eapx.py" install \
       --recipe "$GAMEDIR/deadspace.eapx.json" \
       --game-dir "$GAMEDIR" --tty "$CUR_TTY"; then
    echo "Game-data import failed; see $GAMEDIR/eapx.log"
    show_screen 15 <<EOF

  Dead Space - game data not ready

  Put your own Xperia Play v1.1.33
  APK, ZIP, or extracted folder in:
    ports/deadspace/

  The filename does not matter.
  See README.md and eapx.log.

EOF
    pm_finish
    exit 1
  fi
fi

if [ ! -f "$GAME_SO" ] || [ ! -f "$GAMEDIR/assets/EAMCore.ini" ] \
   || [ ! -d "$GAMEDIR/assets/published" ]; then
  echo "Game-data check failed: extracted Xperia Play files are incomplete."
  show_screen 12 <<EOF

  Dead Space - missing game data

  Put your APK, ZIP or extracted game in:
    ports/deadspace/

  Required:
    assets/EAMCore.ini
    assets/published/
    lib/armeabi/
      libEAMGameDeadSpace.so

  Required build: Xperia Play v1.1.33

EOF
  pm_finish
  exit 1
fi

rm -f "$GAMEDIR/PUT_DEAD_SPACE_DATA_HERE.txt"

# eapx v3 records the coherent donor fingerprint it imported.  The runtime
# still dispatches every texture by its GL enum; this value is diagnostic and
# never selects a renderer-wide mode.
DONOR_MARKER="$GAMEDIR/.eapx-deadspace-data.json"
DEADSPACE_DONOR_PROFILE="unknown-existing-install"
if [ -f "$DONOR_MARKER" ]; then
  MARKER_PROFILE=$(sed -n 's/.*"donor_profile"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p' "$DONOR_MARKER" | head -n 1)
  case "$MARKER_PROFILE" in
    full-pvrtc-original|full-pvrtc-repacked|vita-rip-atc)
      DEADSPACE_DONOR_PROFILE="$MARKER_PROFILE"
      ;;
  esac
fi
export DEADSPACE_DONOR_PROFILE
echo "Donor profile: $DEADSPACE_DONOR_PROFILE"

# The loader patches fixed offsets, so accepting a different binary would turn
# a useful "wrong build" message into a crash deep inside startup.
EXPECTED_SHA1="0ed42b611415015807f759ec9b5457857143ce39"
if command -v sha1sum >/dev/null 2>&1; then
  GAME_SHA1=$(sha1sum "$GAME_SO" | cut -d' ' -f1)
  if [ "$GAME_SHA1" != "$EXPECTED_SHA1" ]; then
    echo "Game-data check failed: sha1=$GAME_SHA1 expected=$EXPECTED_SHA1"
    show_screen 12 <<EOF

  Dead Space - unsupported game build

  This port needs the Xperia Play
  v1.1.33 native library.

  Found SHA1:
    $GAME_SHA1

EOF
    pm_finish
    exit 1
  fi
fi

# SDL must create its context through the device's own 32-bit GL stack. Build
# symlinks in /tmp because the SD card may be exFAT and cannot preserve them.
#
# Which stack that is depends on the device, not on the firmware's name, so it
# is found by capability:
#
#   1. A unified Mali blob - one .so exporting EGL, GLESv1_CM and GLESv2. Try
#      the exact tested filenames first (fast, known-good), then any Mali build
#      in the 32-bit library directories, because every distribution names it
#      differently: versioned upstream names on Debian-style CFWs
#      (libmali-bifrost-g31-*.so), an unversioned libmali.so.1 on Buildroot ones,
#      libMali.so where a firmware symlinks it. The blob is then linked to every
#      name the game asks for.
#   2. No blob, but a real 32-bit EGL/GLES set - a Mesa/glvnd userland, which is
#      what a Panfrost-only device ships. Each entry point is linked under its
#      own name; only GL names are exposed, so nothing else in a system library
#      directory can shadow the port's bundled libs.
#   3. Neither. Say so on screen instead of leaving the user with a black panel:
#      without a 32-bit provider SDL either falls back to something that never
#      reaches the framebuffer, or fails to create a window at all.
#
# Directories are architecture-scoped, so a 64-bit library can never be picked:
# the multiarch triplet dir and lib32 are 32-bit by definition, and the bare
# /usr/lib and /lib are only consulted on a pure-armhf rootfs.
GL_DIRS="/usr/lib/arm-linux-gnueabihf /usr/lib/arm-linux-gnueabihf/mali \
/lib/arm-linux-gnueabihf /usr/lib32/mali /usr/lib32"
if [ "$DEVICE_ARCH" = "armhf" ]; then
  GL_DIRS="$GL_DIRS /usr/lib /lib"
fi

MALI_BLOB=""
for candidate in \
  /usr/lib/arm-linux-gnueabihf/libmali-bifrost-g31-rxp0-gbm.so \
  /usr/lib/arm-linux-gnueabihf/libMali.so \
  /usr/lib/arm-linux-gnueabihf/libmali.so.1; do
  [ -e "$candidate" ] && { MALI_BLOB="$candidate"; break; }
done
if [ -z "$MALI_BLOB" ]; then
  for _gldir in $GL_DIRS; do
    [ -d "$_gldir" ] || continue
    for _cand in "$_gldir"/libmali-*.so "$_gldir"/libmali.so.* \
                 "$_gldir"/libmali.so "$_gldir"/libMali.so*; do
      [ -e "$_cand" ] && { MALI_BLOB="$_cand"; break; }
    done
    [ -n "$MALI_BLOB" ] && break
  done
fi

GL_SHIM="/tmp/deadspace-gl"
rm -rf "$GL_SHIM"
GL_READY=""
if [ -n "$MALI_BLOB" ]; then
  if mkdir -p "$GL_SHIM" \
     && ln -sf "$MALI_BLOB" "$GL_SHIM/libEGL.so.1" \
     && ln -sf "$MALI_BLOB" "$GL_SHIM/libGLESv1_CM.so.1" \
     && ln -sf "$MALI_BLOB" "$GL_SHIM/libGLESv2.so.2" \
     && ln -sf "$MALI_BLOB" "$GL_SHIM/libmali.so.1"; then
    GL_READY="y"
    echo "GL: using Mali blob $MALI_BLOB"
  else
    echo "GL: failed to create /tmp shim, using system libraries"
  fi
else
  # No unified blob: link whatever 32-bit EGL/GLES entry points exist, each
  # under its own name. libEGL is the one SDL cannot start without.
  GL_EGL=""
  mkdir -p "$GL_SHIM" 2>/dev/null
  for _gldir in $GL_DIRS; do
    # libEGL is what SDL cannot start without, so one directory must provide
    # it and the GLES libraries are taken from that same directory - a set
    # assembled from two userlands would not be one working stack.
    [ -e "$_gldir/libEGL.so.1" ] || continue
    for _soname in libEGL.so.1 libGLESv1_CM.so.1 libGLESv2.so.2; do
      [ -e "$_gldir/$_soname" ] && ln -sf "$_gldir/$_soname" "$GL_SHIM/$_soname"
    done
    [ -e "$GL_SHIM/libEGL.so.1" ] && { GL_EGL="$_gldir/libEGL.so.1"; break; }
  done
  if [ -n "$GL_EGL" ]; then
    GL_READY="y"
    echo "GL: no Mali blob; using the device's 32-bit EGL/GLES set ($GL_EGL)"
  fi
fi

if [ -n "$GL_READY" ]; then
  export LD_LIBRARY_PATH="$GL_SHIM:$LD_LIBRARY_PATH"
else
  rm -rf "$GL_SHIM"
  echo "GL: no 32-bit GL provider found; searched: $GL_DIRS"
  show_screen 12 <<EOF

  Dead Space - no 32-bit GPU driver

  This firmware ships no 32-bit Mali
  blob and no 32-bit EGL/GLES set, so
  the game cannot open a window.

  The screen will stay black or the
  game will exit. See log.txt.

EOF
fi

mkdir -p "$GAMEDIR/var"
$ESUDO chmod +x "$GAMEDIR/deadspace"

# Controls are delivered directly through the game's JNI key/pointer exports.
# gptokeyb remains only for PortMaster's standard exit combination.
$GPTOKEYB "deadspace" -c "$GAMEDIR/deadspace.gptk" &

if command -v pm_platform_helper >/dev/null 2>&1; then
  pm_platform_helper "$GAMEDIR/deadspace"
fi

$TASKSET "$GAMEDIR/deadspace" "$GAMEDIR"
GAME_RC=$?

$ESUDO kill -9 "$(pidof gptokeyb)" 2>/dev/null
rm -rf /tmp/deadspace-gl
unset LD_LIBRARY_PATH SDL_GAMECONTROLLERCONFIG

pm_finish
exit "$GAME_RC"
