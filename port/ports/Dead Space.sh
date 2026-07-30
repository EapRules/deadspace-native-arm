#!/bin/bash
#
# Dead Space — PortMaster launcher.
#
# Modelled on ports/half-life/Half-Life.sh from PortMaster-New: same
# controlfolder probe, same control.txt / tasksetter / device_info.txt
# sourcing, same $GPTOKEYB + $TASKSET invocation, same "/$directory/ports"
# base. The differences are all forced by what this port is:
#
#   * armhf only. The game ships one native library, armeabi-v7a, and the
#     bionic ELF loader has to map it into a process of the same word size,
#     so there is no $DEVICE_ARCH switch here — the binary is always armhf.
#   * The game data is the user's own APK. It is never bundled with the port
#     and it is passed to the loader as argv[1].
#   * DEADSPACE_DATA_DIR is where the engine writes its savegames. On Android
#     that is /data/data/<pkg>/files and it always exists before onCreate;
#     here it has to be created, because the engine treats a failed open of
#     its save directory as fatal rather than as "no save yet".

# control.txt and the files it pulls in live on the device, not in this repo,
# so shellcheck can neither follow them (SC1090/SC1091) nor see that they are
# what define $directory, $sdl_controllerconfig, $ESUDO, $GPTOKEYB and
# $TASKSET (SC2154). Every other class of finding stays on.
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

# The game ships one native library, armeabi-v7a, so this port is 32-bit and
# there is no aarch64 build to fall back to. The CFW mod files below read this
# flag to set up a 32-bit environment, so it has to be exported before them.
export PORT_32BIT="Y"

# Half-Life.sh sources these three unconditionally. They only exist on newer
# PortMaster installs, and an old one is a plausible thing to meet in the
# wild, so ask first: a missing tasksetter must not take the port down with
# "No such file or directory" before it has printed anything.
[ -f "$controlfolder/tasksetter" ]          && source "$controlfolder/tasksetter"
[ -f "$controlfolder/device_info.txt" ]     && source "$controlfolder/device_info.txt"
[ -f "$controlfolder/mod_${CFW_NAME}.txt" ] && source "$controlfolder/mod_${CFW_NAME}.txt"

get_controls

GAMEDIR="/$directory/ports/deadspace"
cd "$GAMEDIR" || exit 1

# Everything from here on lands in log.txt, which is the only diagnostic
# anyone gets off the console.
#
# Written straight to the file rather than through `tee`. A pipe adds a second
# process with its own buffer, and on a crash PortMaster's exit path pkills
# everything before that buffer is flushed - so the log ends mid-sentence and
# loses exactly the part naming the fault. That happened here: a reproducible
# segfault produced a log with no FATAL block in it at all.
#
# The handheld has no visible terminal, so nothing is lost by not echoing.
: > "$GAMEDIR/log.txt"
exec > "$GAMEDIR/log.txt" 2>&1

export LD_LIBRARY_PATH="$GAMEDIR/libs.armhf:$LD_LIBRARY_PATH"
export SDL_GAMECONTROLLERCONFIG="$sdl_controllerconfig"

# Fire goes on the button the player reads as "A". Which SDL button that is
# depends on the silkscreen, not on SDL: these handhelds are lettered Nintendo
# style, with A on the right, and that is the default. A device lettered Xbox
# style, with A at the bottom, wants "xbox" here.
export DEADSPACE_FACE_LAYOUT="${DEADSPACE_FACE_LAYOUT:-nintendo}"

# ALSA hands out one exclusive handle and EmulationStation's menu music is
# likely holding it, which is why the first device build had no sound at all.
# dmix mixes in software so several processes can share the card. If the CFW
# has no dmix configured this name simply fails and the loader falls back to
# enumerating whatever devices SDL reports.
export AUDIODEV="${AUDIODEV:-plug:dmix}"
export SDL_AUDIODRIVER="${SDL_AUDIODRIVER:-alsa}"

