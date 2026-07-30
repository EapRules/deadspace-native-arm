#!/usr/bin/env bash
#
# Build the release zip that goes in PortMaster's autoinstall/ folder.
#
# The layout follows PortMaster's packaging guide
# (https://portmaster.games/packaging.html): the launch script sits at the top
# of the zip and everything else lives in a folder named after the port, which
# is what lands in ports/ when PortMaster unpacks it.
#
# The file that is easy to miss is gameinfo.xml. PortMaster uses it to write
# the entry into EmulationStation's gamelist.xml on install - name, artwork,
# description. Without it the port installs correctly, PortMaster even lists it
# under Manage Ports, and the game still never appears in the Ports menu,
# because the frontend was never told about it. That failure looks exactly like
# a broken port and is not one.
#
# licenses/ is likewise required by the guide: every borrowed source, library
# and asset gets its licence shipped alongside the binary. The loader here is
# GPL, so this is an obligation, not a formality.
#
# What never travels is the game: no APK, no assets from it. The cover and the
# optional 4:3 splash are promotional artwork, not game data - see NOTICE.md.

set -euo pipefail

cd "$(dirname "$0")/.."

OUT="build/deadspace.zip"
STAGE="build/pkg"

[ -x build/deadspace ] || { echo "build/deadspace missing - run make first" >&2; exit 1; }
[ -d build/libs.armhf ] || { echo "build/libs.armhf missing - run tools/collect_libs.sh" >&2; exit 1; }

rm -rf "$STAGE" "$OUT"
mkdir -p "$STAGE/deadspace"

cp "ports/Dead Space.sh"           "$STAGE/"
cp build/deadspace                 "$STAGE/deadspace/"
cp ports/deadspace/deadspace.gptk  "$STAGE/deadspace/"
cp ports/deadspace/port.json       "$STAGE/deadspace/"
cp ports/deadspace/gameinfo.xml    "$STAGE/deadspace/"
cp ports/deadspace/cover.png       "$STAGE/deadspace/"
cp ports/deadspace/screenshot.png  "$STAGE/deadspace/"
cp ports/deadspace/README.md       "$STAGE/deadspace/"
# Travels with the port so a screenshot can be taken from the device itself:
# PortMaster's own tool uses kmsgrab, which needs CAP_SYS_ADMIN that a port
# launcher does not have. Reading /dev/fb0 needs no privileges.
cp ports/deadspace/grab_screen.sh  "$STAGE/deadspace/"
cp -R ports/deadspace/licenses     "$STAGE/deadspace/"
cp -R build/libs.armhf             "$STAGE/deadspace/"
cp -R ports/deadspace/assets       "$STAGE/deadspace/"

chmod +x "$STAGE/Dead Space.sh" "$STAGE/deadspace/deadspace" "$STAGE/deadspace/grab_screen.sh"

# macOS sprinkles ._* resource forks over anything it touches, and they end up
# in the zip looking like broken duplicates of every file.
if command -v dot_clean >/dev/null 2>&1; then
    dot_clean -m "$STAGE"
fi
find "$STAGE" \( -name '._*' -o -name '.DS_Store' \) -delete

( cd "$STAGE" && zip -qr "../../$OUT" . -x '.*' )

# Fail loudly rather than shipping a zip that installs but never shows up.
#
# The listing is captured once instead of being piped per check: under
# `set -o pipefail`, grep -q exits as soon as it matches, unzip gets SIGPIPE,
# and the pipeline reports failure for a file that is present. Every check
# would then "fail" on a perfectly good zip.
listing="$(unzip -l "$OUT")"
for required in "Dead Space.sh" deadspace/port.json deadspace/gameinfo.xml \
                deadspace/cover.png deadspace/screenshot.png deadspace/deadspace; do
    case "$listing" in
        *"$required"*) ;;
        *) echo "MISSING FROM ZIP: $required" >&2; exit 1 ;;
    esac
done

echo "$OUT"
unzip -l "$OUT" | tail -3
