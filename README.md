# KiCadD

Headless, in-memory KiCad daemon for LLM (MCP) and human co-editing.
Built as a thin extension of upstream `kicad-cli api-server`, not a fork of the core.

## Requirements this project tracks

1. Headless/CLI on a Linux server + MCP + remote 2D view/edit
2. No SWIG; Python only as an isolated IPC client (no host pollution)
3. Component library backed by LCSC / JLC online catalog

## Layout

| Path | What |
|---|---|
| `proto/kicadd/board.proto` | Compact, spatially scoped board API (`kicadd.board.*`) |
| `src/pcb/kicadd_handler_pcb.*` | Handler; shares the resident `PCB_CONTEXT` with upstream `API_HANDLER_PCB` |
| `cmake/kicadd.cmake` | Generates C++/Python protobuf, defines `kicadd_proto`, `KICADD_PCB_SRCS` |
| `patches/0001-*.patch` | Upstream hooks (all guarded by `KICADD_DIR` / `#ifdef KICADD`) + api-server wake-up fix |
| `src/pcb/kicadd_local_drc.*` | Local (incremental) DRC |
| `mcp/` | `kicadd-mcp`: MCP server (C++; stdio + Streamable HTTP via Boost.Beast) + web view hub |
| `src/pcb/kicadd_render.*` | Change journal (`BOARD_LISTENER`) + render snapshot for remote views |
| `web/index.html` | 2D view (vanilla JS + Canvas 2D), embedded into `kicadd-mcp` at build time |
| `client/` | Python IPC client, benchmark, verification + A/B scripts, `run_server.sh` |

### Upstream patch (~320 lines, 13 files)

- **Hooks** (guarded by `KICADD_DIR` / `#ifdef KICADD`): `CMakeLists.txt`, `pcbnew/CMakeLists.txt`,
  `pcbnew/pcbnew.cpp` register `KICADD_HANDLER_PCB` next to `API_HANDLER_PCB`.
- **api-server wake-up**: `SetRequestQueuedNotifier()` replaces the fixed 10 ms poll
  (`api_server.h/.cpp`, `command_api_server.cpp`). Ping 10.1 ms → 49 µs.
- **Incremental connectivity** (`connectivity_algo.*`): `SearchClusters(..., aDirtyOnly)`.
  Ratsnest clusters are per-net, so only dirty nets are searched; net propagation crosses nets,
  so only components reachable from a dirty-net item are searched. `Remove()` marks the
  neighbours' nets dirty so split-off components stay visible.
- **Teardrop update** (`teardrop.*`, `connectivity_data.*`, `board_commit.cpp`): the
  incremental `UpdateTeardrops` visits only tracks that are dirty or directly connected to a
  dirty pad/via; skips the board-wide track cache when no candidate has teardrops enabled;
  returns whether it created anything, and the commit re-runs `RecalculateRatsnest` only then.

- **Lazy ratsnest for large nets** (`connectivity_data.*`, `board.cpp`): an optional
  per-net predicate defers ratsnest rebuild (never net propagation). Deferred nets are
  *cleared* and marked pending, never left stale. Every ratsnest reader (`GetUnconnectedCount`,
  `GetRatsnestForNet`, `RunOnUnconnectedEdges`, `GetNodeCount`, local ratsnest) rebuilds them
  first. A flush can run mid-commit, so it saves and restores the other nets' dirty flags.
  Ratsnest exclusions move from `BOARD::UpdateRatsnestExclusions` into connectivity
  (`SetRatsnestExclusions`), which re-applies them when deferred nets are rebuilt. Without
  this, that function read every edge on each commit and defeated the deferral. The
  predicate is only installed by KiCadD's `MoveDelta`; with no predicate, behaviour is upstream's.

These changes are always on. A/B switches (env vars, read once at startup):
`KICADD_FULL_CONNECTIVITY=1` (all upstream behaviour), or individually
`KICADD_FULL_CLUSTERS`, `KICADD_FULL_TEARDROPS`, `KICADD_ALWAYS_RECALC`.