CUR_TTY=/dev/tty0
[ -w "$CUR_TTY" ] || CUR_TTY=/dev/tty1

# The user's own APK. Without it there is nothing to run, and a handheld shows
# no terminal, so this has to reach the screen: a silent exit back to the menu
# is indistinguishable from a crash.
if [ ! -f "$GAMEDIR/deadspace.apk" ]; then
  $ESUDO chmod 666 "$CUR_TTY" 2>/dev/null
  printf "\033c" > "$CUR_TTY"
  {
    echo ""
    echo "  Dead Space - missing game file"
    echo ""
    echo "  Put your own copy of the game here:"
    echo "    $GAMEDIR/deadspace.apk"
    echo ""
    echo "  It is the Android APK, package"
    echo "  net.mountainsheep.deadspace"
    echo ""
    echo "  See README.md in the port folder."
    echo ""
  } > "$CUR_TTY"
  sleep 10
  printf "\033c" > "$CUR_TTY"
  pm_finish
  exit 1
fi

# Say something useful about the wrong APK, because the alternative is awful.
#
# The loader binds to symbols in this exact build of libdeadspace.so. Hand it a
# repacked "MOD" APK, an XAPK renamed to .apk, or a split install, and it dies
# somewhere deep with a stack trace nobody can act on - the port looks broken
# when the file was.
#
# So two questions get asked here, cheaply, before anything else runs:
# does the native library exist at all, and does the manifest still carry the
# version this port was built against? The first is fatal; the second is only a
# warning, because a future repack might still work and refusing to start would
# be worse than letting someone try.
#
# unzip is not guaranteed on every CFW. If it is missing, both checks are
# skipped in silence rather than blocking a launch that would have worked.
show_screen() {
  $ESUDO chmod 666 "$CUR_TTY" 2>/dev/null
  printf "\033c" > "$CUR_TTY"
  cat > "$CUR_TTY"
  sleep "${1:-10}"
  printf "\033c" > "$CUR_TTY"
}

if command -v unzip >/dev/null 2>&1; then
  if ! unzip -l "$GAMEDIR/deadspace.apk" 2>/dev/null | grep -q "lib/armeabi-v7a/libdeadspace.so"; then
    echo "APK check: lib/armeabi-v7a/libdeadspace.so is not in the APK"
    show_screen 12 <<EOF

  Dead Space - wrong game file

  That APK has no armeabi-v7a library
  inside, so there is nothing to run.

  Common causes:
    - it is an XAPK or APKM renamed
      to .apk (those are archives that
      hold the real APK inside)
    - it is a split install, arm64 only

  You need the plain single APK of
  net.mountainsheep.deadspace

EOF
    pm_finish
    exit 1
  fi

  # AndroidManifest.xml is binary XML holding its strings as UTF-16, so the
  # version reads as "1.8" only once the padding bytes are gone. Reducing it
  # to printable characters does that and, just as importantly, stops grep from
  # treating the stream as binary - a binary match with -q reports nothing found
  # and the check would reject every APK, including the right one.
  if ! unzip -p "$GAMEDIR/deadspace.apk" AndroidManifest.xml 2>/dev/null \
       | LC_ALL=C tr -cd '[:print:]\n' | LC_ALL=C grep -q "net\.mountainsheep\.deadspace"; then
    echo "APK check: this does not look like net.mountainsheep.deadspace - continuing anyway"
    show_screen 8 <<EOF

  Dead Space - unexpected APK

  This port was built against the OUYA
  release of net.mountainsheep.deadspace
  (v1.8, October 2013), and your file
  does not look like it.

  Other builds may still work, but the
  Play version is touch-only and this
  port drives the game as a controller.

  Starting anyway - if it crashes or
  the menus do not respond, this is the
  first thing to suspect.

EOF
  else
    echo "APK check: net.mountainsheep.deadspace, native library present"
  fi
fi

