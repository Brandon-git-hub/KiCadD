---
name: kicadd
description: How to work on a KiCad schematic or PCB through the kicadd MCP tools (KiCadD): a new project, reviewing a design, editing the schematic, sourcing LCSC/JLCPCB parts, simulation, PDN impedance and reflections, a new board's outline / layers / pours / fab rules, schematic-to-board, auto placement, routing (route, route_short, fanout), design rules, DRC/ERC and fabrication output. Use whenever the kicadd tools (board_summary, sch_summary, auto_place, ...) are available and the task touches a schematic, board, footprint, part or manufacturing file.
---

# Working with KiCadD (kicadd MCP tools)

KiCadD is a KiCad build with an agent API. The tools act on the design **in memory**: in the
desktop app that is the open Schematic / PCB Editor (the user sees every edit at once and can
Ctrl+Z it); headless it is a board loaded by `kicad-cli api-server`. The tool descriptions say
what each tool does; this skill says how to use them together.

## While routing: the short audit

`audit sections=["drc", "unrouted", "impedance"]` (any of the section names) answers only
those: a full DRC's counts by type, the unrouted connections per net, the impedance line.
Use it in the route / check loop instead of the whole audit.

## Tool descriptions

The descriptions in the tool list are one or two sentences, to keep every turn cheap. The full
text of each tool (arguments in detail, examples, defaults, formulas, limits) is in
[TOOLS.md](TOOLS.md) next to this file, and `tool_help {"name": "auto_place"}` returns it for one
tool. Read it before the first use of a tool with many options (audit, auto_place, simulate,
set_rules, zones, pdn, reflection), or when a call fails: a tool's first error also points there.

## Ground rules

1. **Look before you change.** Start with `board_summary` and/or `sch_summary`. Ask KiCad for
   connectivity (`sch_netlist`, `sch_part`, `query_net`, `query_area`) instead of reading
   `.kicad_sch` / `.kicad_pcb` files: labels, sheet pins, hidden power pins, multi-unit parts
   and no-connect flags are already resolved there.
2. **Never write the design files while the editor has them open.** The editor would overwrite
   them, or the two would disagree. Edit through the tools. Direct file edits are for KiCad
   closed (bulk text changes), checked afterwards with the tools or `kicad-cli`.
3. **Every edit is undoable, and the user's edits are safe.** PCB: `undo` / `redo`. Schematic:
   `sch_undo` / `sch_redo`. They take back only the agent's own edits and leave alone what the
   user changed since. The user can also Ctrl+Z any agent edit in the editor.
4. **"The editor is busy"** means the user is in a tool, a dialog or a progress window; the
   message says which. Do not hammer it: tell the user, or wait and try once more. kicadd-mcp
   already retries for 10 s.
5. **Save only when asked** (`save_board`, `sch_save`). Before saving, or to report what a
   session did, `board_diff` lists what changed since the last save. Saving writes this build's file format
   (KiCad nightly 10.99); KiCad 9/10 may not open the files afterwards. Say so the first time.
6. **Keep answers short.** Long lists are cut by default; pass `detail=true` only when you need
   every position or violation. For repeated or routine work use `batch` (one call, one line per
   step) instead of many single calls.
7. **Arguments are checked:** a name a tool does not have is an error that lists the ones it
   has; read it and call again rather than guessing.
8. **Units:** mm everywhere, Y grows downwards. The schematic snaps to the 1.27 mm grid. Angles
   in degrees, counter-clockwise.

## Review a design

1. `sch_summary` (parts, sheets, unconnected pins) and `board_summary`.
2. `audit`: decoupling caps measured on the board, supply sources, ESD on external connectors,
   one-pin nets; on the board, silkscreen text over pads and overlapping references, nets
   without a test point. Give what you know about the power: `currents {"VIN": 2}` (A) checks
   each net's narrowest track per layer against IPC-2221; `voltages {"HV": 48}` (V; names like
   `+12V` / `3V3` are read on their own, other nets count as 0 V) checks the spacing to every
   other net against IPC-2221B table 6-1 (refill zones first). Judge the findings (a socket for
   a programmed chip needs less decoupling than a switching regulator).
   The ESD check goes by connector names: wire-to-board connectors (JST SH / PH / GH, Molex
   PicoBlade, pin headers) are taken as on-board and not checked. When one takes an outside
   cable (a port at the board edge), check its TVS yourself.
   ICs are found by their pins' electrical types. LCSC symbols come with most pins
   "unspecified"; fetching types them from their names, conservatively (`lcsc_fetch` says so):
   GND / VSS / VCC / VDD / VIN power_in, a regulator's or switch's output and an IC's own
   regulator outputs (VD33_O, 3V3_OUT) power_out, connectors and everything else passive.
   A net fed only by a connector's VBUS then needs a PWR_FLAG for ERC (as in KiCad). Check
   the ICs' supply pins with `sch_part` when the audit header still says "0 IC(s)" (a
   symbol fetched before this, or names the rules did not know).
