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
| `client/` | Python IPC client, benchmark, verification + A/B scripts, `run_server.sh`, `desktop_e2e.py` (M2) |

### Upstream patch (~2600 lines incl. context, 53 files; see the M1–M7 sections and the 2026-11-11 fixes for the newest parts)

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
pad path and track path, the hole-clearance rules, net ties, and courtyard overlap, plus the
edge-clearance provider (copper vs Edge.Cuts / Margin, copper entirely outside the board). All
rules come from the board's `DRC_ENGINE`. Not covered: zone fills (stale until refill), silk,
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

Topology of this setup: KiCadD runs inside a Docker container (`dev-server`, 172.18.0.2) that
has no sshd. The container runs code-server on `[::]:8443`. VS Code Tunnel runs on the
**Docker host**, not in the container, so forwarding `localhost:8765` in the Ports panel
reaches the host's loopback, where nothing is listening ("無法轉送 localhost:8765").

- **Via code-server's proxy (no new exposure)**: in VS Code's Ports panel, forward
  `172.18.0.2:8443` (the container's code-server, already listening on all interfaces),
  then open `http://localhost:<forwarded port>/proxy/8765/`. code-server proxies to
  127.0.0.1:8765 inside the container. The page computes its WebSocket URL relative to its
  location, and code-server keeps the Host header, so the same-origin check passes.
- **Expose kicadd-mcp itself** (your decision; it opens a port on the Docker network):
  `KICADD_MCP_HOST=0.0.0.0 client/kicadd_up.sh BOARD` generates a token in `run/token` and
  prints `http://172.18.0.2:8765/?token=...`. Forward `172.18.0.2:8765` in the Ports panel.
  Remote peers need the token; loopback peers (agents in the container, the code-server
  proxy) are trusted via `--trust-loopback`. Untested so far with a real non-loopback peer.
- **Plain SSH** to the Docker host works the same way as the Ports panel:
  `ssh -N -L 8765:172.18.0.2:8443 user@docker-host` → `http://localhost:8765/proxy/8765/`.

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

## PCB editing safety and display (2026-11-06)

| Feature | Daemon / MCP | Web view |
|---|---|---|
| **Save** | `save_board` (upstream SaveDocument) | Save button, `Ctrl+S`, unsaved marker `Save •` |
| **Undo / Redo** | KiCadD history (before/after footprint states, 200 deep); `undo` / `redo` tools | buttons with the edit's name, `Ctrl+Z` / `Ctrl+Y` / `Ctrl+Shift+Z`; state synced across browsers and agent |
| **Ratsnest** | `GetRatsnest {flush}` (no-flush reader added upstream) | layer toggle; follows a dragged footprint; deferred large nets fill in 400 ms after edits stop |
| **Copper fill** | `GetZoneFills`; `refill_zones` tool | layer toggle (default on), fetched after the snapshot, per-zone refresh; Refill zones button |
| **Full DRC** | `run_drc` tool (upstream DRC task + readable handles, filters) | Run full DRC: summary, list, markers, click to zoom |

Undo covers KiCadD edits (footprint move/rotate from MCP or the web), not edits made with
other API commands, and not zone refills. The history resets when another board is opened.
Headless KiCad commits keep no undo list of their own.

Verification: `client/history_e2e.py` (19 checks, including save and reload from disk),
`browser_check.py` (Undo/Redo buttons and shortcuts), `mcp_e2e.py` (14 tools). `run_drc`
reports exactly upstream's non-excluded markers (Jetson: 317 = 317).

## Layout closure: flip, add, delete, edge DRC (2026-11-06)

| Feature | Daemon / MCP | Web view |
|---|---|---|
| **Flip** | `MoveDelta.flip`; `move_footprints {flip}` — upstream `FOOTPRINT::Flip` around the footprint's own anchor, left-right: padstacks, silk, fab, courtyard, mask/paste swap sides, geometry mirrored | `F` on the selection |
| **Delete** | `DeleteFootprints`; `delete_footprints` — one commit, `BOARD::Remove` takes the pads out of connectivity; the removed footprint is owned by the history (headless commits keep no undo list), detached: no parent, nets kept by name | `Del` / `Backspace` |
| **Add** | `AddFootprint`; `add_footprint {footprint: "Lib:Name" \| like: "C12", ref: "C?", x, y, rotate, back, value, nets: {pad: net}}` | `A` / ＋ Add: at the cursor, `Lib:Name` or a ref to copy |
| **Edge DRC** | local DRC: `DT_EDGE_CLEARANCE`, `DT_COPPER_OUTSIDE_BOARD`, in every move/flip/add result | listed with the local check |

Footprints for `add_footprint` come from the project/global footprint library table, then
`KICADD_FOOTPRINT_DIRS` (colon separated directories of `Nickname.pretty/`), then
`/usr/share/kicad/footprints` (Debian package `kicad-footprints`), then a footprint already on
the board with the same lib_id. Nets that don't exist are created and reported. `ref` ending
in `?` takes the next free number. Undo/redo of adds and deletes restore the same KIIDs and pad
nets, so earlier history entries (moves) still apply.

Edge DRC follows `DRC_TEST_PROVIDER_EDGE_CLEARANCE`: Edge.Cuts strokes at width 0, rectangles
and polygons split into sides, one report per item, castellated and edge-connector pads
exempt. A headless board has no cached outline until something builds it; local DRC builds it
once per outline generation. Cost: +0.1–0.8 ms per footprint; the first check after the
outline changes also triangulates it (~16 ms on Jetson).

Verification: `client/layout_e2e.py` (58 checks) and `client/browser_layout.py` (F / Del / A / Ctrl+Z in Chromium). Flip mirrors pads and courtyard exactly,
round-trips and undoes to identical geometry, and the saved file has the part on B.Cu with
B.SilkS / B.Fab / B.CrtYd. After every delete / add / undo / redo `VerifyConnectivity`
(incremental vs full rebuild) is empty. 24 footprints pushed over / off the Jetson edge:
local edge DRC reports exactly the pads full DRC reports (41 = 41); unedited RoyalBlue54L
board: 28 = 28. `verify_incremental.py` part B (12 random moves, all local DRC types incl. edge)
matches full DRC exactly (seeds 7 / 3: 47 = 47, 79 = 79); run part B on a fresh board, since
after part A's 60 random moves full DRC hits its per-type error limits (199 / 499).

## Silkscreen / Fab, multi-select, faster local DRC (2026-11-06)

**Silkscreen and Fab in the 2D view.** The daemon flattens each footprint's silk / fab
graphics and visible text (KiCad's own `GetEffectiveShape`, so text is KiCad's stroke font)
into polylines and polygons (`render.Graphic`), joining consecutive segments; board-level
artwork comes too. The snapshot leaves them out (`GetRenderSnapshot.skip_art`): the page
fetches each layer the first time it is shown (`GetArt` / `{"t":"art"}`), like copper fills.
Edited footprints carry their graphics in the delta. Layers F.SilkS, B.SilkS (on by default),
F.Fab, B.Fab (off: Fab is dense, e.g. Antmicro parts carry "License"/"Author" text).
Jetson: silk ~7k graphics, fab ~89k / 600k points (fetched in ~0.5 s); merging them into the
per-footprint path caches adds ~0.4 ms to a rebuild.

**Multi-select.** Shift/Ctrl+click toggles parts; dragging on empty board draws a box (left to
right: enclosed parts, right to left: touched parts). Dragging any selected part moves all of
them; R / F / Del / arrows act on the selection; several parts rotate and flip as a group
around their common centre (`MoveDelta.group_pivot`, MCP `move_footprints {pivot: "group"}`),
one undo step. Hit testing uses the courtyard, so a mechanical outline (no pads, no
courtyard, e.g. an M.2 card) is only picked at its edge and the board inside stays selectable.

**Local DRC on large parts.** The moved footprint's pads go into a grid; candidate tracks come
from the connectivity R-tree (`CN_LIST::QueryArea`, a small upstream helper) with its cached
boxes; no LSET allocation per pair; rules are evaluated only for shapes within the worst
CLEARANCE / HOLE_CLEARANCE of any rule. `KICADD_DRC_PROFILE=1` prints phase timings.

| part | pads | before | after |
|---|---|---|---|
| J1 | 699 | 183 ms | 12.5 ms |
| J18 | 564 | 51 ms | 6.2 ms |
| U21 | 101 | 5.7 ms | 3.2 ms |
| U78 | 24 | 1.75 ms | 0.6 ms |
| C100 | 2 | 1.25 ms | 0.2 ms |

Results unchanged: local == full DRC for J1 (238), J1+J18+U21+U30 (510), J18+J1 (263) and
random parts (47, 79) (`KICADD_TEST_REFS=J1 verify_incremental.py SOCK 0 SEED B`).

Verification: `client/browser_multiselect.py` (selection, group drag / rotate / flip / delete,
undo, silk follows the parts), plus every earlier suite.

## M1: trimmed build (2026-11-06)

Three switches in the upstream patch (top-level `CMakeLists.txt`), all `OFF` by default so a
normal build is unchanged:

| switch | effect |
|---|---|
| `KICADD_NO_OCC` | no OpenCASCADE: `pcbnew/exporters/step` + `u3d` and the STEP export dialog are not built; `kicad-cli pcb export 3d` (STEP, STEPZ, BREP, XAO, GLB, PLY, STL, U3D, 3D PDF) reports "not available"; the 3D model importer loads VRML / X3D only; the footprint panel's "export extruded body" is disabled |
| `KICADD_NO_SPICE` | no ngspice: eeschema builds without the simulator. `eeschema/CMakeLists.txt` filters out the ngspice backend, simulator frame, plot/tab widgets, tuners, probe/sim dialogs and `SIMULATOR_CONTROL`; the call sites (frame factory, probe/tune tools, *Inspect → Simulator*, toolbar button) are `#ifdef`'d out; `SIMULATOR::CreateInstance()` is a static stub returning `nullptr` (IBIS model generation then reports "no simulator"). The SPICE **models** stay (symbol `Sim.*` fields, the model dialog, SPICE netlist export) since they are schematic data, not simulation |
| `KICADD_NO_PYTHON` | no Python interpreter at configure time or run time: Python API plugins are skipped, no interpreter lookup, no virtualenvs; executable plugins still load. SWIG bindings are already gone upstream |

```sh
cmake -S kicad -B build-lean -G Ninja -DCMAKE_BUILD_TYPE=Release -DKICADD_DIR=$PWD/KiCadD \
      -DKICADD_NO_OCC=ON -DKICADD_NO_SPICE=ON -DKICADD_NO_PYTHON=ON -DKICAD_BUILD_QA_TESTS=OFF
ninja -C build-lean kicad-cli pcbnew_kiface eeschema_kiface eeschema kicadd-mcp kicad_3d_import
KICADD_BUILD=$PWD/build-lean KiCadD/client/kicadd_up.sh board.kicad_pcb 8765
```

Measured (Release, Jetson board, `kicad-cli api-server`):

| | full | lean |
|---|---|---|
| OCC libraries linked by `_pcbnew.kiface` | 31 | **0** |
| `_pcbnew.kiface` dependency closure (ldd) | 191 libs, 180 MB | **148 libs, 122 MB** |
| `_pcbnew.kiface` itself | 71.4 MB | 70.5 MB |
| shared libraries mapped at run time | 191 | **148** |
| RSS with the board loaded | 828 MB | 810 MB |
| ready (process start → board answers) | 1.50 s | 1.48 s |
| `kicad-cli` process start | ~63 ms | ~63 ms |

What this shows: the dependency footprint (what must be shipped and installed) drops by a
third, but start-up time doesn't move: the process starts in ~60 ms either way and the 1.5 s
is parsing an 81 MB board. Binary size is pcbnew's own code, not its dependencies.

Verified on the lean build: `layout_e2e`, `history_e2e`, `web_e2e`, `mcp_e2e`,
`browser_multiselect`, `browser_layout`, local == full DRC (J1+J18+U21+U30 510, random 79).
All symbols resolve at load (`dlopen` with `RTLD_NOW`) for both `_pcbnew.kiface` and
`_eeschema.kiface`; no object in the lean tree has an `ngspice/` header dependency
(`ninja -t deps`), and the lean `_eeschema.kiface` has 0 `SIMULATOR_FRAME` symbols (full: 151).

`client/smoke.py` (standard library only; Linux build tree or Windows install) checks the build
end to end: `kicad-cli version`, DRC on `demos/pic_programmer`, a SPICE netlist from
`demos/simulation/rectifier` (eeschema + SPICE models without ngspice), and the API server
answering `board_summary` through `kicadd-mcp --stdio`. Passes on the full and the lean build.

## M2: desktop co-pilot (2026-11-09)

The agent edits the board **open in the desktop editor** (pcbnew), live, next to the user. Same
board, same undo stack, same canvas; the user keeps routing (push-and-shove, diff pairs, length
tuning) while the agent places.

```
Claude Desktop / Cursor / Claude Code
   │  MCP over stdio
kicadd-mcp --stdio            (no --socket: finds the editor by itself)
   │  protobuf over NNG: <temp>/kicad/api.sock (Windows: a named pipe of that name)
pcbnew (KiCadD build) ── PCB_EDIT_FRAME ── KICADD_HANDLER_PCB + upstream API_HANDLER_PCB
```

MCP client config (Claude Desktop `claude_desktop_config.json`, Cursor `mcp.json`):

```json
{ "mcpServers": { "kicadd": { "command": "/path/to/kicadd-mcp", "args": ["--stdio"] } } }
```

Claude Code: `claude mcp add kicadd -- /path/to/kicadd-mcp --stdio`. The order does not
matter: with no editor open a tool call fails within 5 s ("is KiCadD running?"), and the next
call after pcbnew opens a board goes through (NNG redials in the background).

What changed:

- **Handler in the editor** (`pcb_edit_frame.*`, under `#ifdef KICADD`): `PCB_EDIT_FRAME`
  registers `KICADD_HANDLER_PCB` on `CreatePcbFrameContext( this )`, next to upstream's handler.
  Commits use the frame's tool manager, so they land on the editor's undo stack, update the
  view and the ratsnest. The canvas is repainted after each agent commit (an API request is no
  tool event, nothing else repaints it), and the status bar says what the agent did
  ("Agent: move U3 +0.00,-10.00 mm"); the same text is the Undo menu entry.
- **API server on by default**: the editor starts it even with the preference off (it is the
  co-pilot's transport); `KICADD_NO_API=1` opts out. Socket: KiCad's standard
  `<temp>/kicad/api.sock`, which is also `kicadd-mcp`'s default now.
- **Busy**: edits (move/flip/rotate, add, delete, undo/redo, ratsnest flush, connectivity check)
  are refused with `AS_BUSY` while the user is in an interactive operation (router, drag, a
  point edit, a modal dialog; upstream's `CanAcceptApiCommands`). kicadd-mcp retries for 10 s,
  then tells the agent the user is busy and nothing was applied. Reads still answer.
- **Board swap**: File → Open replaces (deletes) the board; `PCB_EDIT_FRAME::SetBoard` calls
  `DetachBoard()` first (drops the change feed's listener and the agent history), the feed
  re-attaches on the next request.
- **Agent undo/redo never overwrites the user**: a footprint changed since the agent's edit (by
  the user, or anyone) is left alone and reported (`did not undo "move U3 …": U3 (changed
  since; left as is)`). The user's Ctrl+Z / Ctrl+Y work on agent edits like on their own.
- **Ownership of deleted footprints**: with an editor, `BOARD_COMMIT::Push` hands a removed
  footprint to the undo list (Ctrl+Z puts that object back), so the agent history keeps a
  copy; headless it keeps the original as before.
- kicadd-mcp waits at most 5 s for a connection (the reply timeout stays 120 s, for full DRC).
- Commit messages are the history labels ("Agent: …") instead of "KiCadD MoveDelta".
- The STEP/GLB export menu entry is hidden without OpenCASCADE (left over from M1).

Verification: `client/desktop_e2e.py --build build-lean --demos kicad/demos --work DIR` runs the
real editor under Xvfb (xdotool for the user's keys, screenshots in DIR), 13 checks: API server
up with no preference set, agent move → canvas + status bar, user Ctrl+Z, agent undo after a user
change left alone, edits refused while the router is active and applied after Escape, agent
delete → user Ctrl+Z / Ctrl+Y → agent undo, clean exit (no double free). The headless suites
(`layout_e2e`, `history_e2e`, `web_e2e`, `mcp_e2e`, `smoke.py`) still pass.

Run on real Windows by the user (2026-11-09): `board_summary` answers from the editor. KiCadD
requests are served while the **PCB Editor** window is open; the `kicad.exe` project window alone
answers only the common API (kicadd-mcp says so). kicadd-mcp finds the socket with wx's temp-dir
rules (TMPDIR, TMP, TEMP, then GetTempPath).

Exit crash fixed later (M4a work): pcbnew aborted on exit in about one run in three
("corrupted double-linked list"), with or without the agent. valgrind: upstream
`TEXT_EVAL_VCS` keeps its context path in a `thread_local wxString`, destroyed before the static
`PGM_BASE` whose `~SETTINGS_MANAGER` then assigns to it. The patch keeps it behind a never-freed
pointer; 8/8 desktop runs exit 0 since (`desktop_e2e.py --upto N --wrap "valgrind ..."` bisects).

Not yet: a co-pilot panel in the editor (agent activity log, pause switch); for now the status
bar and the Undo menu show it.

## M4a: LCSC part number → footprint (2026-11-09)

An LCSC number is enough to place a part: `add_footprint lcsc="C8734"` (or `lcsc_fetch` to look
first) fetches the part, converts its footprint to KiCad, saves it to the project library and
places it, as one undoable edit. Headless and in the desktop editor alike.

```
lcsc_fetch {"lcsc": "C8734"}
C8734 STM32F103C8T6: footprint LQFP-48_L7.0-W7.0-P0.50-LS9.0-BL
  saved as LCSC:LQFP-48_L7.0-W7.0-P0.50-LS9.0-BL (<project>/lcsc.pretty/...kicad_mod); LCSC library added to the project fp-lib-table
  fetched, courtyard 10.55x10.55 mm, 48 pads (SMD)
  pads (number@x,y mm): 1@-2.75,4.25 2@-2.25,4.25 ...
  Source: JLCEDA/EasyEDA Official Library (https://lceda.cn/, https://easyeda.com)
```

How (`src/lcsc/kicadd_lcsc.cpp`, handler `FetchLcsc` and `AddFootprint.lcsc`):

- **Source:** the JLCEDA/EasyEDA component API (`easyeda.com/api/products/<C>/components`), the
  data behind the LCSC catalogue, through KiCad's curl wrapper.
- **Conversion:** KiCad's own EasyEDA Standard importer (`pcbnew/pcb_io/easyeda`, GPL like
  KiCad); no third-party converter code (easyeda2kicad is AGPL). KiCadD then tidies the result:
  - solid regions on paste/mask (EasyEDA's stencil and mask openings) become filled shapes;
    pads that EasyEDA gives a −100 mm paste margin for that reason lose their paste layer
    instead (same output, no DRC "questionable padstack")
  - component-shape → F.Fab body outline, polarity mark → F.Fab, lead shapes dropped
  - courtyard = pads + body + 0.25 mm, on a 0.01 mm grid
  - no STEP model is referenced (no OpenCASCADE); the EasyEDA 3D model id stays in a field
  - fields: `LCSC` = C number (for JLC BOM/CPL), Datasheet; the description names the source
- **Orientation:** kept as EasyEDA draws it (e.g. LQFP pin 1 bottom-left, the "-BL" in the
  name), which is what JLC assembly rotations are based on.
- **Project library:** `<project>/lcsc.pretty`, nickname `LCSC`, added once to the project
  `fp-lib-table` as `${KIPRJMOD}/lcsc.pretty`, so the project carries its parts and
  Update PCB from Schematic finds `LCSC:...`.
- **Cache:** `KICADD_LCSC_CACHE` or `<KiCad user cache>/kicadd/lcsc/<C>.json` (the raw answer).
  A cached part never touches the network unless `refresh`; when the network fails the cache is
  used with a note; `KICADD_LCSC_OFFLINE=1` is cache-only.
- **Rate limit:** easyeda.com's CDN answers HTTP 403 for a while after a burst (~30 requests in
  a few minutes from one address). Requests are spaced ≥ 1.5 s and the error says what happened.
- The fetch runs on the editor's main thread (about a second, once per part).

Licensing: the EasyEDA data carries a notice asking for its source to be named (done in every
footprint's description and the library description) and grants commercial use "when used in
JLCEDA/EasyEDA". Fetching on demand for your own boards is the same use easyeda2kicad and others
make; redistributing a bundle of converted parts would be a different question, so KiCadD ships
none and CI does not fetch.

Verification: `client/lcsc_e2e.py` (13 parts: 0603/0805 R/C, tantalum, SOD-123F, SOT-23, SOT-223,
LQFP-48, QFN-56 + EP, two USB-C with slots and polygon pads, 1x40 header) compares every pad of
the written `.kicad_mod` with the raw EasyEDA PAD/HOLE records parsed on its own (position, size,
drill, SMD/THT within 2 µm), checks courtyard, attribution, no STEP, nothing left on User layers,
places each part and undoes it, then the fp-lib-table row, the cache, unknown/malformed numbers.
All pass. Full DRC on a board with all 13: only the C2765186 USB-C's locating holes sit
0.17–0.21 mm from its outer pads (as designed by the part's land pattern). `desktop_e2e.py` with
`KICADD_LCSC_CACHE` set adds an editor step (agent places C25804, user Ctrl+Z). `smoke.py` (CI)
checks the offline path only.

## M4b: LCSC symbols (2026-11-10)

`lcsc_fetch` (and `add_footprint lcsc=`) now saves the part's schematic symbol too, so the
schematic side has it when it gets there (M5) and a human can place it from the symbol chooser:

```
  symbol LCSC:STM32F103C8T6 (<project>/lcsc.kicad_sym), reference U, 48 pins; LCSC library added to the project sym-lib-table
```

- **Where it runs:** symbols are eeschema code and `lcsc_fetch` runs in pcbnew (the server, or
  the PCB Editor). The patch adds a kiface export, `KIFACE_KICADD_LCSC_SYMBOL`
  (`include/kiface_ids.h`, `kiface_services.h`, `eeschema/eeschema.cpp`), the same mechanism
  KiCad uses for the schematic netlist; pcbnew loads `_eeschema.kiface` through its KIWAY on the
  first fetch. Both sides exchange a small JSON request/answer. If eeschema can't be loaded, the
  footprint is still saved and the note says why the symbol wasn't.
- **Conversion:** `src/sch/kicadd_lcsc_sym.cpp` feeds the cached answer to KiCad's EasyEDA
  Standard symbol parser (`eeschema/sch_io/easyeda`). Multi-unit parts (EasyEDA `subparts`)
  become one symbol with a unit per subpart.
- **Fields:** Footprint = the footprint's `LCSC:<package>`; `LCSC` = C number; Datasheet;
  Description with the source named and a `Source` field; Manufacturer / Manufacturer Part /
  Supplier Part from EasyEDA; Reference prefix without "?" ("U", "R", "USBC" ...); Reference
  above the body and Value below (the parser left both at the origin).
- **Library:** `<project>/lcsc.kicad_sym`, nickname `LCSC`, added once to the project
  `sym-lib-table`. Symbol name = part title; a different part with the same title gets
  `_<C number>` appended instead of overwriting it.
- **Check:** the answer lists symbol pins without a pad and pads without a pin (a WARNING for
  the former).

Verification: `lcsc_e2e.py` now also checks, for all 13 parts, that the symbol's pin numbers
equal the EasyEDA symbol's P records and the footprint's numbered pads, the Footprint field, the
LCSC field, the attribution, the reference prefix, and the sym-lib-table row (252 checks, PASS).
`desktop_e2e.py` step 5b saves a symbol from inside the PCB Editor. Multi-unit conversion was
checked with a synthetic two-unit entry only (none of the 13 parts, nor LM358DR2G, has
subparts on EasyEDA).

## M4c (part 1): stock and price in `lcsc_fetch` (2026-11-10)

The EasyEDA component answer already carries the catalogue data, so `lcsc_fetch` reports it at
no extra request:

```
  LCSC stock 10693, US$2.398 each (min 1); JLC assembly: yes (Extended Part) -- as of 2026-11-09 (cached; refresh=true for today's)
```

Stock, unit price at the minimum quantity, JLC assembly availability (`SMT` + `jlcOnSale`) and
Basic/Extended (the symbol's "JLCPCB Part Class"). The date is when the answer was fetched:
cached parts show their cache date, `refresh` gets today's. Some parts (e.g. C2040 RP2040) come
without LCSC stock data and show `?`. `lcsc_e2e.py` checks the reported stock and JLC flag
against the raw answer.

Not yet: keyword search (`lcsc_search`): to use the official JLCPCB Components API with each
user's own key (applied for; its auth/endpoints are only documented in the developer portal);
3D models (EasyEDA OBJ → VRML).

## M4d: "Add LCSC Part" in the editors (2026-11-10)

The human gets the same thing the agent has, from the Place menu:

- **PCB Editor → Place → Add LCSC Part...**: enter a C number; the part is fetched (cache
  first), footprint and symbol go to the project's LCSC libraries, and the footprint is handed
  to KiCad's own footprint placement tool: it follows the cursor (rotate/flip keys work), a click
  places it, Esc takes it back, Ctrl+Z undoes it. The reference is the symbol's prefix + the
  next free number (U7). The infobar shows stock, price, JLC Basic/Extended and the data date.
  (`src/pcb/kicadd_lcsc_tool.cpp`, the same insert-then-place hand-over as the footprint
  browser's "Insert Footprint".)
- **Schematic Editor → Place → Add LCSC Symbol...**: same prompt; the symbol is placed with
  KiCad's symbol placement (annotated, Footprint field = `LCSC:<package>`), so Update PCB from
  Schematic later finds the footprint. Fetching is board code, so pcbnew exports
  `KIFACE_KICADD_LCSC_FETCH` and eeschema calls it, the reverse of M4b
  (`src/sch/kicadd_lcsc_sch_tool.cpp`).
- Every save now reloads the LCSC library entry in the library manager, so an editor that
  already has `lcsc.pretty` / `lcsc.kicad_sym` open sees the new part at once.

Verification: `desktop_e2e.py` step 5c drives the PCB Editor menu under Xvfb (Place > Add LCSC
Part..., C8734, click, Ctrl+Z) and checks through the agent that U7 appeared and went again
(PASS). The schematic side was checked by hand under Xvfb: C5446 placed as U7 with
`LCSC:SOT-23-3_L2.9-W1.6-P1.90-LS2.8-BR`, saved, `lib_id "LCSC:XC6206P332MR"` in the file; a
second part in the same session (C25804 → R22) came from the refreshed library.

## M5a: the agent draws the schematic (2026-11-10)

The headless server now holds the whole project: `kicad-cli api-server` given a board (or a
`.kicad_pro`) also opens the `.kicad_sch` next to it, and `KICADD_HANDLER_SCH` (eeschema
kiface, next to KiCad's own schematic handler) gives the agent eight tools:

| tool | |
|---|---|
| `sch_summary` | parts (ref, value, symbol, footprint, position), nets, unconnected pins; `with_pins` |
| `sch_add_part` | `lib_id` ("Device:R") or `lcsc` ("C6186", fetched like `lcsc_fetch`); `ref` "R?" numbered; value / footprint override; x/y or the next free spot; rotation; unit |
| `sch_part` | one part, every pin: number, name, type, side, position, net |
| `sch_connect` | `{"nets": {"GND": ["U1.1", "C1.2"], "+3V3": ["U1.VOUT"]}}`; "NC" = no-connect flag |
| `sch_disconnect` | pins off their nets |
| `sch_netlist` | nets with "REF.PIN" members, unconnected pins |
| `sch_delete` | parts, with their stubs and labels |
| `sch_save` | |

- **Connections without routing:** each pin gets a 2.54 mm wire stub away from the body and a
  net label at its end (text pointing outward; global labels with `global`). The agent names
  every net and never draws wires; the result is an ordinary, readable KiCad schematic.
  A pin already on another net is refused (disconnect first); the same net again is a no-op;
  a stub end that would land on something else is pushed further out.
- **Placement:** parts without x/y go to the first free slot on the sheet (scanning rows, body +
  12.7 mm / 7.62 mm clearance for stubs and labels); explicit positions snap to the 1.27 mm grid.
- **Libraries:** headless, nothing loads symbol libraries in the background, so the one asked
  for is loaded on the spot (the LCSC one reloaded after pcbnew has just written a part).
- Net names are KiCad's (`/+3V3` for a label "+3V3" on the root sheet), the same names the board
  will get. A pin alone is reported as unconnected, not as its `unconnected-(...)` net.

Verification: `client/m5_e2e.py` builds a blank project (empty schematic, 60 x 40 mm board),
starts its own server, places 4 LCSC parts and a `Device:R` (value, LCSC footprint, rotation),
connects a 3.3 V regulator circuit by net name and checks the netlist pin by pin, refusals, NC,
disconnect, delete (labels go with the part), then saves, restarts and finds the same netlist
(PASS). Rendered with `kicad-cli sch export svg`: stubs and labels as intended.

## M5b + M5c: schematic to placed board, headless (2026-11-10)

```
sch_add_part / sch_connect ...   -> the schematic
sync_pcb                         -> 5 footprint(s) added: U1 R1 R2 C1 C2; board now 5 footprints, 5 nets
auto_place                       -> placed 5 part(s); wire length (half-perimeter) 43.9 -> 25.04 mm
run_drc                          -> errors: unconnected_items 7   (nothing routed yet: the human's turn)
```

**`sync_pcb`** (Update PCB from Schematic): saves the schematic, gets its netlist from eeschema's
export (`KIFACE_NETLIST_SCHEMATIC`), and runs KiCad's own netlist updater exactly as
`ImportNetlist` does (match by symbol UUID): new footprints from each symbol's Footprint field,
nets on the pads, changed values. `dry_run`, `delete_extra`, `update_footprints`. The report
keeps only what isn't routine. New footprints land near the origin, outside the board, until
placed.

**`auto_place`** (`src/pcb/kicadd_autoplace.cpp`, a plain model with no BOARD in it):

1. *Global:* parts off the board are pulled to the weighted centre of the pins they connect to
   (clique weights 1/(k-1); nets on more than 30 % of the parts, typically GND and the main
   supply, are left out so everything doesn't collapse into one point). Small passives are also
   pulled to the nearest pin of the biggest part sharing a non-ground net with them, so
   decoupling caps go to their supply pin and pull-ups to their signal. Parts already on the
   board keep their place as the target, so `all` improves a layout instead of starting over.
2. *Legal:* big parts first, each to the free spot nearest its target (square rings on a grid of
   0.1 to 0.2 mm), in its rotation or, failing that, the other three. Footprint areas
   (courtyard + silkscreen) stay `spacing` (0.25 mm) apart and `edge` (0.5 mm) inside the
   Edge.Cuts outline; pads stay clear of copper drawings/text, tracks, vias and footprint
   keep-outs (silk may go over board text). Parts on no net (mounting holes, logos) go last, to
   the corners and then the middles of the edges.
3. *Refine:* each part tries the legal spot nearest its connection centre in all four
   rotations, then same-size parts try swapping; kept only if the wire length goes down.
   Unanchored groups are centred on the board.
4. Four starts (spiral phase, order by size or by connectivity) run in parallel threads; the
   one placing the most parts with the shortest wire length wins. The result is one undoable
   edit in the agent's history.

Locked parts, parts on the back and parts not at a right angle are kept where they are and
named; `fixed` keeps more (place connectors first, then the rest).

Measured (`client/autoplace_bench.py`, KiCad's pic_programmer demo, tracks stripped, all 62
movable parts moved off the board): **10 s, half-perimeter wire length 1584 mm against the
human layout's 1489 mm (+6 %)**, every part placed, full DRC: no courtyard, clearance, hole or
silkscreen errors (only the 125 unrouted connections). The first versions taught what matters:
silkscreen in the footprint area (20 silk_clearance errors otherwise), copper obstacles checked
against pads only (otherwise the 40-pin ZIF socket found no room under the board's copper
text), mounting holes to the corners, and several starts, since one start can land 15 % worse.

Verification: `m5_e2e.py` continues from the drawn schematic: `sync_pcb` (dry run changes
nothing; 5 footprints with every net pad by pad; a second sync adds nothing), `auto_place` (all
inside the outline, wire length down, DRC only unrouted, both caps within 8 mm of the regulator,
undo takes it back), save. PASS, together with mcp_e2e (25 tools), layout, history, lcsc,
desktop and smoke.

Not yet (at the time): the schematic tools in the desktop Schematic Editor (see M5d), hierarchical sheets, placing on the back side,
keep-together groups / placement rules from the agent, routing.

### auto_place: functional blocks, two sides, speed (2026-11-10)

**Blocks.** `sch_add_part` takes `block` ("LDO", "USB"), saved as a hidden `Block` field that
`sync_pcb` carries to the footprint. `auto_place` keeps each block together: blocks from
`groups` (`{"LDO": ["U2", "C3"]}`), else the footprints' Block field; `groups: "sheets"` uses
the schematic sheets, `false` none. Each part is also pulled to the centre of the rest of its
block, in all three steps, with `group_weight` (default 3; 1 is like one two-pin net). The
answer says how mixed the blocks are: parts whose nearest neighbour (courtyard gap, same side)
belongs to another block, before and after.

Measured on four KiCad demos with the sheets as blocks (all front parts moved off the board,
connectors, mounting holes, switches and logos kept where the human put them on cm5_minima;
wire length is half-perimeter, mm; "mixed" as above):

| board (front parts) | human | no blocks | regions | soft pull, weight 1 | weight 3 |
|---|---|---|---|---|---|
| pic_programmer (62) | 1489, mixed 6 | 1385-1731, mixed 11-12 | 1993, mixed 19 | 1985-2106, mixed 8-10 | 1824-1879, mixed 2-5 |
| kit-dev-coldfire (146) | 7931, mixed 18 | 7091-7496, mixed 65-69 | 10087-10522, mixed 37-49 | 7237-8096, mixed 37-48 | 8300-8719, mixed 21-37 |
| cm5_minima (54) | 2720, mixed 32 | 2976 | 2976, mixed 43 | 2979, mixed 48 | |
| video (86) | 30912, mixed 26 | 58264, mixed 57 | 52463, mixed 23 | 58005, mixed 56 | |

What that says: keeping blocks together costs wire length (here 10-35 %), it does not buy it.
Without blocks the placer is already at or below the human layout's wire length on two boards
(coldfire 9 % shorter, pic_programmer 7 % shorter on one of three seeds); the remaining gap is
mostly how a start lands (the same board spreads 1385-1731 over seeds), not mixed blocks. The
sheets are not the blocks a layout keeps apart either: pic_programmer's second sheet is its 7
sockets, wired to everything. So blocks are opt-in (the agent says which parts belong together)
and soft. The first version gave each block an exclusive region (slicing floorplan, regions
placed by their connections): the most expensive and, on pic_programmer, even more mixed; it
was dropped.

**Two sides.** Front and back no longer share room: a part's courtyard only conflicts with the
same side's, through-hole pads count on both (no body over pins on the other side), and holes
keep the hole clearance from every pad (the board's worst `hole_clearance` rule and the pads'
own overrides: cm5_minima's CM5 connector holes ask for 1.5-1.7 mm). Before, the 103 back-side
parts of video and the CM5 connectors filled the front.

**Speed.** The legality test looked at every placed part; a 4 mm bucket grid of the placed
parts makes it look at the neighbours only (same results, 6-8x faster: coldfire 97 s -> 13 s,
cm5_minima > 120 s -> 3 s), and refinement no longer searches the whole board for a spot
farther away than the part already is. Eight starts instead of four (16 threads here).

**Bottom side.** Opt-in, since two-sided assembly costs a second stencil and setup:
`back_decaps: true` lets decoupling caps (two pads, one on ground, tied to an IC pin) go to the
bottom, under their pin, where the IC's body doesn't block them; `back: ["C*", "R12"]` lets any
listed SMD part go there (through-hole parts stay on their side and are named). Such a part
carries its shape on both sides (taken from a flipped copy, with the same flip that applies
it); legalisation and refinement try both sides, and the bottom has to win by 0.5 mm. Copper
obstacles count on their own side (vias and inner layers for through-hole pads only). Parts
already on the bottom can now be placed too (they stay there). `measure_only` reports the
numbers for the parts as they are, moving nothing (the bench measures the human layout so).

| board | front only | `back_decaps` | `back: ["C*", "R*"]` |
|---|---|---|---|
| kit-dev-coldfire | 7237 mm, passives 26.5 mm from their IC pin (human 35.6) | 7027 mm, 19.5 mm, 33 caps on the bottom | 6100 mm, 11.2 mm, 79 parts on the bottom |
| cm5_minima | 2976 mm, 13.6 mm (human 24.4) | 2954 mm, 10.8 mm, 11 on the bottom | |
| video | 57647 mm, 78.1 mm | 57662 mm, 75.2 mm, 5 on the bottom | |

Every run: no DRC errors the human layout doesn't already have. pic_programmer is through-hole
almost throughout: nothing to flip. `m5_e2e.py`: the regulator's two caps go under it (pad to
pin 3.41 -> 1.59 mm), U1 stays on top, DRC only unrouted, undo puts them back on top.

Bench: `autoplace_bench.py --sheets --group-weight W --fix J,H --no-groups --back-decaps --back C*` (the offset off the
board now follows the board's width; only DRC errors the human layout doesn't already have
count). `m5_e2e.py` gives U1 C1 C2 block LDO and R1 R2 block LED: saved as Block fields,
`auto_place` reports 2 blocks from the Block field. PASS, with mcp_e2e and smoke.

## M5d: the schematic tools in the desktop Schematic Editor (2026-11-10)

The `sch_*` tools now work in the open Schematic Editor too, the way the board tools work in the
PCB Editor: the agent's edits are the editor's own commits (`Agent: add R22`, `Agent: connect 2
pins` on the undo list, the canvas repainted, the status bar naming the edit), so the user's
Ctrl+Z / Ctrl+Y take them back and forth. While the user is in an interactive tool (drawing a
wire, moving, a dialog) edits are refused as busy and nothing changes. The Schematic Editor
registers `KICADD_HANDLER_SCH` on its frame (`CreateSchFrameContext`) and starts the API server
if nothing has; it deregisters on close and in its destructor.

In the KiCad project manager both editors live in one process, on one API server, so one
kicadd-mcp reaches both. `sync_pcb` there asks the open Schematic Editor for its netlist
(`MAIL_SCH_GET_NETLIST`, as Update PCB from Schematic does): unsaved edits come along and the
user's schematic file is not written. Headless it saves first and netlists the file, as before;
with the PCB Editor alone it uses the saved file. `sch_save` in the editor saves through the
editor (title and modified flags).

`client/desktop_sch_e2e.py` (Xvfb, the real project manager on pic_programmer): Ctrl+E, the agent
reads the editor's 69 parts, adds a resistor and connects it to VPP and a new net (the title
shows the schematic modified); the user's Ctrl+Z takes the connection back, a second the part;
with the wire tool active the agent's add is refused and nothing changes, after Escape it
applies; Ctrl+P, `sync_pcb` puts the new part on the PCB Editor's board on the right nets "from
the open Schematic Editor" while the `.kicad_sch` on disk stays byte for byte the same; Ctrl+Q,
don't save, exit code 0. PASS, together with m5_e2e (headless) and desktop_e2e (PCB Editor).

## M5e: hierarchical sheets (2026-11-10)

The agent builds multi-sheet designs the way it builds one sheet:

```
sch_add_sheet {"name": "Power"}                                  -> Power.kicad_sch, page 2
sch_add_part  {"lib_id": "Device:R", "sheet": "Power"}           -> R1 on /Power/
sch_connect   {"nets": {"VOUT": ["R1.2", "C1.1"], "VIN": ["R1.1"]},
               "ports": ["VOUT:output", "VIN:input"]}            -> hierarchical labels inside,
                                                                    sheet pins on Power's symbol
sch_connect   {"nets": {"+5V": ["Power.VOUT", "LED.VCC"]}}       -> on the root: sheet pins are
                                                                    wired like part pins
```

* `sch_add_sheet`: name (unique among its siblings), file (default `<name>.kicad_sch` next to
  the root; a new file only: an existing one, or one already in the schematic, is refused),
  parent (name or path), position (default the next free spot), size. One commit; the editor's
  hierarchy navigator follows.
* `sch_add_part sheet` puts the part on that sheet (annotated across the whole hierarchy).
* `sch_connect ports`: those nets become ports of the pins' sheet: a hierarchical label inside
  and, if missing, a sheet pin of the same name and shape on the sheet's symbol (inputs on the
  left, the rest on the right, 2.54 mm apart; the sheet grows when a side is full). A port on a
  root pin is refused. Other nets on a sub-sheet stay local (`/LED/LED_A`).
* A sheet pin is `SHEET.PIN` wherever a part pin is accepted (`sch_connect`, `sch_disconnect`).
* `sch_summary` lists the sheets: path, file, position, parts, and each pin with its direction,
  side and the net it has on the parent ("not connected on /" until wired there). Parts say
  which sheet they are on.
* `sch_delete` with a sheet's name removes it with everything on it (its file stays on disk).

One trap on the way: `SCH_COMMIT` turns adding an item whose parent item is a sheet into
modifying that sheet, so a sheet created with its parent sheet as parent item was never added.
It is created without one; appending it to the screen sets the parent.

Verification: `client/hier_e2e.py` (headless): two sheets with R/C and LED/R, ports, the root
wiring them with a cap: one `/+5V` across three sheets (R1.2 C1.1 R2.1), one `/GND`, `/VIN`
from the root into Power, `/LED/LED_A` local; disconnecting `Power.VOUT` splits the net,
reconnecting joins it; saved (Power.kicad_sch, LED.kicad_sch), restarted, same netlist;
`sync_pcb` puts all five parts on the board with the cross-sheet nets; `auto_place groups
"sheets"` sees three blocks; deleting LED leaves Power and three parts. KiCad's ERC on the saved
design: 0 violations. `desktop_sch_e2e.py` adds a sheet with a part in the open Schematic Editor
(the navigator shows "Agent (page 3)"), and the user's two Ctrl+Z take both away. PASS, with
m5_e2e, mcp_e2e (26 tools) and smoke.

Not supported: several instances of one sheet file (a channel used twice): adding a part to
such a sheet is noted ("the part is on every copy"), annotation per instance is not handled.

## M5f: placement rules from the agent (2026-11-10)

`auto_place rules` says what a designer would say before placing; refs may be patterns:

```
"rules": {
  "edge":     {"J1": "left", "P1": "left", "USB*": "any"},  // courtyard at that board edge
  "region":   {"U3": [73.6, 40.6, 129.7, 139.8]},          // courtyard inside, mm
  "rotation": {"J1": 270},                                 // only this rotation
  "near":     [{"refs": ["U4", "C4"], "max": 8}]           // pad centres within 8 mm
}
```

* edge, region and rotation are hard: a spot breaking them is not legal (a part that can't meet
  them is reported as having no room). Parts with an edge or region go first, so connectors
  frame the rest; their target is moved onto their edge (the nearest one for "any") or into
  their region before the free spot is searched for.
* near is kept by a strong pull (every mm over the limit costs like 10 mm of wire) and by trying
  the spot next to the partners in legalisation and refinement; what can't be met comes back as
  "rules not met: P2 U5: 11.6 mm apart (max 2.0)".
* Bad rules (a side that isn't one, an unknown ref, 45 degrees) are refused before anything moves.

`client/rules_e2e.py` (pic_programmer, tracks stripped, all front parts off the board): J1 and
P1 on the left edge (0.6 and 0.8 mm from it) at 270, U3 inside the left 35 % of the board, U4
and C4 7.5 mm apart, read back from the saved board's courtyards and pads; DRC: nothing the
human layout doesn't have; wire length 1620 mm (1712 without rules). P2 and U5 within 2 mm is
reported as not met. Without rules nothing changes (pic_programmer 1711.69 mm, coldfire 7236.76
mm, as before). PASS, with m5_e2e and mcp_e2e.

Not yet: connectors that overhang the edge (the DB9 in the human layout sticks 8.6 mm out): an
edge rule keeps the courtyard inside, flush with the edge.

## M7: desktop declutter (2026-11-10)

What a KiCadD user should not meet any more:

* **No first-run "KiCad Setup" wizard.** It was modal and appeared before anything else (the
  agent waited behind it). KiCadD applies the wizard's own defaults silently: settings as
  shipped, library tables (blank when no templates are installed: parts come per project from
  LCSC), API server on, no data collection.
* **No checks against KiCad's servers.** KiCad's update check and the package manager's
  background check are off by default and never run in the project manager, whatever an
  installed KiCad left in the shared config directory.
* **No Plugin and Content Manager** (launcher button and Tools menu entry): parts come from LCSC,
  not KiCad's package repositories.
* **No "External Plugins" menus** in the PCB and Schematic Editors: there is no Python; the
  agent talks to the API directly. (STEP export and the simulator were already hidden in M1.)
* **Help menu**: KiCad's "Get Involved" and "Donate" are gone (KiCadD is a fork); "Report Bug"
  opens a KiCadD GitHub issue.

`KICADD` is defined only on the files concerned (`set_property( SOURCE ... )`), so the rest of
common/ and the project manager keep their build cache: 36 build steps. Verified on a fresh config
directory under Xvfb: the project manager opens straight away (no wizard), `check_for_updates`
is false, no PCM launcher, the trimmed Help menu; desktop_e2e and desktop_sch_e2e pass.

## M3: Windows CI (2026-11-06)

`.github/workflows/windows.yml` builds KiCadD for Windows x64 on GitHub-hosted runners:

1. `ci/fetch_upstream.sh` checks out upstream at `patches/UPSTREAM_BASE` (shallow fetch by
   SHA, ~16 s) and applies `patches/*.patch`. Also usable locally for a clean tree.
2. vcpkg at the manifest's baseline, KiCad's own `vcpkg.json`. The patch makes OpenCASCADE,
   ngspice and Python **default features** (`occ`, `spice`, `python`), so an upstream-style
   build is unchanged and the lean build passes `--x-no-default-features`. It also adds
   `boost-beast` for `kicadd-mcp`.
3. `ci/vcpkg-triplets/x64-windows.cmake`: upstream's triplet, release only.
4. MSVC + Ninja, `Release`, `KICADD_NO_OCC/NO_SPICE/NO_PYTHON=ON`. On Windows `KICADD_NO_OCC`
   also drops the native 3D model previewer (an OpenCASCADE viewer).
5. `cmake --install` into a portable folder with the vcpkg DLLs, `client/smoke.py` against it,
   uploaded as the `kicadd-windows-x64` artifact (14 days).

Two jobs because each GitHub job is capped at 6 h: `deps` builds the vcpkg packages into a
binary cache (`actions/cache`, saved even when the job fails part way, so a re-run resumes),
`build` restores it and builds KiCadD. The first run builds every dependency from source and
is slow; later runs only rebuild KiCadD.

**Releases.** Pushing a `v*` tag runs the same build; a third job, `release`, zips the
artifact and publishes a GitHub release with `ci/release-notes.md` (upstream base, commit,
CI run and SHA-256 filled in). A tag with a `-` (`v0.1.0-alpha.2`) is a pre-release:

    git tag v0.1.0-alpha.2 && git push origin v0.1.0-alpha.2

The install prefix is set at configure time (`CMAKE_INSTALL_PREFIX`), not with
`cmake --install --prefix`: on Windows `KICAD_DATA` is an absolute path baked at configure
time, and `--prefix` would leave `share/kicad` (icons, templates, demos) out of the zip.

**Build modes (2026-11-09).** The compile step was 53–71 min on every push. Now:

| | check (push to main, PR, manual) | release (`v*` tag) |
|---|---|---|
| runs when | the push touches `src/ mcp/ proto/ web/ cmake/ patches/ ci/`, `client/smoke.py` or the workflow | always |
| compiler cache | sccache (local folder, one `actions/cache` entry, older ones deleted) | none: clean build |
| precompiled headers | off (sccache can't cache MSVC `/Yu`) | on |
| debug info | none (`KICADD_MSVC_DEBUG_FORMAT=`) | `/Zi`; link PDBs as `kicadd-windows-x64-pdb.zip` on the release |

- The patch adds `KICADD_MSVC_DEBUG_FORMAT` because KiCad's CMake otherwise forces `/Zi`, which
  sccache can't cache.
- Not the whole `build/` folder: checkout resets file times and Ninja would rebuild everything;
  sccache keys on content.
- One sccache entry (≤ 4 GB; 206 MB measured, no debug info) next to the 445 MB vcpkg
  cache stays well inside the 10 GB repo quota; pruning older entries keeps LRU eviction from
  ever dropping the vcpkg cache (rebuilding it takes hours).
- Measured: cold cache 1 h 41 min (no precompiled headers), warm cache 15 min for the build
  step and about 18 min for the whole job (2434 of 2436 compiles from the cache, the rest is
  linking). A change to a widely included header or to the patch recompiles more.
- sccache doesn't know MSVC's `/external:env:INCLUDE` and counts it as a second input file,
  which made every C++ compile uncacheable; under sccache the patch swaps it for
  `/external:anglebrackets` (`cmake/Warnings.cmake`).
- Trade-off: a docs-only push no longer proves Windows still builds; anything touching code
  still does.
- `vcpkg.exe` is fetched with the job token (`gh release download`); anonymous downloads timed
  out (HTTP 504) during GitHub's 2026-11-09 incident.

## auto_place time limit (2026-11-11)

From the Windows trial (TODO 5): `auto_place` runs on the editor's thread, so a long run makes
the editor stop responding. Profiling the routed pic_programmer (`all`, tracks kept) showed
legalizing takes 0.9 s and improving (refine) the rest, about 2 s per pass.

- **Time limit.** Improving stops after `time_limit` seconds (default 10, `-1` no limit), shared
  by all starts. Every part keeps a legal spot throughout, so stopping anywhere leaves a valid
  placement; the result says `stopped improving at the time limit (10 s, 3 pass(es))`.
  Placing every part (legalize) is never cut short.
- **Duplicate starts stop early.** With the parts already on the board the starts differ only in
  order, and 8 starts gave just 2 layouts. A start whose layout after legalizing matches
  another's stops there (same layout, same result), so 6 of the 8 threads stop early.
- **Converged passes stop.** Improving ends when a pass gains less than 0.1 % of the cost.
- **Usage note.** Tracks and vias are obstacles. When parts find no room on a routed board, the
  result says so and suggests deleting the routing first or placing right after `sync_pcb`.
  The tool description says this too.

| routed pic_programmer, `all` | before | after |
|---|---|---|
| headless, 16 cores | 14.3 s | 9.4 s, same result (1753.1 mm) |
| headless, 4 cores (`taskset`) | not measured | 10.0 s (time limit, 4 passes), same 1753.1 mm |
| desktop pcbnew (Xvfb), 16 cores | 14.9 s | 9.5 s |
| desktop pcbnew (Xvfb), 4 cores | not measured | 10.0 s (time limit) |

`autoplace_bench.py` (tracks stripped): same wire length in all four configurations (plain,
`--sheets`, `--fix J,H`, `--back-decaps --back C*`), 7.9/12.7/8.7/7.7 s → 7.2/10.0/6.7/7.8 s.
`m5_e2e`, `rules_e2e` and `hier_e2e` pass.

## auto_place in the background (2026-11-11)

TODO 5a: the placement no longer runs on the editor's thread.

- `AutoPlace.background` builds the model from the board on the main thread, then hands the
  placement to a worker thread and answers at once (`running`, `progress`: "placing, 30/62
  parts", "improving, 2 pass(es) done"). The worker touches only its own copy of the parts.
- `AutoPlaceStatus` reports progress; once the run is done, that call applies the result on the
  main thread as one undoable edit, as before. `cancel` stops the run and moves nothing.
- While it runs, the agent's own edits are refused as busy (`auto_place is running`).
  kicadd-mcp retries busy for up to 10 s, so in practice they wait and land after the
  placement. If the board changes anyway (the user edits it, an upstream API call refills
  zones), the result is discarded: `the board changed while auto_place was running; nothing
  was moved`. The check uses the change feed's revision and the board pointer, then finds the
  footprints by KIID.
- If the run finishes while the user is in an interactive tool, the result waits until the
  tool is done.
- The MCP `auto_place` tool always runs it in the background and polls every 200 ms, so for
  the agent it is still one call. It gives up and cancels after `time_limit` + 300 s.
  Without `background`, the request runs synchronously as before (Python client, tests).
- Closing or replacing the board (`DetachBoard`) and the handler's destructor cancel and join
  a running placement.

Verified: `desktop_autoplace_e2e.py` (desktop pcbnew under Xvfb, routed pic_programmer, `all`):
`board_summary` answers in 1 ms while the placement runs (it used to wait about 15 s); an agent
move made meanwhile lands after the placement (undo order: the move, then `auto-place 62
parts`); `refill_zones` during a run gives "board changed" and nothing moves.
`autoplace_bg_e2e.py` (headless, Python client): progress while running, edits refused,
cancel moves nothing and leaves no job, the synchronous call still works. `m5_e2e`,
`hier_e2e`, `rules_e2e` pass, and `autoplace_bench.py` wire lengths are unchanged.

## auto_place on a routed board (2026-11-11)

TODO 5(e) and what turned up with it. On the routed pic_programmer (`all`, tracks kept), DRC
after auto_place had 563 errors, among them 18 `shorting_items`, 10 `overlapping_footprints`
and 48 `pth_in_courtyard`. Three causes:

- **Tracks as one bounding box.** A diagonal track's box covers far more than the track. Track
  obstacles are now a row of small boxes along the segment (arcs: along their polyline), each
  grown by half the width plus the track's own net-class clearance (POWER asked for 0.28 mm,
  the default for 0.25 mm). Vias also use their own clearance.
- **The part's side as one box around all pads.** A track running between the rows of an IC's
  pins kept the IC out. For tracks and vias (`OBST_PADS`) each pad is now checked once that box
  meets the track. Copper text and graphics still keep the whole pad area clear: allowing them
  under a part made the stripped-board benchmark 15 % worse, and they don't belong under a part
  body anyway.
- **A part with no room was no obstacle.** P3 (the 40-pin ZIF socket, whose courtyard is
  already over the board edge in the human layout) stays where it is, but the other parts
  were placed over its pads: the shorts. A part that finds no room and is (partly) on the
  board is now kept as a fixed part at its place, and the result says so.

| routed pic_programmer, `all` | before | after |
|---|---|---|
| wire length | 1753.1 mm | 1653.1 mm |
| DRC after `refill_zones` | 563 errors (shorts, overlaps, holes, silk, clearance...) | only `unconnected_items` 122 |

`autoplace_bench.py` (tracks stripped): the same wire lengths as before in all four
configurations. `m5_e2e`, `hier_e2e`, `rules_e2e`, `smoke`, `desktop_autoplace_e2e` and
`autoplace_bg_e2e` pass.

**TODO 5(c), memory growth (Windows 425 → 1432 MB): not reproduced on Linux.** RSS sampled
during `auto_place all` (no time limit) on the routed pic_programmer stayed flat: headless
`kicad-cli` 251 → 257–260 MB, desktop pcbnew 602 → 607 MB. That holds for the code before the
time limit too (peak 266 MB headless). The suspected `pullPoint` reallocation doesn't exist:
`pullPoint` only sums, and the hot path (`free`, `legalAt`, `nearestLegal`) allocates nothing.
The only per-call container is `localCost`'s small `nets` vector. If the growth shows up again
on Windows with alpha.4, a heap snapshot (or the working set at the start and end of a run)
would tell where it goes.

## DRC: library check without KiCad's libraries (2026-11-11)

TODO 1 from the Windows trial: `run_drc` on pic_programmer gave `0 errors, 63 warnings`, all
`lib_footprint_issues`. There were two causes:

- **KiCad's standard libraries are not part of KiCadD.** A footprint from one of them (no table
  row, or a row pointing into `${KICADn_FOOTPRINT_DIR}`) can't be compared. In KiCadD builds the
  library parity check now skips these (`drc_test_provider_library_parity.cpp`, `#ifdef KICADD`).
- **Headless DRC never loaded the libraries.** The API's DRC task did not load the footprint
  libraries first, as `kicad-cli pcb drc` does, so every library read as "not enabled",
  including the project's own `footprints`. The task now loads them first
  (`api_handler_pcb_tasks.cpp`, `#ifdef KICADD`); the editor has them loaded already.

Libraries that exist are still compared. After growing one pad of Q3 (`footprints:TO-92`) on a
copy of the board, DRC reports `lib_footprint_mismatch Q3 ... does not match copy in library
'footprints'`; the untouched board has 0 warnings, headless and in the desktop pcbnew.
A separate "library check skipped" line in `run_drc` (planned) was not added: the skipped
footprints are only the ones from libraries KiCadD doesn't ship.

## sch_netlist: one entry per pin, no virtual symbols, no-connect pins listed (2026-11-11)

TODO 2–4 from the Windows trial, all in `handleGetNetlist`:

- A pin shared by the units of a symbol is listed once. The supply pins of the 74HC125 came back
  from each of its four units: `U2.7` four times in `GND`.
- Symbols whose reference starts with `#` are skipped, even without the power flag
  (pic_programmer's `VPP` listed `#PWR122.1 #PWR123.1 #PWR134.1`).
- Pins marked no-connect (X) are listed in a new `no_connect` field (`SchNetlist`, field 3),
  shown as a `no-connect (N): ...` line. Before, they appeared nowhere, so neither the agent
  nor the user could tell them apart from pins the tool missed. pic_programmer: 77 such pins,
  among them `U4.3 U4.4`.

Release check, 2026-11-11 (`build-lean`): `m5_e2e`, `hier_e2e`, `rules_e2e`, `smoke`,
`desktop_e2e` (with the LCSC cache), `desktop_sch_e2e` and `mcp_e2e` (Jetson board) all pass.

## 待做清單 (TODO)

2026-11-10，`v0.1.0-alpha.2` 在 Windows 上試用後回報；以下已在 Linux 上用 `pic_programmer` 重現確認，**尚未實作**。

1. **（已完成 2026-11-11，見「DRC: library check without KiCad's libraries」）`lib_footprint_issues` 警告雜訊。** 重現：`run_drc` 回報 `0 errors, 63 warnings`，全是
   `The current configuration does not include the footprint library 'Capacitor_THT'`（以及
   `MountingHole` 等）。原因：KiCadD 不附 KiCad 官方封裝庫，demo 的 `fp-lib-table` 指向
   `${KICAD10_FOOTPRINT_DIR}/*.pretty`，找不到庫所以 DRC 無法比對。板子本身沒問題。
   方向：在無官方庫的 KiCadD 把這類警告預設設為忽略（或 `run_drc` 摘要中分開列出）。
   不要建議使用者「把標準庫加回來」，因為本版沒有那些庫。
2. **（已完成 2026-11-11）（低優先，純顯示）`sch_netlist` 重複腳位。** `GND` 裡 `U2.7`、`VCC` 裡 `U2.14` 各出現 4 次：
   74HC125 四個單元共用的電源腳，`handleGetNetlist`（`src/sch/kicadd_handler_sch.cpp`）對每個單元
   各呼叫 `symbol->GetPins( &sheet )`，共用腳（unit 0）每次都回傳。電路沒有錯，只是輸出變長；
   KiCad 自己的網路表匯出每支腳在每個網路只列一次。方向：以 `ref.pin` 去重。
3. **（已完成 2026-11-11）（低優先，純顯示）`sch_netlist` 列出虛擬電源符號。** `VPP` 裡出現 `#PWR122.1 #PWR123.1 #PWR134.1`：
   demo 的 `pic_programmer:VPP` 是後加的虛擬電源，符號沒有 `(power)` 旗標，`IsPower()` 為假。
   一般的 GND / VCC 電源符號有正確跳過。方向（可選）：另外跳過 `#` 開頭的參考編號。

4. **（已完成 2026-11-11）（低優先）`sch_netlist` 不顯示標了不接（X）的腳。** `handleGetNetlist` 對沒有網路的腳：
   有 no-connect 旗標就直接略過，沒有才列進 `unconnected`。結果是「刻意不接」的腳不出現在任何
   地方，Agent 和使用者分不出它和「工具沒看到」。`pic_programmer` 的 U4.3 / U4.4 就是這樣
   （電路圖有 X，PCB 有 2 個 `unconnected-` 網路）。方向：proto `SchNetlist` 加 `no_connect`
   清單，MCP 文字輸出多列一行。

5. **`auto_place` 在桌面版會讓編輯器「沒有回應」，期間所有 MCP 呼叫逾時。**
   **(a)(b)(d)(e) 已完成（2026-11-11，見「auto_place time limit」「auto_place in the background」
   「auto_place on a routed board」）。(c) 在 Linux 無法重現（見同節），請在 Windows 上用 alpha.4 再確認。** 2026-11-11 在卡住當下
   取得傾印檔（`kicad_16528_drc_hang.dmp`，用 `v0.1.0-alpha.2` 的 PDB 還原），**原因不是 DRC，
   也不是死結**：
   * 主執行緒停在 `KICADD_HANDLER_PCB::handleAutoPlace → kicadd::autoplace::Place`，等 8 條
     多起點執行緒（`std::thread`）；API 處理函式在 GUI 執行緒執行，所以整個編輯器被占住，
     之後的 `run_drc` 排在後面而逾時。
   * 8 條執行緒各累積約 29 秒 CPU，全在 `PLACER::refine → nearestLegal → legalAt → ruled → free`；
     CPU 時間持續增加，記憶體 425 → 1432 MB。是長時間運算，不是卡住不動。
   * Linux（16 核）同一操作：無頭 7.7–14.9 秒，桌面版 14.9 秒；`no room for: P3` 那顆放不進去的
     零件最花時間（把 P3 固定後減半）。核心數較少的 Windows 機器會慢數倍。
   方向：(a) 運算移出 GUI 執行緒，或至少可取消、有進度；(b) 放不進去的零件要有搜尋上限
   （`nearestLegal` 的範圍／時間預算），並在結果中提早回報；(c) 查記憶體成長（`pullPoint` 的
   `vector<VECTOR2>` 重新配置）；(d) 在已佈線的板子上，舊走線當障礙會讓放不進去的零件變多，
   使用說明要寫明（建議先移除走線，或只對剛 `sync_pcb` 的板子執行）。
   （先前懷疑 DRC 的 GUI 進度回報，已排除：Linux 桌面版 DRC 4 秒，563 個錯誤也不卡。）
   (e) 新發現：走線障礙物用的是整條走線的外框（`GetBoundingBox`），斜走線會擋住一大塊矩形，
   已佈線的板子上零件更難放進去。改成依走線形狀判斷，可能讓 P3 放得進去。

6. **Agent 做不到的事（2026-11-11 試用回報，已對照 MCP 工具清單確認）。** 依建議順序分三批。

   **第一批：電路圖編輯**
   * (a) `sch_move`：移動、旋轉已放置的零件。`sch_connect` 產生的短線和標籤要跟著零件移動，
     連線才不會斷。
   * (b) `sch_wire`：依座標點畫導線，交叉處自動放 junction。短線加標籤仍是預設做法。
   * (c) 電源符號：本版沒附 `power` 庫，在程式裡直接產生 GND / VCC / +3V3 / +5V 等常用電源符號
     （`sch_add_part lib_id="power:GND"`，或 `sch_connect` 指定電源網路時自動加上）。
   * (d) `run_erc`：在記憶體中的電路圖上跑 KiCad 的 ERC（`ERC_TESTER`），未存檔的內容也能查，
     做法比照 `run_drc`。`kicad-cli sch erc` 只讀檔案。
   * (e) `lib_register`：把專案裡的 `.kicad_sym` / `.pretty` 加進 `sym-lib-table` / `fp-lib-table`。

   **第二批：審查與製造輸出**
   * (f) `audit`：用規則檢查，輸出報告，判斷交給 Agent。
     - 去耦電容：IC 的電源腳，同網路上有沒有電容；在 PCB 上算出電容到腳的距離。
     - 電源：每個電源網路有沒有來源（穩壓器或連接器），有沒有只接一端的網路。
     - ESD：接到連接器的訊號網路上有沒有 TVS / ESD 元件。
   * (g) `export_fab`：一次輸出 Gerber、鑽孔、座標檔（CPL）和 BOM，用 JLCPCB 的格式。
     直接處理編輯器裡的板子，不用先存檔；這些匯出程式本版都有保留。

   **第三批：JLCPCB 與元件庫**
   * (h) JLCPCB 官方 API：使用者已申請，**審查中**。官方文件存放在
     `kicad_refactor/JLCPCB_API_DOC/`（網頁存檔）。要用到的部分：
     - Components：`Query Component Detail Data`（依 C 編號查詳情）；
       `Component information`（分頁取得整個公開庫）。**清單裡沒有依規格搜尋的介面**，
       依規格搜料可能要先把公開庫分頁下載成本機索引再搜。
     - PCB：`Upload gerber files` → `Get PCB pre-review information` → `Online quote`
       可做 PCB 報價，接在 (g) 後面。清單裡**沒有 SMT / PCBA 報價**，組裝費只能用 BOM 估算。
     - 下單（`Create order`）不做，或至少要使用者明確確認。
     - 開始前先讀 `API Request Signature`、`IP Whitelist`、`Configure API Key`；
       金鑰不能寫進專案或 log。
   * (i) `lib_create_symbol`：給腳位清單（號碼、名稱、類型、方向），產生方框符號。
   * (j) 參數化封裝：只做 SOIC / QFN / QFP / 0402–1206 / 排針等常見封裝；異形封裝仍由使用者在
     編輯器裡畫，立創有的零件優先用 `lcsc_fetch`。

保留不動：專案管理器的 Version Control（Git）面板。

## License

GPL-3.0-or-later (same as KiCad). Konnect (AGPL-3.0-only) is used as design reference only;
do not copy its code into this tree.
