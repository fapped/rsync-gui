#!/bin/bash
# Builds the Debian package of rsync-gui (Release, installed to /usr) and copies it to the given directory
# (default: build-deb/ in the repository).
#
#   tools/make-deb.sh [output directory]
#
# The package is built in a temporary directory under $TMPDIR (/tmp): CPack takes the modes of the files from
# the staging directory, so a file system without Unix permissions (e.g. NTFS/exFAT, where everything is
# rwx------) would give a broken package.
set -euo pipefail

repo=$(cd "$(dirname "$0")/.." && pwd)
out=${1:-$repo/build-deb}
work=$(mktemp -d)
trap 'rm -rf -- "$work"' EXIT

cmake -S "$repo" -B "$work/build" -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF \
    -DCMAKE_SKIP_INSTALL_RPATH=ON > /dev/null
cmake --build "$work/build" > /dev/null
(cd "$work/build" && cpack -G DEB > /dev/null)
mkdir -p "$out"
cp "$work/build"/*.deb "$out"/
ls -1 "$out"/*.deb
