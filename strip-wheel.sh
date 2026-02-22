#!/bin/bash
# Runs auditwheel repair then strips debug symbols from all bundled .so files.
# Usage: strip-wheel.sh <wheel> <dest_dir>
set -e

WHEEL="$1"
DEST_DIR="$2"

# Step 1: bundle FFmpeg .so files into the wheel
auditwheel repair -w /tmp/_aw_out "$WHEEL"
REPAIRED=$(ls /tmp/_aw_out/*.whl)

# Step 2: unpack (a .whl is a zip), strip unneeded symbols, repack with max deflate
TMPDIR=$(mktemp -d)
cd "$TMPDIR"
unzip -q "$REPAIRED"

# Step 3: remove spurious libpython3.x dependencies injected by distro-packaged FFmpeg
# (RPM Fusion FFmpeg on AlmaLinux 8 may include the 'pythonscript' demuxer which pulls
# in libpython3.6m.so.1.0; we never use that demuxer and Python 3.6 is not part of the
# manylinux ABI, so users without Python 3.6 get an OSError at import time).
find . -name "*.so*" | while read -r lib; do
    for py_dep in $(patchelf --print-needed "$lib" 2>/dev/null | grep 'libpython'); do
        echo "Removing spurious dependency '$py_dep' from $lib"
        patchelf --remove-needed "$py_dep" "$lib"
    done
done

find . -name "*.so*" -exec strip --strip-unneeded {} \; 2>/dev/null || true
zip -q -r -9 "$DEST_DIR/$(basename "$REPAIRED")" .

# Cleanup
cd /
rm -rf /tmp/_aw_out "$TMPDIR"
