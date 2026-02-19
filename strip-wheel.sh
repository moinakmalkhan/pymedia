#!/bin/bash
# Runs auditwheel repair then strips debug symbols from all bundled .so files.
# Usage: strip-wheel.sh <wheel> <dest_dir>
set -e

WHEEL="$1"
DEST_DIR="$2"

# Step 1: bundle FFmpeg .so files into the wheel
auditwheel repair -w /tmp/_aw_out "$WHEEL"
REPAIRED=$(ls /tmp/_aw_out/*.whl)

# Step 2: unpack (a .whl is a zip), strip debug symbols, repack
TMPDIR=$(mktemp -d)
cd "$TMPDIR"
unzip -q "$REPAIRED"
find . -name "*.so*" -exec strip --strip-debug {} \; 2>/dev/null || true
zip -qr "$DEST_DIR/$(basename "$REPAIRED")" .

# Cleanup
cd /
rm -rf /tmp/_aw_out "$TMPDIR"
