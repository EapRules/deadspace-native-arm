#!/usr/bin/env bash
#
# Build a game-data-free PortMaster package for the Xperia Play port.
# `tools/` is a frozen historical scaffold, so current packaging lives here.
set -euo pipefail

cd "$(dirname "$0")"

OUT="build/deadspace-portmaster.zip"
STAGE="build/pkg-portmaster"

[ -x build/deadspace ] \
    || { echo "build/deadspace missing - run make first" >&2; exit 1; }
[ -f build/libs.armhf/MANIFEST.txt ] \
    || { echo "build/libs.armhf missing - run make libs first" >&2; exit 1; }

rm -rf "$STAGE"
rm -f "$OUT"
mkdir -p "$STAGE/deadspace"

cp "ports/Dead Space.sh"            "$STAGE/"
cp build/deadspace                  "$STAGE/deadspace/"
cp ports/deadspace/deadspace.gptk   "$STAGE/deadspace/"
cp ports/deadspace/port.json        "$STAGE/deadspace/"
cp ports/deadspace/gameinfo.xml     "$STAGE/deadspace/"
cp ports/deadspace/README.md        "$STAGE/deadspace/"
cp ports/deadspace/grab_screen.sh   "$STAGE/deadspace/"
cp -R build/libs.armhf              "$STAGE/deadspace/"

chmod +x "$STAGE/Dead Space.sh" "$STAGE/deadspace/deadspace" \
         "$STAGE/deadspace/grab_screen.sh"

find "$STAGE" \( -name '._*' -o -name '.DS_Store' \) -delete
(cd "$STAGE" && zip -qr "../../$OUT" .)

listing="$(unzip -Z1 "$OUT")"
for required in "Dead Space.sh" "deadspace/deadspace" \
                "deadspace/deadspace.gptk" "deadspace/port.json" \
                "deadspace/gameinfo.xml" "deadspace/README.md" \
                "deadspace/libs.armhf/MANIFEST.txt"; do
    case "$listing" in
        *"$required"*) ;;
        *) echo "package missing $required" >&2; exit 1 ;;
    esac
done

case "$listing" in
    *libEAMGameDeadSpace.so*|*assets/published/*)
        echo "refusing package: proprietary game data found" >&2
        exit 1
        ;;
esac

echo "$OUT"
du -h "$OUT"
