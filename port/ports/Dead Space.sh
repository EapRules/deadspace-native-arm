#!/bin/bash
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
export LOADER_TRACE=1
export AUDIODEV="${AUDIODEV:-plug:dmix}"
export SDL_AUDIODRIVER="${SDL_AUDIODRIVER:-alsa}"

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

# SDL must create its context through the 32-bit Mali blob. Build symlinks in
# /tmp because the SD card may be exFAT and cannot preserve symlinks.
MALI_BLOB=""
for candidate in \
  /usr/lib/arm-linux-gnueabihf/libmali-bifrost-g31-rxp0-gbm.so \
  /usr/lib/arm-linux-gnueabihf/libMali.so \
  /usr/lib/arm-linux-gnueabihf/libmali.so.1; do
  [ -e "$candidate" ] && { MALI_BLOB="$candidate"; break; }
done

if [ -n "$MALI_BLOB" ]; then
  GL_SHIM="/tmp/deadspace-gl"
  rm -rf "$GL_SHIM"
  if mkdir -p "$GL_SHIM" \
     && ln -sf "$MALI_BLOB" "$GL_SHIM/libEGL.so.1" \
     && ln -sf "$MALI_BLOB" "$GL_SHIM/libGLESv1_CM.so.1" \
     && ln -sf "$MALI_BLOB" "$GL_SHIM/libGLESv2.so.2" \
     && ln -sf "$MALI_BLOB" "$GL_SHIM/libmali.so.1"; then
    export LD_LIBRARY_PATH="$GL_SHIM:$LD_LIBRARY_PATH"
    echo "GL: using Mali blob $MALI_BLOB"
  else
    echo "GL: failed to create /tmp shim, using system libraries"
  fi
else
  echo "GL: no compatible 32-bit Mali blob found"
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