## Build

```sh
cd kicad && git apply ../KiCadD/patches/0001-kicadd-hooks-and-api-server-wakeup.patch   # if not applied
cmake -S kicad -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DKICAD_BUILD_QA_TESTS=OFF -DKICAD_BUILD_I18N=OFF -DKICAD_USE_SENTRY=OFF \
      -DKICADD_DIR=$PWD/KiCadD
ninja -C build kicad-cli pcbnew_kiface eeschema_kiface kicadd-mcp
```

## Run

```sh
# Unix socket paths are limited to 108 bytes — keep --socket short.
KICAD_RUN_FROM_BUILD_DIR=1 build/kicad/kicad-cli api-server \
    --socket $PWD/KiCadD/run/kicadd.sock path/to/board.kicad_pcb
```

`--socket` takes a plain path (the server adds `ipc://` itself).

## Python client (requirement 2)

```sh
python3 -m venv KiCadD/.venv
KiCadD/.venv/bin/pip install "protobuf>=4.21,<5" pynng
protoc --python_out=KiCadD/client/kicadd_client/_pb --proto_path=kicad/api/proto \
       $(cd kicad/api/proto && find . -name '*.proto' | sed 's|^\./||')
protoc --python_out=KiCadD/client/kicadd_client/_pb --proto_path=KiCadD/proto kicadd/board.proto
KiCadD/.venv/bin/python -I KiCadD/client/bench.py $PWD/KiCadD/run/kicadd.sock
```

Note: upstream item commands (`GetItems`, …) need `header.document.board_filename` set to the
board's file name, otherwise the handler answers `AS_UNHANDLED`.

## Phase-1 baseline (2026-11-05)

Board: `demos/jetson-agx-thor-baseboard` — 81 MB, 1125 footprints, 22 151 tracks,
3235 vias, 125 zones, 1388 nets, 10 Cu layers. Release build, 16 cores, Python client RTT.

| Operation | p50 | ≈ LLM tokens |
|---|---|---|
| Board load → resident | 1.7 s (once) | — |
| RSS resident | ~1.0 GB | — |
| Ping (before wake-up fix) | 10 100 µs | — |
| Ping (after) | 49 µs | — |
| upstream `GetItems` all footprints | 456 ms | 29.7 M |
| `kicadd QueryRadius` 5 mm (64 of 268) | 1.7 ms | 2.5 k |
| `kicadd QueryNet` (55 items) | 0.32 ms | 2.2 k |
| `kicadd GetBoardSummary` | 3.8 ms | <100 |
| `kicadd MoveDelta` (1 fp, undoable commit) | 37 ms | <50 |

## Incremental evaluation (2026-11-05)

### KiCadD commands

| Command | What |
|---|---|
| `MoveDelta` | Δ move/rotate, one undoable commit; returns positions, `unconnected`, **local DRC** |
| `CheckLocalDrc` | Local DRC for given refs without moving |
| `VerifyConnectivity` | Diff incremental connectivity vs a from-scratch rebuild (diagnostic) |
| `GetStateDigest` | Hash of nets / ratsnest / teardrops for A/B tests (diagnostic) |
| `SetRatsnestDeferral` | Enable/disable lazy ratsnest; `min_pads` threshold (default: defer nets with > 50 pads) |
| `FlushRatsnest` | Rebuild deferred nets now; returns the complete unconnected count |

`MoveDelta.unconnected` excludes deferred nets; `MoveDelta.ratsnest_pending` lists them
(e.g. `GND`, `+3V3`, `+1V8`). Anything that reads ratsnest, including full DRC, the upstream
API and `GetStateDigest`, triggers the rebuild automatically. Saving needs no flush, because
the ratsnest isn't stored in the file.

