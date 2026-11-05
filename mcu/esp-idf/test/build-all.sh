#!/bin/bash
# Builds every example for every supported target. Usage: test/build-all.sh [extra sdkconfig defaults file]
# Needs an exported ESP-IDF environment (. $IDF_PATH/export.sh). Exits non-zero on the first failure.
set -euo pipefail
cd "$(dirname "$0")/.."
ROOT=$PWD
TARGETS=${TARGETS:-"esp32 esp32s2 esp32s3 esp32c3 esp32c6"}
EXTRA=${1:+$(realpath "$1")}
for ex in "$ROOT"/alloy_storage/examples/*; do
  for t in $TARGETS; do
    defaults="sdkconfig.defaults"
    [ -n "$EXTRA" ] && defaults="$defaults;$EXTRA"
    echo "=== $ex $t"
    idf.py -C "$ex" -B "$ex/build-$t" -D SDKCONFIG="$ex/build-$t/sdkconfig" \
      -D SDKCONFIG_DEFAULTS="$defaults" -D IDF_TARGET="$t" build > "$ex/build-$t.log" 2>&1 \
      || { tail -40 "$ex/build-$t.log"; exit 1; }
    grep -E "binary size|free" "$ex/build-$t.log" | tail -1
  done
done
