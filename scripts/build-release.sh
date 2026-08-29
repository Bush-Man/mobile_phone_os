#!/usr/bin/env bash
#
# build-release.sh - the phase-16 release image builder (item 89).
#
# Assembles a reproducible release bundle from a completed build:
#
#   release/
#     kernel8.img      raw AArch64 boot image (kernel8.elf stripped)
#     kernel.elf       unstripped ELF for post-mortems
#     manifest.txt     git hash, date, sizes, sha256 of every file
#     docs/            RELEASE.md, FLASHING.md, BOARD_BRINGUP.md
#
# Usage: build-release.sh <build-dir> <git-hash> [board]
# Boards: qemu-virt (default), pinephone (see docs/BOARD_BRINGUP.md).
# This script is part of the delivered tooling; CI runs it after
# `make all` -- it is NOT part of the kernel build itself.

set -euo pipefail

BUILD_DIR="${1:-build}"
GIT_HASH="${2:-unknown}"
BOARD="${3:-qemu-virt}"
OUT="$BUILD_DIR/release"

if [ ! -f "$BUILD_DIR/kernel8.img" ]; then
    echo "build-release: $BUILD_DIR/kernel8.img missing -- run make all first" >&2
    exit 1
fi

rm -rf "$OUT"
mkdir -p "$OUT/docs"

cp "$BUILD_DIR/kernel8.img" "$OUT/kernel8.img"
cp "$BUILD_DIR/kernel.elf"  "$OUT/kernel.elf"
cp docs/RELEASE.md docs/FLASHING.md docs/BOARD_BRINGUP.md "$OUT/docs/"

cat > "$OUT/manifest.txt" <<EOF
mobile_phone_os release manifest
project:  mobile_phone_os (AArch64 bare-metal phone OS)
board:    $BOARD
git:      $GIT_HASH
date:     $(date -u +"%Y-%m-%dT%H:%M:%SZ")
tooling:  aarch64-linux-gnu-gcc $(aarch64-linux-gnu-gcc -dumpversion), GNU ld
files:
EOF

(
    cd "$OUT"
    for f in kernel8.img kernel.elf docs/RELEASE.md docs/FLASHING.md \
             docs/BOARD_BRINGUP.md; do
        printf '  %-24s %10s bytes  sha256=%s\n' \
            "$f" "$(stat -c%s "$f")" "$(sha256sum "$f" | cut -d' ' -f1)" \
            >> manifest.txt
    done
)

echo "build-release: bundle ready in $OUT"
echo "build-release: flash per docs/FLASHING.md (board: $BOARD)"