Local DRC (`src/pcb/kicadd_local_drc.cpp`) mirrors the upstream copper-clearance provider's
pad path and track path, the hole-clearance rules, net ties, and courtyard overlap. All rules
come from the board's `DRC_ENGINE`. Not covered: zone fills (stale until refill), silk, edge,
text, track-to-track (an unmoved track pair can't change).

### Results (Jetson AGX Thor board, Release build)

| | before | after |
|---|---|---|
| `MoveDelta` 1 footprint (U78), commit p50 | 35.5 ms | **12.7 ms** |
| random 1–3 fp moves, commit p50 (seed 5) | 45.7 ms | **18.1 ms** |
| local DRC after a move | — (full DRC 4.7 s) | **~1.6 ms p50** |
| random moves on a fragmented board (seed 9: GND split into ~1000 pieces), commit p50 | 126.9 ms | **9.7 ms** (lazy ratsnest) |
| random moves seed 5 / 13, commit p50 | 44.1 / 45.8 ms | **9.2 / 9.1 ms** |
| `FlushRatsnest` of GND, +3V3, +1V8 | — | 7.2 ms |

The fragmented case is the realistic one for an agent: during initial placement on an
unrouted board every pad is its own cluster, and GND (1266 pads here) dominates the MST.

### Verification (`client/verify_incremental.py`, `client/ab_equivalence.py`)

- Local DRC vs upstream full DRC, scoped to moved footprints: **exact match on 8 seeds**
  (405/405 violations, 0 false positives, 0 misses); seed 42 re-checked on the final build.
- A/B, same random move sequence on an optimised and an upstream-mode server, comparing
  net assignments, per-net ratsnest, teardrop zones and unconnected count:
  final build: Jetson 2 seeds × 200 moves and RoyalBlue54L (263 teardrops) 4 seeds × 120
  moves **identical at every checkpoint**. Seed 3 on RoyalBlue diverges in some runs (below).
  With lazy ratsnest (digest reads trigger the auto-flush): Jetson seeds 5/9/13 identical;
  RoyalBlue seeds 2/5/6 identical, seed 4 identical in 4 of 5 runs (the same nondeterminism).
- **Upstream nondeterminism found:** on RoyalBlue54L, two servers that both run *unmodified
  upstream* logic diverge at the same move in 2 of 3 runs (net + teardrop state). The same
  binary/config/input also gives different outcomes from run to run. The one optimised-vs-upstream
  divergence (seed 3 at move ~110) falls in this class: both sides flip between 196 and 197
  teardrops across runs. Root cause not investigated yet (thread scheduling or pointer-ordered
  containers in propagation/teardrop code). Worth reporting upstream.
- Also pre-existing upstream: commit-time net propagation can differ from a fresh
  `BuildConnectivity()` (`VerifyConnectivity` shows it, identically with or without KiCadD).

### Known next targets

- Large-net ratsnest is deferred, not incremental: a flush still rebuilds the whole net
  (GND ≈ 7 ms here). Incremental per-net MST would also make the flush cheap.
- The mid-commit flush path (a reader running inside `Push` before the ratsnest step) is
  handled by design but not exercised by the current tests.
- `SearchClusters` still does an O(items) participation pass; `updateJumperPads` is board-wide.
- `QueryRadius` / local DRC candidate search are linear scans (fine at ~1–2 ms here).
- MCP endpoint, change-push channel for the web view, LCSC integration (Phases 2–4).

## Phase 2: MCP (2026-11-05)

`kicadd-mcp` (`build/kicadd-mcp`) is a standalone C++ MCP server. It is an IPC client of the
daemon, so it shares the daemon's semantics and needs no further changes to upstream. No
Python or Node runtime is needed.

| Tool | |
|---|---|
| `board_summary` | counts, layers, outline |
| `query_area` | items near `ref` or `x,y` within `radius` (mm), nearest first; `kinds`, `limit` |
| `query_net` | pads / tracks / vias / zones of one net |
| `move_footprints` | `refs`, `dx`, `dy` (mm), `rotate` (deg); returns positions, unconnected count, local DRC |
| `check_drc` | local DRC for `refs` without moving |
| `flush_ratsnest` | rebuild deferred large nets, full unconnected count |
| `open_board` / `save_board` | load another board / write the in-memory board to disk |

Output is compact text, e.g. a move that creates violations (~266 tokens for 14 violations):

```
C216 @152.5,107.5 r180
unconnected=0 (excl. deferred nets: GND +1V8; call flush_ratsnest for the full count)
drc: 14
SHORT C216.2[+1V8] x C356.2[+3V3] F.Cu need0.1 have0 @152,107.328
COURTYARD C216 x C356 F.Courtyard @152,107.5
...
```

Protocol: JSON-RPC 2.0, handshake versions 2025-11-25 / 2025-06-18 / 2025-03-26 / 2024-11-05.
Clients that probe the 2026 `server/discover` get -32601 and fall back to `initialize`.
Streamable HTTP: POST → JSON response, notification → 202, GET → 405 (the server never
pushes messages), `Mcp-Session-Id` is set on initialize.
Security defaults: binds 127.0.0.1; any other bind address is refused without `--token`;
`Origin` must be localhost or `--allow-origin`; optional `Authorization: Bearer`.

### Run and connect

```sh
# on the server
KiCadD/client/kicadd_up.sh /path/board.kicad_pcb 8765        # daemon + MCP on 127.0.0.1:8765

# from your machine: tunnel, then register with Claude Code
ssh -N -L 8765:127.0.0.1:8765 user@server
claude mcp add --transport http kicadd http://127.0.0.1:8765/mcp

# or stdio over ssh (no tunnel, no port; the daemon must already be running)
claude mcp add kicadd -- ssh user@server /path/build/kicadd-mcp \
    --socket /path/KiCadD/run/kicadd.sock --stdio
```

With a token: `KICADD_MCP_TOKEN=... kicadd_up.sh ...` and
`claude mcp add --transport http kicadd URL --header "Authorization: Bearer ..."`.

### Verification (`client/mcp_e2e.py`)

All 38 checks pass, using the official MCP Python SDK (2.3.0) both over HTTP (`Client`,
`mode=auto`: discover probe, then fallback) and over stdio (`ClientSession`), plus raw-HTTP
edge cases (version negotiation, 202, 405, 404, 403 for a foreign Origin, 401/200 with a
token, malformed JSON → -32700, refusing a non-loopback bind without a token). A move costs
**~12–14 ms end-to-end** through MCP on the Jetson board.

### Known limits

- One board per daemon; tool calls are serialised (one shared board).
- No MCP resources/prompts, no server-initiated notifications (Phase 3 adds a push channel
  for the web view).
- `query_area` ranks by distance, then by size. Large modules whose body contains the centre
  still have distance 0 and come early; filter with `kinds` if needed.

## Phase 3: remote 2D view (2026-11-05)

`kicadd-mcp --http` also serves a browser view on the same port: `GET /` is the page, and
`/ws` is a WebSocket. One SSH tunnel carries MCP, the view and edits.

```
browser ──ws──┐                                      ┌─ GetRenderSnapshot (new view)
browser ──ws──┤  kicadd-mcp: io thread (Beast async) │
agent ──/mcp──┤              worker thread ──IPC─────┼─ GetChanges(since) every 50 ms
              │                                      └─ MoveDelta (browser edits)
              └──────────────────────── kicad-cli api-server: RENDER_FEED (BOARD_LISTENER)
```

- **Daemon** (`src/pcb/kicadd_render.*`, no upstream changes): a `BOARD_LISTENER` journals
  every changed or removed top-level item (pad → footprint) under a revision counter.
  `GetChanges(since)` returns the current state of changed items plus removed ids, or `resync`
  when the journal no longer covers `since` (or after `open_board` restarts revisions).
- **Hub** (`mcp/web.*`): a single worker thread does all IPC (snapshots, browser moves,
  polling), and it polls immediately after any browser or MCP edit. Deltas fan out to every
  browser. Clients apply a delta when `since <= rev < delta.rev` and ask for `resync` on a gap.
- **View** (`web/index.html`, ~28 KB, no dependencies): F.Cu/B.Cu/inner tracks and arcs,
  pads (circle/rect/oval/roundrect/polygon), vias, courtyards, zone outlines, references,
  layer toggles. Drag a footprint (0.05 mm snap, Alt = free), `R`/`Shift+R` rotate, arrow keys
  nudge. The local DRC result is listed and marked on the board. Edits by others (the agent)
  flash and are logged as remote. Optimistic local move; the server's delta is authoritative.
- Security: the page is static; `/ws` checks Origin (localhost, same-origin, or
  `--allow-origin`) and the token (`?token=` since browsers can't set WebSocket headers).
  `--no-web` disables both routes.

### Opening the view remotely

This dev container has no sshd; it runs code-server (port 8443) and VS Code Remote servers.
kicadd-mcp stays on 127.0.0.1:8765. Reach it in one of these ways:

- **VS Code Remote-SSH / Tunnel**: in the **Ports** panel, forward `8765`, then open
  `http://localhost:8765/`. The MCP endpoint is `http://localhost:8765/mcp`.
- **code-server in the browser**: open `https://<code-server>/proxy/8765/`. The page
  derives its WebSocket URL from its own location, so the path prefix works. code-server
  passes the Host header through, so the same-origin check accepts it (tested: 101 for an
  upgrade from the code-server origin).
- **Plain SSH to the Docker host**: bind kicadd-mcp to the container IP with a token
  (`KICADD_MCP_HOST=172.18.0.2 KICADD_MCP_TOKEN=... client/kicadd_up.sh BOARD`), then
  `ssh -N -L 8765:172.18.0.2:8765 user@docker-host` and open `http://localhost:8765/?token=...`.

`Bad local forwarding specification` from ssh means the `-L` argument itself could not be
parsed. This usually happens when a full-width colon or a non-breaking space comes along
with a copied command; type it by hand.

### Numbers (Jetson board, headless Chromium)

| | |
|---|---|
| snapshot | 2.9 MB JSON, ~110–150 ms server side, page live in ~220–300 ms |
| delta for one moved footprint | ~2 KB |
| browser A drags → browser B sees it | **15 ms** |
| agent `move_footprints` via MCP → browsers see it | **14–30 ms** |
| UI round trip of a drop (incl. local DRC) | 37 ms |
| render cache: cold build + draw / after one footprint changes | 27 ms / **2.4 ms** |

The render cache keeps one `Path2D` per footprint and merges them with `addPath`. Building a
single `Path2D` of all 4.8k pads took ~217 ms (superlinear) and stalled the UI on every edit.

### Verification

- `client/web_e2e.py`: two WebSocket clients plus an MCP agent covering snapshot, move → ack +
  DRC, fan-out to the other browser, agent edit pushed to both, rotation propagation,
  unknown ref, malformed frame, resync, 403 for a foreign Origin, 404 on other paths. All pass.
- `client/browser_check.py` (Playwright + headless Chromium in the venv): real mouse drag with
  grid snap, DRC panel, agent edit flashing in the view, keyboard rotate, no console errors,
  plus screenshots. All pass.

### Known limits / next

- No ratsnest lines in the view yet. Routing ("拉線") and track editing are not implemented;
  only footprint move/rotate.
- Zones are drawn as outlines, not fills (fills can be MBs and go stale until refill).
- Polling at 50 ms (≤ 50 ms extra latency for external IPC clients; in-process MCP and
  browser edits trigger an immediate poll). An NNG pub socket would make it push-based.
- Concurrent edits are last-writer-wins per move. The daemon serialises them; there is no
  locking or "someone else is dragging this" indicator yet.

## License

GPL-3.0-or-later (same as KiCad). Konnect (AGPL-3.0-only) is used as design reference only;
do not copy its code into this tree.
