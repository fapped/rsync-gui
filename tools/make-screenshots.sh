#!/bin/bash
# Regenerates the README screenshots in docs/screenshots/ (rsync-gui-*.png).
# Runs the real application offscreen in the Breeze style (nothing appears on the screen, no Plasma
# notifications) with demo data in /tmp/rsync-gui-demo and a temporary configuration, so the remembered
# options and recent destinations of the user are neither shown nor changed. The Dolphin menu screenshots
# (context-menu-*.png) are taken by hand.
set -euo pipefail
# The times in the log and the dialog do not depend on the time zone of the machine.
export TZ=UTC

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
    head -c $((40000000 + i * 4100000)) /dev/urandom > "$demo/Photos/2026/summer/IMG_$((4100 + i)).jpg"
done
head -c 180000 /dev/urandom > "$demo/Photos/2026/cover.png"
head -c 92000 /dev/urandom > "$demo/Documents/report.pdf"

# Names that are not valid UTF-8 (Latin-1) for the built-in file dialog; not used by the job itself.
names="$demo/Old disk"
mkdir -p "$names/"$'Fr\xfchling' "$names/scans"
for entry in $'caf\xe9.jpg':2310000 $'caf\xe8.jpg':2290000 $'M\xfcnchen station.jpg':3104000 \
             $'Stra\xdfe 12.jpg':1876000 harbour.jpg:2650000 $'na\xefve sketch.png':512000 \
             'résumé.pdf':88000 notes.txt:2400; do
    head -c "${entry##*:}" /dev/zero > "$names/${entry%:*}"
    touch -d '2004-07-14 18:22' "$names/${entry%:*}"
done
touch -d '2004-07-11 09:05' "$names/"$'Fr\xfchling' "$names/scans"

mkdir -p "$repo/docs/screenshots"
# Only the presets of the repository (next to the executable), not the user's own ones; English number
# and date formats, whatever the locale of the user.
SCREENSHOT_DIR="$repo/docs/screenshots" SCREENSHOT_NAMES_DIR="$names" \
    XDG_CONFIG_HOME="$config" XDG_DATA_HOME="$config/data" \
    QT_QPA_PLATFORM=offscreen QT_QPA_PLATFORMTHEME=kde QT_STYLE_OVERRIDE=Breeze \
    DBUS_SESSION_BUS_ADDRESS=unix:path=/nonexistent \
    LC_ALL=en_US.UTF-8 \
    "$build/rsync-gui-screenshots" --preset rsync-copy-verify --destination "$demo/Backup" \
    "$demo/Photos" "$demo/Documents/report.pdf"

rm -rf -- "$demo"