3. **Reference planes** (a routed board with planes; refill zones first). `audit` alone gives
   one line per section, measured and not judged, with the worst net: plane splits and the
   longest stretch with no plane under a track (`plane`); the farthest return via of the vias
   that change planes, the nearest on the other side, BGA fields, vias between different plane
   nets with the nearest capacitor (`return_via`). For rows per net (a diff pair is one row,
   `X_P/N`), worst first: `plane_limit 20`, `plane_nets ["*HDMI*"]` or `pairs_only true` (globs
   like `*_N` also catch active-low signals). You set the limits, by net glob in mm (an exact
   name beats a glob, a more literal glob a looser one); each rule then gets a count line,
   passing or not, the tightest first, and its worst nets / pairs:
   `return_via {"*PCIE*": 1.0, "*DDR*": 2, "*": 3}`, `two_sided ["*PCIE*"]` (a return via on each
   side, G-S-S-G), `bga_return_via_mm`, `plane_gap {"*USB*": 0.5}` (splits are always reported
   for the nets it matches). Choose them from the parts and the schematic, not only the names:
   - strict, 1.0 to 1.5 mm, a GND via on each side (G-S-S-G): PCIe Gen4 / Gen5, USB 3.2
     (10G / 20G), HDMI 2.1, MIPI CSI / DSI lanes, fast clocks;
   - standard, 2.0 mm: DDR4 / LPDDR4X address and control, PCIe Gen3, USB 3.0 (5G), gigabit
     Ethernet (RGMII / SGMII);
   - relaxed, 3.0 mm: a BGA's fanout when it has an even GND via array (the overview counts
     the GND vias and pads in the field), SPI, UART, GPIO and other slow control lines.
   Read the overview first, then call again with limits for the nets that matter; a split
   under a fast pair is worth fixing, a slow line over a gap seldom is. A track's planes are
   the ones `impedance` uses (by poured copper, across signal layers between; the header names
   such layers). A pour on a signal layer between a track and its plane is reported as "over
   local copper", never judged: the return runs on that pour there, so the line's impedance
   steps (see `audit impedance` with `z_nets`); for a fast net, move the pour or the track,
   or make the pour the plane.
