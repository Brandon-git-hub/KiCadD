#!/bin/sh
# Start KiCadD (kicad-cli api-server with the board resident) and its MCP endpoint + 2D view.
#
#   client/kicadd_up.sh /path/board.kicad_pcb [PORT]
#
# Default: 127.0.0.1:PORT (8765) — MCP at /mcp, 2D view at /.
# KICADD_MCP_HOST=0.0.0.0 makes it reachable from outside this machine/container (e.g. a
# Docker host running VS Code Tunnel). Remote peers then need a token: KICADD_MCP_TOKEN, or
# one is generated into run/token. Local peers (this container: agents, code-server proxy)
# don't need it. Ctrl-C stops both.
set -eu
HERE=$(cd "$(dirname "$0")/.." && pwd)
BOARD=${1:?usage: kicadd_up.sh BOARD.kicad_pcb [PORT]}
PORT=${2:-8765}
HOST=${KICADD_MCP_HOST:-127.0.0.1}
SOCK=${KICADD_SOCK:-$HERE/run/kicadd.sock}
MCP="$HERE/../build/kicadd-mcp"
mkdir -p "$HERE/run"

# A daemon still shutting down on the same socket path would unlink our new socket when it
# finally exits (it removes "its" socket file), leaving us unreachable. Wait for it.
for i in $(seq 1 60); do
    pgrep -f -- "--socket $SOCK" >/dev/null || break
    [ "$i" = 1 ] && echo "waiting for the previous KiCadD on $SOCK to exit…"
    sleep 0.5
done
if pgrep -f -- "--socket $SOCK" >/dev/null; then
    echo "another KiCadD is still using $SOCK; stop it first" >&2
    exit 1
fi
# Nobody serves $SOCK, so a leftover socket file is orphaned (a daemon that was killed).
# Upstream only removes it when it can take the directory's api.lock, which another daemon
# in the same directory holds; it would then listen on api-<pid>.sock instead.
[ -S "$SOCK" ] && rm -f "$SOCK"

EXTRA=""
TOKEN=${KICADD_MCP_TOKEN:-}
case "$HOST" in
    127.0.0.1|localhost|::1) ;;
    *)
        if [ -z "$TOKEN" ]; then
            [ -s "$HERE/run/token" ] || ( umask 077; head -c 16 /dev/urandom | od -An -tx1 | tr -d ' \n' > "$HERE/run/token" )
            TOKEN=$(cat "$HERE/run/token")
        fi
        EXTRA="--trust-loopback"
        ;;
esac

KICADD_SOCK=$SOCK "$HERE/client/run_server.sh" "$BOARD" > "${KICADD_LOG:-$HERE/run/daemon.log}" 2>&1 &
DAEMON=$!
trap 'kill $DAEMON 2>/dev/null' EXIT INT TERM

echo "daemon pid $DAEMON (log: ${KICADD_LOG:-$HERE/run/daemon.log}), socket $SOCK"
if [ -n "$EXTRA" ]; then
    for ip in $(hostname -I 2>/dev/null); do
        echo "remote 2D view: http://$ip:$PORT/?token=$TOKEN   (MCP: http://$ip:$PORT/mcp + Bearer token)"
    done
fi
echo "local  2D view: http://127.0.0.1:$PORT/   MCP: http://127.0.0.1:$PORT/mcp"
KICADD_MCP_TOKEN=$TOKEN "$MCP" --socket "$SOCK" --http "$HOST:$PORT" $EXTRA
