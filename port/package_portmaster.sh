#!/usr/bin/env bash
#
# Build a game-data-free PortMaster package for the Xperia Play port.
# `tools/` is a frozen historical scaffold, so current packaging lives here.
set -euo pipefail

cd "$(dirname "$0")"

OUT="build/deadspace.zip"
STAGE="build/pkg-portmaster"

[ -x build/deadspace ] \
    || { echo "build/deadspace missing - run make first" >&2; exit 1; }
[ -f build/libs.armhf/MANIFEST.txt ] \
    || { echo "build/libs.armhf missing - run make libs first" >&2; exit 1; }
[ -d build/libs.armhf/licenses ] \
    || { echo "build/libs.armhf/licenses missing - run make libs again" >&2; exit 1; }

rm -rf "$STAGE"
rm -f "$OUT"
mkdir -p "$STAGE/deadspace"

cp "ports/Dead Space.sh"            "$STAGE/"
cp build/deadspace                  "$STAGE/deadspace/"
cp ports/deadspace/deadspace.gptk   "$STAGE/deadspace/"
cp ports/deadspace/deadspace.eapx.json "$STAGE/deadspace/"
cp ports/deadspace/PUT_DEAD_SPACE_DATA_HERE.txt "$STAGE/deadspace/"
cp tools/eapx.py                    "$STAGE/deadspace/"
cp ports/deadspace/port.json        "$STAGE/deadspace/"
cp ports/deadspace/gameinfo.xml     "$STAGE/deadspace/"
cp ports/deadspace/cover.png        "$STAGE/deadspace/"
cp ports/deadspace/screenshot.png   "$STAGE/deadspace/"
cp ports/deadspace/README.md        "$STAGE/deadspace/"
cp ports/deadspace/CREDITS.md       "$STAGE/deadspace/"
cp ports/deadspace/grab_screen.sh   "$STAGE/deadspace/"
cp -R build/libs.armhf              "$STAGE/deadspace/"

mkdir -p "$STAGE/deadspace/licenses/libraries"
cp LICENSE "$STAGE/deadspace/licenses/LICENSE-portmaster-port.txt"
cp NOTICE.md "$STAGE/deadspace/licenses/NOTICE.md"
cp third_party/gmloader/LICENSE.md "$STAGE/deadspace/licenses/LICENSE-gmloader.md"
cp third_party/powervr/LICENSE.md "$STAGE/deadspace/licenses/LICENSE-powervr.txt"
cp third_party/vfpvector/LICENSE "$STAGE/deadspace/licenses/LICENSE-vfpvector.txt"
mv "$STAGE/deadspace/libs.armhf/licenses/"* \
   "$STAGE/deadspace/licenses/libraries/"
rmdir "$STAGE/deadspace/libs.armhf/licenses"

chmod +x "$STAGE/Dead Space.sh" "$STAGE/deadspace/deadspace" \
         "$STAGE/deadspace/grab_screen.sh" "$STAGE/deadspace/eapx.py"

find "$STAGE" \( -name '._*' -o -name '.DS_Store' \) -delete
(cd "$STAGE" && zip -qr "../../$OUT" .)

listing="$(unzip -Z1 "$OUT")"
for required in "Dead Space.sh" "deadspace/deadspace" \
                "deadspace/deadspace.gptk" "deadspace/port.json" \
                "deadspace/gameinfo.xml" "deadspace/README.md" \
                "deadspace/CREDITS.md" \
                "deadspace/cover.png" "deadspace/screenshot.png" \
                "deadspace/eapx.py" \
                "deadspace/deadspace.eapx.json" \
                "deadspace/PUT_DEAD_SPACE_DATA_HERE.txt" \
                "deadspace/licenses/LICENSE-portmaster-port.txt" \
                "deadspace/licenses/LICENSE-gmloader.md" \
                "deadspace/licenses/LICENSE-powervr.txt" \
                "deadspace/licenses/LICENSE-vfpvector.txt" \
                "deadspace/licenses/libraries/libcrypto.so.3.copyright" \
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
