#!/usr/bin/env bash
#
# Regenerates thunks/libc/generated/{impl_tab.h,impl_header.h}.
#
# These two headers are the bionic libc export table, derived by libclang from
# thunks/libc/*.cpp. They are checked in on purpose: the build image the
# harness uses has no clang and no python bindings, and adding them there would
# mean rebuilding the one environment that is already known to work. Codegen
# therefore happens in a separate throwaway image, and only its output ships.
#
# Run from the port directory. Re-run whenever thunks/libc changes.
set -euo pipefail

PORT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ARCH="${ARCH:-arm-linux-gnueabihf}"

docker build -t deadspace-codegen -f - "$PORT_DIR" >/dev/null <<'DOCKERFILE'
FROM deadspace-build
RUN apt-get update && apt-get install -y --no-install-recommends \
      clang python3-clang libclang-dev python3 \
    && rm -rf /var/lib/apt/lists/*
DOCKERFILE

docker run --rm -v "$PORT_DIR":/src -w /src deadspace-codegen bash -lc "
    PYTHONPATH=/src/tools/clang19_compat python3 thunks/libc/generate_libc.py $ARCH \
        --llvm-library-file /usr/lib/llvm-19/lib/libclang.so \
        --llvm-includes /usr/$ARCH/include"

cp "$PORT_DIR/build/$ARCH/thunks/libc/impl_tab.h" \
   "$PORT_DIR/build/$ARCH/thunks/libc/impl_header.h" \
   "$PORT_DIR/thunks/libc/generated/"
echo "regenerated thunks/libc/generated/ for $ARCH"
