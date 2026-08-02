#!/usr/bin/env bash
#
# Capture the console's screen, from the console itself.
#
# PortMaster's screenshot tool uses ffmpeg's kmsgrab, which wants CAP_SYS_ADMIN.
# A port launcher runs as an ordinary user, so it fails with "No handle set on
# framebuffer" and there is no screenshot. Reading /dev/fb0 needs no privileges.
#
# Run it ON THE DEVICE, from a port entry or over SSH. It writes a raw dump plus
# a text file with the geometry, because the framebuffer layout is not something
# to assume: an R36S reported 1280x1440, which turned out to be two 720-high
# halves at 32bpp BGRA. Convert on the computer, where there is a PNG encoder.
#
# The point is not only the screenshot for the release. Two captures and a diff
# answer "did that change do anything" without anyone watching the screen.

set -u

OUT="${1:-/roms2/screen}"
mkdir -p "$(dirname "$OUT")" 2>/dev/null

if [ ! -r /dev/fb0 ]; then
    echo "no /dev/fb0 (KMS-only device?). Try: ls /dev/fb* /dev/dri/*" >&2
    exit 1
fi

# Geometry first: without it the dump is unconvertible.
{
    echo "=== $(date) ==="
    for f in /sys/class/graphics/fb0/virtual_size \
             /sys/class/graphics/fb0/bits_per_pixel \
             /sys/class/graphics/fb0/stride; do
        [ -r "$f" ] && echo "$(basename "$f"): $(cat "$f")"
    done
    echo "size: $(stat -c %s /dev/fb0 2>/dev/null || echo unknown)"
} > "$OUT.info.txt" 2>&1

dd if=/dev/fb0 of="$OUT.raw" bs=1M 2>>"$OUT.info.txt"
sync

echo "wrote $OUT.raw ($(stat -c %s "$OUT.raw" 2>/dev/null) bytes)"
echo "geometry in $OUT.info.txt"
echo
echo "On the computer, with the geometry from that file:"
echo "  magick -size WIDTHxHEIGHT -depth 8 bgra:$OUT.raw shot.png"
echo "If the panel is split in halves, crop the top one:"
echo "  magick shot.png -crop WIDTHx480+0+0 +repage shot-top.png"