# On this hardware libEGL.so.1 resolves through glvnd to Mesa, while
# libGLESv2.so.2 is the Mali blob. Mixing the two yields a context that looks
# valid and then reports null for every GL string, so the game renders nothing.
# Point both names at the blob and put that directory first.
#
# It has to be built at runtime under /tmp: the SD card is exFAT, which has no
# symlinks, so a shim shipped inside the port would arrive as broken files.
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
  if mkdir -p "$GL_SHIM" && ln -sf "$MALI_BLOB" "$GL_SHIM/libEGL.so.1" \
                          && ln -sf "$MALI_BLOB" "$GL_SHIM/libGLESv2.so.2"; then
    export LD_LIBRARY_PATH="$GL_SHIM:$LD_LIBRARY_PATH"
    echo "GL: forcing the Mali blob from $MALI_BLOB"
  else
    echo "GL: could not build the shim in $GL_SHIM, using the system default"
  fi
else
  echo "GL: no Mali blob found, using the system default"
fi

# The engine's writable storage. Kept inside the port directory so the port
# stays self-contained and uninstalls cleanly.
export DEADSPACE_DATA_DIR="$GAMEDIR/gamedata"

# Drop a file in here to replace the APK asset of the same name. The game's own
# splash is 1920x1080 and this panel is 640x480, so splash.jpg is the one worth
# replacing: a 4:3 image fills the screen instead of sitting letterboxed. The
# port ships a 1440x1080 one.
export DEADSPACE_ASSET_OVERRIDE="$GAMEDIR/assets"
mkdir -p "$DEADSPACE_ASSET_OVERRIDE"
mkdir -p "$DEADSPACE_DATA_DIR"

# The zip may arrive off a filesystem with no permission bits.
$ESUDO chmod +x "$GAMEDIR/deadspace"

# gptokeyb is here for one thing only: so the standard PortMaster exit
# combination can terminate the game by name. Every button is deliberately
# left unbound in deadspace.gptk, because this port reads the pad itself
# through SDL_GameController (android/platform.cpp) and translating the same
# physical presses into keystrokes as well would double every input.
$GPTOKEYB "deadspace" -c "$GAMEDIR/deadspace.gptk" &

# Whatever the CFW needs done between gptokeyb starting and the game running.
# Older PortMaster installs do not define it, so it is called only if present.
if command -v pm_platform_helper >/dev/null 2>&1; then
  pm_platform_helper "$GAMEDIR/deadspace"
fi

# A crash was taking the log with it: tee buffers, and a segfault discards
# whatever has not been flushed - exactly the part naming the fault. stdbuf
# fixes that, but it is coreutils and this firmware may not carry it, so it is
# used only if present. Never let a debugging aid decide whether the game
# starts.
#
# Present is not the same as usable. stdbuf works by LD_PRELOADing
# libstdbuf.so, and on this device that library is 64-bit while the port is
# 32-bit, so the loader rejects it on every command it wraps:
#
#   ERROR: ld.so: object '/usr/libexec/coreutils/libstdbuf.so' from LD_PRELOAD
#   cannot be preloaded (wrong ELF class: ELFCLASS64): ignored.
#
# It says "ignored" and the command still runs, so nothing breaks - but the
# buffering stays unfixed and the message lands at the top of the log looking
# like a real fault. Running stdbuf once and checking whether it complains is
# cheaper and more portable than reading the ELF class ourselves.
USE_STDBUF=""
if command -v stdbuf >/dev/null 2>&1; then
  if [ -z "$(stdbuf -o0 true 2>&1)" ]; then
    USE_STDBUF="stdbuf -o0 -e0"
  else
    echo "stdbuf: present but not usable from a 32-bit process, skipping it"
  fi
fi

$USE_STDBUF $TASKSET "$GAMEDIR/deadspace" "$GAMEDIR/deadspace.apk"

$ESUDO kill -9 "$(pidof gptokeyb)" 2>/dev/null
rm -rf /tmp/deadspace-gl
unset LD_LIBRARY_PATH SDL_GAMECONTROLLERCONFIG DEADSPACE_DATA_DIR

pm_finish
