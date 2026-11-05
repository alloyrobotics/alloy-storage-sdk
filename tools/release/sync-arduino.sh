#!/usr/bin/env bash
# Copy the Arduino package (mcu/arduino) to the root of the Library Manager repo.
#
# Library Manager only indexes a library at the root of its own repository, so the Arduino package
# is published from alloyrobotics/alloy-logger-arduino. That repo also hosts the alloylogger.com
# site, which this script leaves alone: it replaces only the library's own paths.
#
#   tools/release/sync-arduino.sh /path/to/alloy-logger-arduino
#
# Then review, commit with the printed message, tag with the library.properties version, and push.
set -euo pipefail

SDK="$(git -C "$(dirname "$0")" rev-parse --show-toplevel)"
DEST="${1:?usage: sync-arduino.sh <alloy-logger-arduino checkout>}"
PKG="$SDK/mcu/arduino"

[ -f "$DEST/library.properties" ] || { echo "not a library checkout: $DEST" >&2; exit 1; }
if [ -n "$(git -C "$SDK" status --porcelain -- mcu/arduino)" ]; then
  echo "mcu/arduino has uncommitted changes; commit them first" >&2
  exit 1
fi

# Library-owned directories are mirrored exactly; library-owned files are overwritten.
for dir in src examples test/portable extras/uno-probe extras/platformio; do
  mkdir -p "$DEST/$dir"
  rsync -a --delete "$PKG/$dir/" "$DEST/$dir/"
done
mkdir -p "$DEST/docs"
rsync -a "$PKG/docs/" "$DEST/docs/"
for file in library.properties library.json README.md THIRD_PARTY_NOTICES LICENSE; do
  cp "$PKG/$file" "$DEST/$file"
done

version="$(sed -n 's/^version=//p' "$PKG/library.properties")"
sha="$(git -C "$SDK" rev-parse --short HEAD)"
echo "synced mcu/arduino @ $sha (version $version) into $DEST"
echo "commit message: release: Arduino $version from alloy-storage-sdk@$sha"
