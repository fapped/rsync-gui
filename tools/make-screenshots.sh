#!/bin/bash
# Regenerates the README screenshots in docs/screenshots/ (rsync-gui-*.png).
# Runs the real application on the current desktop (windows appear for a few seconds) with demo
# data in /tmp/rsync-gui-demo and a temporary configuration, so the remembered options and recent
# destinations of the user are neither shown nor changed. The Dolphin menu screenshot
# (context-menu.png) is taken by hand.
set -euo pipefail

repo=$(cd "$(dirname "$0")/.." && pwd)
build="$repo/build-screenshots"
demo=/tmp/rsync-gui-demo
config=$(mktemp -d)
trap 'rm -rf -- "$config"' EXIT

cmake -S "$repo/tools/screenshots" -B "$build" -G Ninja -DCMAKE_BUILD_TYPE=Release > /dev/null
cmake --build "$build" > /dev/null

rm -rf -- "$demo"
mkdir -p "$demo/Photos/2026/summer" "$demo/Documents" "$demo/Backup"
for i in 1 2 3 4 5 6; do
    head -c $((300000 + i * 41000)) /dev/urandom > "$demo/Photos/2026/summer/IMG_$((4100 + i)).jpg"
done
head -c 180000 /dev/urandom > "$demo/Photos/2026/cover.png"
head -c 92000 /dev/urandom > "$demo/Documents/report.pdf"

mkdir -p "$repo/docs/screenshots"
# Only the presets of the repository (next to the executable), not the user's own ones.
SCREENSHOT_DIR="$repo/docs/screenshots" XDG_CONFIG_HOME="$config" XDG_DATA_HOME="$config/data" \
    "$build/rsync-gui-screenshots" --preset rsync-copy-verify --destination "$demo/Backup" \
    "$demo/Photos" "$demo/Documents/report.pdf"

rm -rf -- "$demo"
