#!/bin/sh
# usage: run_server.sh <board.kicad_pcb> [extra env assignments...]
# Starts kicad-cli api-server from the build tree on $KICADD_SOCK (default KiCadD/run/kicadd.sock).
HERE=$(cd "$(dirname "$0")/.." && pwd)
BOARD=$1; shift
mkdir -p "$HERE/run"
exec env KICAD_RUN_FROM_BUILD_DIR=1 "$@" "$HERE/../build/kicad/kicad-cli" api-server \
    --socket "${KICADD_SOCK:-$HERE/run/kicadd.sock}" "$BOARD"
