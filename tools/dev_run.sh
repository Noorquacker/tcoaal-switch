#!/bin/sh
# Rebuild the runtime JS into an existing romfs and run the host build.
#   tools/dev_run.sh <romfs> <save dir> [timeout seconds]
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
ROMFS=$1; SAVE=$2; T=${3:-20}
rm -rf "$ROMFS/runtime" && cp -r "$ROOT/runtime/js" "$ROMFS/runtime"
"$ROOT/build/host/jsbc" --root "$ROMFS" "$ROMFS"/runtime/*.js
exec timeout "$T" "$ROOT/build/host/tcoaal" "$ROMFS" "$SAVE"
