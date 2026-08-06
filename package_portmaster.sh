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
cp third_party/deadspace-vita/LICENSE "$STAGE/deadspace/licenses/LICENSE-deadspace-vita.txt"
cp third_party/powervr/LICENSE.md "$STAGE/deadspace/licenses/LICENSE-powervr.txt"
cp third_party/vfpvector/LICENSE "$STAGE/deadspace/licenses/LICENSE-vfpvector.txt"
mv "$STAGE/deadspace/libs.armhf/licenses/"* \
   "$STAGE/deadspace/licenses/libraries/"
rmdir "$STAGE/deadspace/libs.armhf/licenses"

chmod +x "$STAGE/Dead Space.sh" "$STAGE/deadspace/deadspace" \
         "$STAGE/deadspace/grab_screen.sh" "$STAGE/deadspace/eapx.py"

find "$STAGE" \( -name '._*' -o -name '.DS_Store' \) -delete
(cd "$STAGE" && zip -qr "../../$OUT" .)

# PortMaster rewrites an unsigned root launcher in place to add this line. Its
# implementation opens the file with mode "w" before writing, so an interrupted
# install can leave a zero-byte launcher on exFAT. Ship the canonical signature
# ourselves: with the release filename below, PortMaster recognizes it and does
# not touch the launcher after extraction.
EXPECTED_SIGNATURE="# PORTMASTER: deadspace-portmaster.zip, Dead Space.sh"
ACTUAL_SIGNATURE="$(unzip -p "$OUT" "Dead Space.sh" | sed -n '2p')"
[ "$ACTUAL_SIGNATURE" = "$EXPECTED_SIGNATURE" ] || {
    echo "package has wrong PortMaster signature: $ACTUAL_SIGNATURE" >&2
    exit 1
}
[ "$(unzip -p "$OUT" "Dead Space.sh" | wc -c | tr -d ' ')" -gt 0 ] || {
    echo "package has an empty launcher" >&2
    exit 1
}
unzip -tq "$OUT" >/dev/null

# The eapx in tools/ is a copy of the canonical one and drifts silently: this
# port shipped 0.4.1 while the canonical tree was at 0.4.2, because nobody
# compared them. Refuse to package on a mismatch instead of trusting memory.
canonical="${EAPX_CANONICAL:-$HOME/Projects/Others/handheld/eapx/eapx.py}"
if [ -f "$canonical" ]; then
  if ! cmp -s tools/eapx.py "$canonical"; then
    echo "refusing package: tools/eapx.py differs from the canonical $canonical" >&2
    echo "  packaged:  $(sed -n 's/^VERSION = "\(.*\)"/\1/p' tools/eapx.py)" >&2
    echo "  canonical: $(sed -n 's/^VERSION = "\(.*\)"/\1/p' "$canonical")" >&2
    exit 1
  fi
else
  echo "note: canonical eapx not found at $canonical; packaged copy not verified" >&2
fi

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
                "deadspace/licenses/LICENSE-deadspace-vita.txt" \
                "deadspace/licenses/LICENSE-powervr.txt" \
                "deadspace/licenses/LICENSE-vfpvector.txt" \
                "deadspace/licenses/libraries/libstdc++.so.6.copyright" \
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

# A stale zip with an old binary passes every check above - they all pass on an
# old binary. Comparing the hashes is the only check that catches it; twice a
# release was nearly published with a binary older than the one just verified.
built_sha="$(shasum -a 256 build/deadspace | cut -d' ' -f1)"
packed_sha="$(unzip -p "$OUT" deadspace/deadspace | shasum -a 256 | cut -d' ' -f1)"
[ "$built_sha" = "$packed_sha" ] || {
    echo "refusing package: the zipped binary is not the one just built" >&2
    echo "  built:  $built_sha" >&2
    echo "  packed: $packed_sha" >&2
    exit 1
}

echo "$OUT"
echo "binary sha256: $built_sha"
du -h "$OUT"
