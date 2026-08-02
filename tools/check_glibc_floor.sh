#!/usr/bin/env bash
#
# Fail the build if any shipped ELF demands a glibc newer than the floor.
#
# A PortMaster port lands on CFWs whose glibc is years older than the machine
# it was cross-compiled on. A binary (or bundled .so) references the highest
# GLIBC_x.y symbol version present in the toolchain it was built with, and the
# device's dynamic linker refuses to load it if it cannot satisfy that version:
# "GLIBC_2.34 not found". ArkOS/AeolusUX ship glibc 2.28-2.31, so anything above
# 2.31 breaks them - and the qemu/Mesa harness never catches it, because it runs
# on a modern host with a new glibc. This gate is the thing that does catch it,
# at build time, before the zip is ever shipped.
#
# Run inside the build container (needs the cross readelf), e.g. from `make libs`:
#   tools/check_glibc_floor.sh build/deadspace build/libs.armhf
#
# GLIBC_FLOOR overrides the ceiling (default 2.31 = Debian bullseye).
#
# Deliberately NOT `set -e`/`pipefail`: the whole job of this script is to reach
# a verdict, and the normal case (a version does not exceed the floor, a file
# has no GLIBC refs at all) makes those benign non-zero exits. A build gate that
# aborts on its own successful checks is worse than useless. Errors are handled
# explicitly instead.
set -u

FLOOR="${GLIBC_FLOOR:-2.31}"
READELF="${READELF:-arm-linux-gnueabihf-readelf}"

command -v "$READELF" >/dev/null 2>&1 || {
    echo "check_glibc_floor: $READELF not found (run inside the build container)" >&2
    exit 2
}

# a > b for dotted versions, using sort -V as the ordering oracle. Returns 0
# (true) only when $1 is strictly greater than $2.
version_gt() {
    [ "$1" = "$2" ] && return 1
    [ "$(printf '%s\n%s\n' "$1" "$2" | sort -V | tail -n1)" = "$1" ]
}

# Collect the ELF targets: every explicit arg that is a file, plus every *.so*
# inside any directory arg. Directories let `make libs` pass build/libs.armhf
# without enumerating its contents.
targets=()
for arg in "$@"; do
    if [ -d "$arg" ]; then
        while IFS= read -r f; do targets+=("$f"); done \
            < <(find "$arg" -type f -name '*.so*' | sort)
    elif [ -f "$arg" ]; then
        targets+=("$arg")
    else
        echo "check_glibc_floor: no such file or directory: $arg" >&2
        exit 2
    fi
done

[ "${#targets[@]}" -gt 0 ] || {
    echo "check_glibc_floor: nothing to check (pass the binary and libs dir)" >&2
    exit 2
}

status=0
for f in "${targets[@]}"; do
    # The *.so* glob also catches text like libbsd.so.0.copyright that shares
    # the stem; readelf -h fails on non-ELF, so skip those quietly.
    "$READELF" -h "$f" >/dev/null 2>&1 || continue

    # --version-info lists the GLIBC_x.y versions the file references. Take the
    # unique set; grep returning no match (a file with no versioned refs) is
    # fine and yields an empty list.
    versions=$("$READELF" --version-info "$f" 2>/dev/null \
        | grep -oE 'GLIBC_[0-9]+\.[0-9]+' \
        | sed 's/GLIBC_//' | sort -u -V)

    over=""
    highest="none"
    for v in $versions; do
        highest="$v"                                   # $versions is sorted -V
        if version_gt "$v" "$FLOOR"; then
            over="$over $v"
        fi
    done

    if [ -n "$over" ]; then
        printf '  FAIL  %-28s needs GLIBC >%s:%s\n' \
            "$(basename "$f")" "$FLOOR" "$over"
        status=1
    else
        printf '  ok    %-28s max GLIBC_%s\n' "$(basename "$f")" "$highest"
    fi
done

if [ "$status" -ne 0 ]; then
    echo
    echo "check_glibc_floor: shipped ELF exceeds the glibc floor ($FLOOR)." >&2
    echo "  The port would fail on ArkOS/AeolusUX with 'GLIBC_x.y not found'." >&2
    echo "  Build against an older base (Dockerfile.build) - see this script's header." >&2
    exit 1
fi

echo "check_glibc_floor: all clear, everything fits glibc <= $FLOOR"
