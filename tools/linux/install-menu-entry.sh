#!/bin/sh
# Adds Boat of Daphnes to the application menu for this user (~/.local/share/applications), pointing at this folder.
# Remove it again by deleting ~/.local/share/applications/BoatOfDaphnes.desktop.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
apps=${XDG_DATA_HOME:-$HOME/.local/share}/applications
mkdir -p "$apps"
sed -e "s|^Exec=.*|Exec=\"$here/boat-of-daphnes\"|" -e "s|^Icon=.*|Icon=$here/boat-of-daphnes.png|" \
    -e "s|^Terminal=.*|Terminal=false\nPath=$here|" "$here/BoatOfDaphnes.desktop" > "$apps/BoatOfDaphnes.desktop"
echo "Added $apps/BoatOfDaphnes.desktop"
