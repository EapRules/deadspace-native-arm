#!/usr/bin/env bash
#
# Build a throwaway Ports entry that plays the game and screenshots itself.
#
# The release needs a screenshot of the game actually running, and getting one
# off this hardware is awkward: PortMaster's own tool uses ffmpeg's kmsgrab,
# which wants CAP_SYS_ADMIN, and a port launcher runs as an ordinary user. So
# it fails with "No handle set on framebuffer" and there is no screenshot.
#
# Reading /dev/fb0 needs no privileges at all. This wraps the normal launcher
# and, in the background, dumps the framebuffer a few times while the game is
# up. Several shots rather than one because nobody is watching the screen to
# decide when the good frame is - it is cheaper to take four and pick later.
#
# Usage:  tools/make_capture_entry.sh <destination ports/ directory>
set -euo pipefail

cd "$(dirname "$0")/.."
DST="${1:?usage: make_capture_entry.sh <ports dir on the card>}"

[ -d "$DST" ] || { echo "no such directory: $DST" >&2; exit 1; }

# The capture helper travels with the port so it can be run over SSH later too.
cp tools/grab_screen.sh "$DST/deadspace/grab_screen.sh"
chmod +x "$DST/deadspace/grab_screen.sh"

# Same launcher, plus a background capture loop and its own log.
sed -e 's|export PORT_32BIT="Y"|export PORT_32BIT="Y"\n\n# Throwaway entry: screenshots itself, see tools/make_capture_entry.sh\n(\n  for delay in 20 15 15 20; do\n    sleep "$delay"\n    "/$directory/ports/deadspace/grab_screen.sh" "/$directory/ports/deadspace/shot-$(date +%s)" >/dev/null 2>\&1\n  done\n) \&|' \
    -e 's|\$GAMEDIR/log\.txt|$GAMEDIR/log-capture.txt|g' \
    "ports/Dead Space.sh" > "$DST/Dead Space (captura).sh"
chmod +x "$DST/Dead Space (captura).sh"

echo "wrote:"
echo "  $DST/Dead Space (captura).sh"
echo "  $DST/deadspace/grab_screen.sh"
echo
echo "Run that entry on the console, play past the menus, and let it sit."
echo "It writes shot-<timestamp>.raw plus a .info.txt with the geometry into"
echo "ports/deadspace/ at roughly 20s, 35s, 50s and 70s."