4. **Diff pairs and lengths** (checks only: you never route or meander; the human tunes on
   the desktop with KiCad's tools, so say which net needs how much). `audit` alone gives one
   `diff_pair` line: routed pairs (`X+/-`, `X_P/N`), the worst skew and the most uncoupled
   track. Lengths are KiCad's net inspector's (tracks, via spans, pad-to-die). Rows per pair
   with `length_limit 20` or `length_nets ["*HDMI*"]` (which also lists those nets' lengths in
   `length`): both lengths, skew, the gap where coupled (per layer; against the net class's diff
   pair gap when a class of its own sets it, else each layer's median), the uncoupled track and
   its longest stretch, vias. Set the class first if you know the target (`set_rules`
   `diff_impedance` or `diff_pair_gap`). Limits by glob in mm, a count line per rule:
   `skew {"*PCIE*": 0.13}` (the row names the short member and what it needs), `uncoupled
   {"*": 5}`, `pair_gap {"*": 0.02}` (a tolerance; stretches where the pair runs into its pins
   or vias don't count), `length_match {"*DDR_DQ*": 0.5}` (each glob is a group, a pair counts
   as its mean; rows say what each short net needs against the longest), `max_length` /
   `min_length`. Take the numbers from the parts' layout guides. Typical starting points:
   - intra-pair skew about 0.13 mm (5 mil) for multi-gigabit pairs (PCIe, USB 3.x, HDMI, MIPI,
     SATA); USB 2.0 and 100BASE-TX tolerate around 1 mm;
   - a byte lane of DDR (DQ / DM against its DQS) within a few tenths of a mm, as the SoC's
     guide says; lanes of one link (HDMI, MIPI) matched to each other more loosely than within
     a pair;
   - uncoupled: keep breakouts short; a few mm over a whole pair is usual.
   Unrouted connections are flagged ("length so far"): judge lengths on a routed board.
5. **DC IR drop** (refill zones first). `audit` alone gives one `ir_drop` line: the supply
   rails solved as resistor networks (zone fills on a grid, tracks, via barrels, the stackup's
   copper) and the worst drop. Rows per rail with `ir_limit 10` or `ir_nets ["+3V3", "GND"]`
   (ground only when named): source -> loads, the drop range and the worst load with where it
   is, the loss split (tracks / planes / vias: what to widen), the busiest via, the most loaded
   track. Read the source and loads first: they are guessed (an output pin, else an inductor,
   a connector, a module; loads: the rail's ICs, the current split per pad). Fix them with
   `ir_source {"+3V3": "U1.5"}` and `ir_loads {"+3V3": "U5:0.8,U7"}` (A per part; parts without
   a number share the rest). Without `currents {"+3V3": 1.2}` drops are per A and the limits
   don't judge that rail. Limits: `ir_drop {glob: mV}`, `via_current {glob: A}`. Typical
   starting points: a few % of the rail (3.3 V: about 50-100 mV board drop; core rails under
   1 V: 1-2 %, from the SoC's guide); about 1 A per 0.3 mm via (0.5 A for 0.2 mm). Show a
   hotspot with `board_render marks=[[x, y, "label"]]` (it zooms to them all). The mesh is the
   zone fills (antipads cut out); big pours get a coarser one, said in the row: under a BGA pass
   `ir_mesh_mm 0.1` before trusting a close call. The model: copper at 20 C (at 85 C about 25%
   more drop), the rail alone (add the ground's drop with `ir_nets ["GND"]`), several source pads
   as one ideal 0 V node (parallel regulators share perfectly).
6. **Crosstalk pre-screen** (routed boards). `audit` alone gives one `crosstalk` line: signal
   net pairs whose tracks run side by side on one layer with an edge gap under 3H (H: the
   dielectric to the nearest plane, as `impedance` takes it), the longest with its closest gap
   in mm and in H. Diff pair partners together and supplies are left out; a pair is one name.
   Rows with `xtalk_limit 20` or `xtalk_nets ["*CLK*", "*PCIE*"]`. Judge only what matters:
   fast edges (clocks, high-speed lanes, resets next to them) against long neighbours, e.g.
   `xtalk_length {"*CLK*": 5, "*PCIE*": 10}`; slow buses (I2C, GPIO) coupling for centimetres
   are usually fine. `xtalk_spacing` changes the multiple of H. On a 2-layer board H is the whole
   board, so 3H takes in most neighbours (the line says so): read the closest gaps instead.
   Same-layer only (broadside coupling between adjacent signal layers is not screened).
7. **Impedance along the line** (routed boards). `audit` alone gives one `impedance` line: Z of
   each signal track piece by piece, from its width, its layer and the copper actually over and
   under that spot (a pour on a signal layer between is the plane there, a gap in a plane takes
   the copper past it; 0.5 mm around a net's own pads and vias left out). Diff pairs first, as
   Zdiff at their gap there: each kind's range of main Z, how many vary 10% from their own, the
   one varying most and why. Rows with `z_limit` / `z_nets`. Judge the controlled nets against
   the stackup's targets: `z_diff_target {"*USB*": 90, "*PCIE*": 85, "*ETH*": 100}`,
   `z_target {"*RF*": 50}` (`z_tolerance` %, default 10; `z_off_mm`, default 1 mm, the length
   allowed off). A layer change to a layer the class width does not fit, a neck, a pour under
   it on the layer between show as stretches off, with where. Fix with `set_rules` widths per
   layer (`impedance_layer`) or by routing; tracks off plane have no Z (see `plane`).
8. `run_erc` (`warnings=true` for the full list) and, on a board, `refill_zones` then
   `run_drc`. Narrow with `types` / `refs`; the type counts are the whole board, the filter line
   says what matches.
9. Report what matters, grouped by severity, with the refs and nets involved.

## Edit a schematic

- Parts: `sch_add_part` with `lcsc` (fetched from LCSC: the usual way for a new part),
  `lib_id` (a symbol from the project's sym-lib-table) or `lib_id="power:GND"` (power symbols
  are generated, no library needed; `power:PWR_FLAG` is KiCad's flag, #FLG: connect its pin 1
  to a net ERC should take as driven, e.g. a connector's VBUS or GND). KiCadD ships no symbol libraries: `Device:R` exists only
  when the project has a Device library, or when the schematic already uses `Device:R` (its own
  copy is taken). On an existing project use the symbols it already uses (`sch_summary`; a
  "not found" reply names them, e.g. `CM5IO:R`). `ref="R?"` takes the next free number. Check
  the reply: it names the footprint and warns when it is missing.
- Connections: `sch_connect` (stubs + net labels; `power: ["GND", "+3V3"]` for power symbols;
  `NC` for no-connect) is the reliable way. `sch_wire` draws real wires for readability.
- Change: `sch_move`, `sch_set_fields` (values, footprints, LCSC numbers, DNP, BOM flags; many
  parts at once with `refs` + `set`), `sch_disconnect`, `sch_delete` (parts, `wires` by a point
  on them, `dangling=true` for wires hanging off nothing).
- Hierarchy: `sch_add_sheet` with `ports`; on the parent connect `SheetName.PORT`.
  **Plan sheets before entering parts:** one A4 sheet holds about 30-40 parts. Beyond that,
  `sch_add_part` without x / y puts parts below the frame. Give each functional block its own
  sheet early (power, the main IC, each connector group); moving parts between sheets later is
  costly.
- Give x / y on a clear spot. A pin that lands on another part's pin joins their nets with no
  warning: read the `->` net names in the reply.
- Check with `sch_netlist` (or `sch_part` for one part) after each group of edits, and
  `run_erc` at the end.

## Parts (LCSC / JLCPCB)

1. Find candidates by specification: `lcsc_search` (`value`, `package`, `tolerance`,
   `dielectric`, `voltage_min`, `keyword`, `basic_only`). Prefer **basic** parts (no JLCPCB
   setup fee), then preferred, then extended; the first hit is checked live for stock (`live`,
   default 1). **Live checks and fetches share one EasyEDA rate limit:** after about 30
   requests in a few minutes, EasyEDA refuses requests (HTTP 403) for a while. Browse with
   `live: 0`, fetch each part number once, and wait rather than retry when refused.
   The index already has JLC stock; the live check is a second opinion.
   MOSFETs, LDOs, op-amps, inductors and diodes list their electrical parameters (one test
   condition each, after @: `Rds(on) 17 mΩ @10V`); filter with `where`, your thresholds in
   SI with prefixes: `category "MOSFET", where {"rds_on": "<=20m", "vds": ">=30", "vgs_th":
   "1..2.5"}` (gate drive from 3.3 V logic), `category "LDO", where {"vout": "3.3",
   "dropout": "<=300m", "iout": ">=500m"}`. A part that lacks a parameter does not match
   (inductor Isat is given for about half, SRF for a quarter; diode Cj and MLCC ESR / DC bias
   never), so drop a condition rather than conclude no part exists. A wrong name lists them.
   **Isat is given for no basic / preferred inductor**: with `where {"i_sat": ...}` leave
   `basic_only` off (or filter on `i_rated`, given for most, and check Isat on the hits).
2. Use one: `sch_add_part lcsc="C25804"` (symbol + footprint saved to the project's LCSC
   library), or `lcsc_fetch` first to inspect pins and pads, or put the number on an existing
   part with `sch_set_fields {"LCSC": "C25804"}`.
   Diodes and LEDs: pad 1 is not always the same pin. EasyEDA symbols differ (C2286 has pin 1 =
   A, C779449 pin 1 = K), and KiCad's LED_* / Diode_* footprints have pad 1 = K. Changing only
   the LCSC number or the footprint on an existing symbol can reverse the part. `sch_set_fields`
   warns `polarity reversed` then: place the part with its own symbol instead.
   The part's 3D model comes along (EasyEDA's, converted to `lcsc.3dshapes/<package>.wrl` and
   placed as EasyEDA places it) for the 3D viewer; `lcsc_fetch model=false` leaves it out. A
   reply line says whether there was one.
3. Many parts at once: `lcsc_match` (dry run) groups the parts without an LCSC number by value
   and footprint and picks one for each group of resistors, capacitors, inductors, ferrite
   beads (FB) and LEDs (colour in the value) in chip packages. Check the choices (capacitor
   voltage and dielectric: write them in the value, "100nF 50V X7R"), then `apply=true`
   (one `sch_undo`); `pick {"R1": "C..."}` for another part or a group it could not match.
   Then `sync_pcb`.
4. No suitable LCSC part: `lib_create_footprint` (chip, sot23, soic, so, qfn, qfp, header) and
   `lib_create_symbol` (a box symbol from a pin list, `footprint` set to the new one); libraries
   already on disk: `lib_register`.

## Simulate a part of the circuit (ngspice)

`simulate` runs a region of the schematic headless: `around ["U1"]` (an LDO with its input and
output capacitors: a hop passes a supply only to two-pin parts), `refs`, or `sheet`; `hops` to
go further along signal nets. Models come from the parts' data (their LCSC field: put LCSC
numbers on first): R, C, L, diodes, MOSFETs, LDOs and op-amps (behavioural), or a part's own
Sim.* model. Read the `models:` lines before the results: every value not from the data is
marked **est.** / **assumed** (MLCC C_eff from DC bias, ESR / ESL, an LDO's loop bandwidth, an
op-amp's GBW when missing), and parts without a model are listed as left out, their nets open.

- Sources: `sources {"VIN": 5}` (or `"dc 5 ac 1"`, `"pulse(...)"`, `"sin(5 0.05 100k)"`);
  supplies not given get the voltage in their name (said). Loads: `loads {"+3V3": 0.3}`
  (amps), a pulse for a load step, or `"10 ohm"`.
- `analysis`: `op`; `tran {stop: "2m", step?, window?}` (average, ripple p-p over the window,
  min / max with when, a step's overshoot / rise / settling); `ac {from, to}` with an `ac 1`
  source (gain, peak, -3 dB). Inductor and regulator currents come with Isat / Irated / Iout
  to compare. Judge against your own targets; the tool gives no verdicts.
- MLCCs are at their DC-biased C_eff by default (`dc_bias false`: nominal), the bias from an
  op run first (tran: the window's average). Right for linear circuits; a switching converter
  or an AC-coupled node is not at its DC point there, so say so or set `c` per part.
- `corners true` is **all low / all high** (every R / C / L moved together), not a worst case
  or Monte Carlo: in a divider or matched RCs the ends cancel. Try a mix by setting single
  values with `models`.
- Through a supply the region takes every two-pin part on it: all the rail's decoupling (right
  for an LDO's stability and load steps), but also e.g. an LED's resistor whose far end is then
  open. Read the `cut at the region's edge` line ("LEDA (R4 here; D1 outside)"): more `hops`,
  or `models {"R4": {"exclude": true}}`.
- Better data than the estimate (a datasheet's ESR, a vendor model): `models {"C5": {"esr":
  "8m", "esl": "0.5n"}, "U1": {"psrr": 55, "psrr_f": "5k", "psrr_hf": 25, "loop_bw": "50k"}, "U2": {"subckt": "TPS7A02",
  "lib": "models/tps7a02.lib", "pins": ["IN", "GND", "EN", "OUT"]}}`, `exclude true` to
  leave one out. Say which numbers were estimated when you report.
- ESL is the part's own: the board's mounting inductance (pads, vias) is not in `simulate`;
  `pdn` adds it from the layout.
- `plot true` returns a PNG next to the text (tran: the probes, and the sources' / inductors'
  currents in a second panel; ac: gain and phase; dc: against the swept value; corners as faint
  lines). Ask for it when the shape matters (ringing, a Bode plot); the numbers stay in the text.
- `dc {source: "VIN", from: 0, to: 5, step: "10m"}` sweeps one of `sources` (volts) or `loads`
  (amps): the source's current against its voltage is an I-V curve (a diode, a MOSFET's Id with
  a gate source fixed).
- An LDO's PSRR has one number in the data (usually at 100 Hz - 1 kHz). Over frequency it is
  assumed: flat to `psrr_f` (loop_bw / 10), then 20 dB/dec worse to `psrr_hf` (20 dB). For ripple
  from a switcher (100 kHz - 2 MHz), read the datasheet's PSRR curve and set those two.

## PDN impedance (pdn)

`pdn {net: "+1V8"}` works out Z(f) at the load from the layout: each capacitor between the rail
and its ground as ESR + (body ESL + its mounting) + C_eff, the plane pair's capacitance, the
regulator (`vrm {r, l}`; 2 mΩ + 20 nH assumed when not given) and the load's own vias. The
mounting comes from the board: the track from each pad to its vias, the via loop down to the
rail's and the ground's planes, the plane pair from the capacitor to the load. Read the
capacitor lines (by SRF): `L 400 pH + 1.3 nH (tracks 0.2, vias 0.9, planes 0.3)` says where
each one's inductance is, so you can tell a far capacitor from a poorly mounted one.

- The target is yours: `target` (ohm), e.g. V × ripple % / transient current
  (1.0 V, 3%, 10 A: 3 mΩ). Without it no verdict, only the peaks and the lowest |Z|.
- Peaks are named by the capacitors on each side (`between 1 µF ... and 100 nF ...`): add a value
  between them, or cut the mounting of the higher one (vias next to the pads, a via in pad).
  Try it before placing: `add [{c: "2.2u", package: "0402", count: 4}]` (the board's median
  mounting), `models {"C12": {"mount": "0.3n"}}` (as if re-routed), `exclude`.
- The load defaults to the part with most pads on the rail; give `at` for another IC.
- `volts` sets the DC bias voltage when the name has none. MLCCs lose a lot at their rated
  voltage (est.), so check C_eff, not the value.
- Limits: lumped planes (no cavity resonances); a peak above every SRF (planes with the
  mounting) is rough in height. Above ~100 MHz the package and die capacitance dominate; they
  are not in the board.

## Reflections and series termination (reflection)

`reflection {net: "CLK"}` simulates the net's edge as routed: every track piece a lossless line
with its layer's and width's Z0 and delay, the driver a step through `rout`, each receiver
`cin`. It runs as routed and with a series resistor of Z0 − rout (E24), and reports per
receiver the overshoot, ringback, flight time, settling and the 50% crossings (more than one:
not monotonic, which matters on a clock or strobe).

- Give the driver's numbers from the datasheet or IBIS: `rise` (10-90%), `rout`, `vdd`, and
  `cin` for the receivers. Defaults (1 ns, 30 Ω, 3.3 V, 3 pF) are said as assumed.
- The `2·TD against the rise` ratio in the copper line: well under 1/3 the line is short
  (lumped) and termination rarely matters; around 1 or more, read the waveforms.
- A series resistor already on the net (driver → R → line) is found: the driver is fetched
  from the other side, and the suggestion replaces its value. `rs [22, 33]` tries values.
- `driver "U3.12"` when there are several outputs (a bus); one net at a time, single-ended
  (for a pair, each member).
- Not in it: losses, via barrels, coupling to other nets (that is `audit` crosstalk), packages.

## A new project (new_project)

`new_project {path: "/abs/folder", copper_layers: 4, outline: [60, 40]}` writes
`<name>.kicad_pro`, `.kicad_pcb` and `.kicad_sch` in the folder (made when missing), opens
them, sets the layers and a stackup (or `stackup: "JLC04161H-7628"`), draws the outline and
saves. It never writes over a file: an existing project is opened with `open_board`. Like
`open_board`, it is refused while the open board has unsaved changes (`discard=true` drops
them). KiCadD ships no symbol libraries: start the schematic with `lcsc_fetch` / `sch_add_part
lcsc=...` or `lib_create_symbol`.

## Starting and stopping KiCad (launch_kicad / close_kicad)

When every tool fails with "is KiCadD running? ... launch_kicad starts one", nothing serves
kicadd-mcp's socket. `launch_kicad {path: "/abs/project"}` starts the headless server there
(no path: an empty one, then `new_project` / `open_board`); `mode: "desktop"` starts the PCB
editor on the project's board instead, for a user to watch, review and route. It is refused
while a KiCad already answers (use `open_board` to switch the server's board). `close_kicad`
stops only what `launch_kicad` started, refused while the board or the schematic has unsaved
changes: save (`save_board`, `sch_save`) or pass `discard=true`. Over HTTP both need
kicadd-mcp's `--allow-launch`.

## A new board (outline, layers, rules, pours)

From a blank project, before placing parts:

1. `board_setup` shows the layers and stackup. `copper_layers=4` (and `thickness`,
   `outer_copper_oz`, `inner_copper_oz`) makes a new stackup like Board Setup's default.
   For controlled impedance use the fab's own: `stackup="JLC04161H-7628"` (`stackup="list"`
   names JLCPCB's 4- and 6-layer 1.6 mm ones) sets its prepreg / core thicknesses and Dk; the
   Dk values differ between JLCPCB's pages, so have the user check widths with JLCPCB's
   impedance calculator. Fewer layers only once the inner ones are empty.
2. `set_rules preset="jlcpcb_2layer"` (or `jlcpcb_4layer`) sets the board minimums to the
   fab's standard process and raises the Default class to them. Add net classes in the same
   call. The numbers are the fab's published ones as of a date; tell the user to check them
   before ordering.
3. `board_outline rect=[x0, y0, x1, y1] corner_radius=1 hole_inset=3.5` replaces the outline
   and puts M3 holes (H1..H4, out of the BOM and the CPL) at the corners. `cutouts` adds
   slots or round holes in the board; `holes` adds holes at chosen points (plated to GND
   with `{"x", "y", "plated": true, "net": "GND"}`). `keep_old=true` adds to the outline
   there is. Read the warnings: a hole where a part already has one is left out; a hole on a
   part or through a track, a cut-out through tracks, and tracks outside a new outline are
   listed at once (DRC would show them later).
4. `zones create={"net": "GND"}` pours GND over the board outline on F.Cu and B.Cu (or
   `layers: ["In1.Cu"]` for a plane; `rect: [x0, y0, x1, y1]` or `outline` for part of the board). Pour after sync_pcb, once the net exists. Two zones of
   different nets on the same layer need different priorities: the higher one is filled, the
   other keeps clear. A new zone inside a bigger one gets the higher priority by itself; an
   overlap at the same priority comes with a warning and the `zones ids=[...] priority=1`
   call that settles it.
5. All of them are one `undo` each.
6. After routing: `stitch_vias` (GND by default, 5 mm grid, also one via in each island of
   the pour that has none) and `teardrops` (vias and through-hole pads; `smd=true` for SMD
   pads). Both are one `undo`, and both refill the zones.

## Schematic to board, placement

1. `sync_pcb dry_run=true`, read it, then `sync_pcb` (one `undo` takes it all back). New
   footprints land outside the outline. A warning that a reference "is already on the board"
   means a part placed by hand (`add_footprint`) has the same ref as a symbol: the sync adds a
   second one. Delete the board's one, or renumber it:
   `pcb_set_fields parts={"R22": {"Reference": "R23"}}`. `run_drc` and `export_fab` also warn
   about a reference used twice, and `export_fab` about parts outside the outline.
   Tidy numbering (R1, R2, ... in position order, or the "R?" of parts placed unnumbered):
   `sch_annotate dry_run=true` to see it, then `sch_annotate` (`reset=true` renumbers every
   part; `sheet` or `refs` limits it) and `sync_pcb`: the board's parts are renamed in place
   (matched by UUID), nothing moves. Renumbering after the user has seen the board changes the
   refs on its silkscreen and in the BOM: ask first.
   Fabrication attributes (DNP, exclude from BOM / position files) come from the symbol: one
   set on the board only (`pcb_set_fields`) is dropped by the sync. The answer lists it with
   the `sch_set_fields` call that keeps it: set it on the symbol, then sync again.
2. `auto_place` (default: the parts outside the board). Useful options: `groups` or the Block
   field to keep functional blocks together, `rules` (edge, region, rotation, near), `fixed`
   for parts that must not move (place connectors first, then fix them), `back_decaps`.
   `measure_only=true` reports the numbers without moving anything.
   Connectors (USB, RJ45, DC jack): an edge rule `{"J1": "left"}` turns one to open over that
   edge, with the footprint's own "PCB Edge" line on the board edge (KiCad's library has it for
   many connectors), else its courtyard front flush with the edge. `{"J1": {"side": "left",
   "overhang": 1}}` puts the front 1 mm past the edge; `"inside": true` keeps it inside. The
   reply's `edge:` lines say which way each went and why; check flush ones against the
   connector's datasheet (the enclosure decides the real overhang).
3. On a board that is already placed or routed, `auto_place` keeps the current placement when
   the new one is not better (`force=true` applies it anyway). Tracks are obstacles: rip up
   with `delete_tracks` first if the parts should really move.
4. After moving parts: `refill_zones`, then `run_drc`. Zones keep their old shape until
   refilled, and that shows up as clearance / soldermask errors that are not real.
5. Fine-tune with `move_footprints` (every reply carries a local DRC of the moved parts) and
   `check_drc`: by a delta (`refs`, `dx`, `dy`, `rotate`, `flip`), or to absolute places,
   `parts {"U1": {"x": 105, "y": 120, "rotation": 90, "side": "bottom"}, "C3": {...}}`, many
   parts in one undoable edit. `query_area ref="U1" radius=1` gives the part's own position,
   side and rotation first, then its neighbours.
6. **A dense board** (a small outline, parts on both sides): `auto_place` keeps room for each
   part's courtyard *and* its silkscreen (reference text, outline) by default. When it says
   "no room" with an area that should do, use `silk_room=false` (courtyards only, as DRC
   checks them; `silk_tidy` or hidden references afterwards), `spacing` 0.1 or 0, and `back`
   for the parts that may go underneath. The "no room" line gives the area per side.

**Before placing a dense or two-sided board, plan it on paper:**
- **Area per side.** Add up the courtyards. The connectors usually fix one side; is the rest
  room for everything else?
- **Pair order.** A part on the bottom is mirrored. An IC on B.Cu presents its pins in the
  opposite order to a connector on F.Cu, so every D+/D- pair between them must cross (a via
  pair each). Prefer the high-speed IC on the connectors' side, or check that the pins' order
  matches the connector before flipping it.
- **Pair order is checked:** `audit sections=["diff_pair"]` has a "pair order" line naming the
  pairs whose P and N swap between two parts (straight lines P-P and N-N cross) as placed,
  routed or not. Check it right after placing, before routing the pairs. `move_footprints` and
  `auto_place` also add a "pair order" note when the parts just moved leave a pair swapped
  (nothing when it is fine). Example: the hub IC U1 on top, a USB-A connector J2 moved to
  `"side": "bottom"`. J2 is mirrored, so its D+ and D- swap sides as U1 sees them, and the move answers
  `note: pair order (a placement check, not of the routing): 1 of 1 pair(s) have P and N swapped ... USB1_P / USB1_N: U1 - J2 (bottom)`.
  Fix it before routing: put J2 back on top, turn it, move U1's side, or accept a via pair. A
  USB-C's doubled D+ / D- pads (A6 / B6, A7 / B7) fit either order, and are not reported.
  It is a placement check only: it compares straight lines between the pads, not the copper,
  so a pair you have routed across (a via pair, a loop) stays listed. Once the pairs are routed,
  judge them with the diff pair rows (skew, uncoupled) and DRC, not with this line.
- **Thin board for 2-layer impedance.** On 2 layers the plane is the whole board away. 90 Ω
  differential on 1.6 mm needs about 0.7 mm tracks, impossible at a fine-pitch IC. Either take
  a thinner board (0.8 mm: about 0.55 mm), or accept a higher Zdiff on short runs: USB 2.0
  tolerates about 110-130 Ω over a few cm, and hand designs on 2 layers often run there. Say
  which you chose. `impedance z=50 ground_gap=0.15` gives a single track's width with ground
  poured beside it (coplanar waveguide with ground, CPWG): narrower than the plain microstrip.

## Routing (the repetitive part, not an autorouter)

The tools draw tracks the way KiCad's interactive router does (around obstacles, never moving
existing copper, net class widths and vias, clearance / keepout / edge rules). You decide what
to connect and where to change layer; the user routes what needs judgement (buses, diff pairs,
length matching, dense escapes).

0. Save as you go: the board lives in the server's memory until `save_board`. Save right
   after placement and every few routing calls (and `sch_save` after schematic work), so a
   server restart or crash loses minutes, not the whole layout.
1. Placement first, then `fanout nets=["GND", ...]` on boards with planes or a ground pour on
   the other side: a via next to each SMD pad. Through-hole pads are skipped; a net without a
   zone on another layer is only warned about.
2. `route_short` with the parts (or nets) and a `max_length` (e.g. 10 mm): decoupling caps,
   pull-ups, LED resistors. `dry_run=true` first to see what would fail. It goes shortest first.
   Where one layer won't do it adds vias: one on the way for ends on different sides, over the
   other side and back for a blocked one (`vias=false`: none). Via spots are tried first along
   the straight line, then at free spots round it (a gap beside a part). The via search has
   45 s per call: on a dense board the connections after that say "vias not tried"; route
   them again in smaller groups. On a dense board run it per
   block (`refs` of one IC and its passives) rather than for everything at once, so the long
   ones don't take the channels the short ones need.
3. One connection: `route from="U1.5"` (to the nearest unrouted connection) or `to="C3.1"`.
   Steer it with `waypoints: [[x, y], ...]`; change layer with `[x, y, "B.Cu"]` among them, or
   `via: [x, y]`. Pick via spots yourself (`query_area` around the point). Between two
   waypoints the router may still go round something; the reply says how far it leaves the
   box between them ("leaves the way through the waypoints by ..."), and `strict=true` refuses
   such a route instead.
4. Read the numbers: length vs the straight line (above ~1.5x it detours), segments, vias, the
   narrowest gap. A failure says how far it got and what is in the way: `delete_tracks` that,
   move a part, or give waypoints round it. Between close pads (a solder jumper, fine pitch)
   "the way out ... is tight": a narrower `width`, or start at a point inside the pad on the side
   the track leaves (`from: [x, y]`), not at its centre.
   A route that changes layer reports them in order ("F.Cu -> B.Cu").
5. Everything of one call is one `undo`. Then `refill_zones` and `run_drc`.
6. Pour ties: `route from="C12.2" to="zone"` ends on the net's own pour: on the pad's layer
   when the pour is there, else with a via down to the nearest layer that has it (not in the
   pad). It tries the nearest spots in the fill until one works. Use it for VBUS / power pads
   to their pours; `fanout` does many GND / supply pads at once.
7. Rip-up: `delete_tracks` filters narrow each other (`nets: ["GND"], region: [...]` deletes
   only GND inside the box). The reply lists what went, per net: check it before going on
   (`undo` brings it back).

## Rules and zones

- `get_rules` first. `set_rules` adds/changes net classes, assigns nets by pattern
  (`{"+5V": "Power", "/USB_D*": "USB"}`) and sets board minimums; it reports the DRC error
  count before and after. Existing tracks keep their width. A class for a current:
  `{"classes": {"VIN_2A": {"current": 2}}, "assign": {"VIN": "VIN_2A"}}` sets the IPC-2221 width
  for the board's outer copper (the note says what inner layers would need).
- Controlled impedance: set the fab's stackup first (`board_setup stackup="JLC04161H-7628"`),
  then `impedance z=50` (or `z_diff=90 gap=0.2`) shows each copper layer's width; `width` alone
  gives a width's impedance. `set_rules {"classes": {"RF": {"impedance": 50}, "USB":
  {"diff_impedance": 90, "diff_pair_gap": 0.15}}}` writes them into net classes, for F.Cu unless
  `impedance_layer` names another (a class has one width: the note gives the other layers').
  A pair with no gap given uses the class's clearance as the gap. The reference planes are the
  nearest layers poured over 40% of the board (not their name or Board Setup type; an empty layer
  counts as a plane to be), so a layer next to another signal layer is measured to the plane
  beyond (`across In2.Cu` in the line, its copper in H). The note lists each layer's coverage:
  if a layer meant as a plane shows as a signal layer, pour it (`zones create`) and look again;
  the response also says which planes are not poured. Tell the user to check the widths with the
  fab's calculator before ordering.
- `zones` lists copper zones; with `nets` / `layers` / `ids` / `all` and new values it changes
  clearance, minimum width, priority or pad connection and refills. `create` makes a new one
  (see "A new board").
- After `undo` / `redo` of a zone, outline or layer edit, the fills are of the other state:
  the response says so. `refill_zones` before reading `unconnected` or running DRC.

## Look at it

`board_render` returns a PNG of the board as it is (KiCad's own plot: copper, pads, pours,
silkscreen, outline) with mm rulers on the top and left and the unrouted connections as thin
white lines. `refs` frames parts (and zooms to them), `nets` draws nets in cyan, `area`
crops, `layers: ["silk"]` (silkscreen, the mask openings where the pads are, outline) /
`["fab"]` / names, `back=true` from below (mirrored). Use it for
what coordinates do not show: silkscreen over pads, part orientation, crowding, how a route
runs. Keep `size` modest (default 1200 px) unless the detail needs more.

`sch_render` does the same for a schematic sheet: KiCad's plot with mm rulers in the
coordinates `sch_summary` gives. `refs` frames parts (and goes to their sheet), `nets` draws a
net's wires, labels and pins in blue, `sheet` / `area` / `frame=true` (the whole page with its
title block; refs / nets are still marked but not zoomed to). Look at it after a group of `sch_add_part` / `sch_connect` edits: overlapping
symbols or text, labels away from their wires, a sheet that does not read well.

## Manufacturing (JLCPCB)

1. `refill_zones` and a clean `run_drc` (or the user's agreement on what remains).
   `export_fab` runs the DRC itself and puts the error count first; `strict=true` writes
   nothing while there are DRC errors, a reference twice or parts off the board.
2. Silkscreen: `silk_tidy dry_run=true` lists references over pads, over each other or off the
   board and where they would go; `silk_tidy` moves them (one undo), then `board_render
   layers=["silk"]` to look. It only moves the ones with a problem (all=true: every one asked).
3. Parts not to be assembled: `sch_set_fields` / `pcb_set_fields` with `dnp` or
   `exclude_from_bom` (they leave both BOM and CPL).
4. `export_fab`: Gerbers, drill, the upload zip, BOM and CPL in JLCPCB's columns, from the
   design in memory. Read its notes: BOM lines without an LCSC number, through-hole parts in
   the CPL, rotations to check in JLCPCB's preview.
5. The schematic for the documents or a review: `sch_export_pdf` (every sheet in one PDF,
   unsaved edits included; default `<project>/<schematic>.pdf`).

## Repeated work

`batch` runs a list of tool calls in one go: `{"steps": [{"tool": "move_footprints", "args":
{...}}, {"tool": "undo"}], "repeat": 20}`. It stops at the first error (`stop_on_error=false`
to go on); `show="last"` gives the last step in full. A step cannot use an earlier step's
answer: when the next call depends on a result, make separate calls.

## Not available (say so instead of improvising)

Autorouting a whole board, differential pairs, length tuning, pushing (shoving) existing tracks;
PCB or assembly price quotes; STEP export (3D models are VRML only). For those, the user works in
the editor or on the manufacturer's site.
