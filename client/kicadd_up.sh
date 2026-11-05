#!/bin/sh
# Start KiCadD (kicad-cli api-server with the board resident) and its MCP endpoint.
#
#   client/kicadd_up.sh /path/board.kicad_pcb [PORT]
#
# MCP Streamable HTTP on 127.0.0.1:PORT/mcp (default 8765), 2D view on 127.0.0.1:PORT/. Set KICADD_MCP_TOKEN to require
# a bearer token, and KICADD_MCP_HOST to bind elsewhere (a token is then mandatory).
# Ctrl-C stops both.
set -eu
HERE=$(cd "$(dirname "$0")/.." && pwd)
BOARD=${1:?usage: kicadd_up.sh BOARD.kicad_pcb [PORT]}
PORT=${2:-8765}
HOST=${KICADD_MCP_HOST:-127.0.0.1}
SOCK=${KICADD_SOCK:-$HERE/run/kicadd.sock}
MCP="$HERE/../build/kicadd-mcp"

mkdir -p "$HERE/run"
KICADD_SOCK=$SOCK "$HERE/client/run_server.sh" "$BOARD" > "$HERE/run/daemon.log" 2>&1 &
DAEMON=$!
trap 'kill $DAEMON 2>/dev/null' EXIT INT TERM

echo "daemon pid $DAEMON (log: $HERE/run/daemon.log), socket $SOCK"
echo "2D view: http://$HOST:$PORT/${KICADD_MCP_TOKEN:+?token=...}   MCP: http://$HOST:$PORT/mcp"
"$MCP" --socket "$SOCK" --http "$HOST:$PORT"
