---
name: kicadd
description: How to work on a KiCad schematic or PCB through the kicadd MCP tools (KiCadD): reviewing a design, editing the schematic, sourcing LCSC/JLCPCB parts, a new board's outline / layers / pours / fab rules, schematic-to-board, auto placement, routing (route, route_short, fanout), design rules, DRC/ERC and fabrication output. Use whenever the kicadd tools (board_summary, sch_summary, auto_place, ...) are available and the task touches a schematic, board, footprint, part or manufacturing file.
---

# Working with KiCadD (kicadd MCP tools)

KiCadD is a KiCad build with an agent API. The tools act on the design **in memory**: in the
desktop app that is the open Schematic / PCB Editor (the user sees every edit at once and can
Ctrl+Z it); headless it is a board loaded by `kicad-cli api-server`. The tool descriptions say
what each tool does; this skill says how to use them together.

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
5. **Save only when asked** (`save_board`, `sch_save`). Saving writes this build's file format
   (KiCad nightly 10.99); KiCad 9/10 may not open the files afterwards. Say so the first time.
6. **Keep answers short.** Long lists are cut by default; pass `detail=true` only when you need
   every position or violation. For repeated or routine work use `batch` (one call, one line per
   step) instead of many single calls.
7. **Units:** mm everywhere, Y grows downwards. The schematic snaps to the 1.27 mm grid. Angles
   in degrees, counter-clockwise.

## Review a design

1. `sch_summary` (parts, sheets, unconnected pins) and `board_summary`.
2. `audit`: decoupling caps measured on the board, supply sources, ESD on external connectors,
   one-pin nets. Judge the findings (a socket for a programmed chip needs less decoupling than a
   switching regulator).
3. `run_erc` (`warnings=true` for the full list) and, on a board, `refill_zones` then
   `run_drc`. Narrow with `types` / `refs`; the type counts are the whole board, the filter line
   says what matches.
4. Report what matters, grouped by severity, with the refs and nets involved.

## Edit a schematic

- Parts: `sch_add_part` with `lib_id` (a library symbol), `lcsc` (fetched from LCSC) or
  `lib_id="power:GND"` (power symbols are generated, no library needed). `ref="R?"` takes the
  next free number. Check the reply: it names the footprint and warns when it is missing.
- Connections: `sch_connect` (stubs + net labels; `power: ["GND", "+3V3"]` for power symbols;
  `NC` for no-connect) is the reliable way. `sch_wire` draws real wires for readability.
- Change: `sch_move`, `sch_set_fields` (values, footprints, LCSC numbers, DNP, BOM flags; many
  parts at once with `refs` + `set`), `sch_disconnect`, `sch_delete` (parts, `wires` by a point
  on them, `dangling=true` for wires hanging off nothing).
- Hierarchy: `sch_add_sheet` with `ports`; on the parent connect `SheetName.PORT`.
- Check with `sch_netlist` (or `sch_part` for one part) after each group of edits, and
  `run_erc` at the end.

## Parts (LCSC / JLCPCB)

1. Find candidates by specification: `lcsc_search` (`value`, `package`, `tolerance`,
   `dielectric`, `voltage_min`, `keyword`, `basic_only`). Prefer **basic** parts (no JLCPCB
   setup fee), then preferred, then extended; the first hits are checked live for stock.
2. Use one: `sch_add_part lcsc="C25804"` (symbol + footprint saved to the project's LCSC
   library), or `lcsc_fetch` first to inspect pins and pads, or put the number on an existing
   part with `sch_set_fields {"LCSC": "C25804"}`.
3. Many parts at once: `lcsc_match` (dry run) groups the parts without an LCSC number by value
   and footprint and picks one for each group of resistors, capacitors, inductors, ferrite
   beads (FB) and LEDs (colour in the value) in chip packages. Check the choices (capacitor
   voltage and dielectric: write them in the value, "100nF 50V X7R"), then `apply=true`
   (one `sch_undo`); `pick {"R1": "C..."}` for another part or a group it could not match.
   Then `sync_pcb`.
4. No suitable LCSC part: `lib_create_footprint` (chip, sot23, soic, so, qfn, qfp, header) and
   `lib_create_symbol` (a box symbol from a pin list, `footprint` set to the new one); libraries
   already on disk: `lib_register`.

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
   `layers: ["In1.Cu"]` for a plane). Pour after sync_pcb, once the net exists. Two zones of
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
2. `auto_place` (default: the parts outside the board). Useful options: `groups` or the Block
   field to keep functional blocks together, `rules` (edge, region, rotation, near), `fixed`
   for parts that must not move (place connectors first, then fix them), `back_decaps`.
   `measure_only=true` reports the numbers without moving anything.
3. On a board that is already placed or routed, `auto_place` keeps the current placement when
   the new one is not better (`force=true` applies it anyway). Tracks are obstacles: rip up
   with `delete_tracks` first if the parts should really move.
4. After moving parts: `refill_zones`, then `run_drc`. Zones keep their old shape until
   refilled, and that shows up as clearance / soldermask errors that are not real.
5. Fine-tune with `move_footprints` (every reply carries a local DRC of the moved parts) and
   `check_drc`.

## Routing (the repetitive part, not an autorouter)

The tools draw tracks the way KiCad's interactive router does (around obstacles, never moving
existing copper, net class widths and vias, clearance / keepout / edge rules). You decide what
to connect and where to change layer; the user routes what needs judgement (buses, diff pairs,
length matching, dense escapes).

1. Placement first, then `fanout nets=["GND", ...]` on boards with planes or a ground pour on
   the other side: a via next to each SMD pad. Through-hole pads are skipped; a net without a
   zone on another layer is only warned about.
2. `route_short` with the parts (or nets) and a `max_length` (e.g. 10 mm): decoupling caps,
   pull-ups, LED resistors. `dry_run=true` first to see what would fail. It goes shortest first.
3. One connection: `route from="U1.5"` (to the nearest unrouted connection) or `to="C3.1"`.
   Steer it with `waypoints: [[x, y], ...]`; change layer with `[x, y, "B.Cu"]` among them, or
   `via: [x, y]`. Pick via spots yourself (`query_area` around the point).
4. Read the numbers: length vs the straight line (above ~1.5x it detours), segments, vias, the
   narrowest gap. A failure says how far it got and what is in the way: `delete_tracks` that,
   move a part, or give waypoints round it. Between close pads (a solder jumper, fine pitch)
   "the way out ... is tight": a narrower `width`, or start at a point inside the pad on the side
   the track leaves (`from: [x, y]`), not at its centre.
   A route that changes layer reports them in order ("F.Cu -> B.Cu").
5. Everything of one call is one `undo`. Then `refill_zones` and `run_drc`.

## Rules and zones

- `get_rules` first. `set_rules` adds/changes net classes, assigns nets by pattern
  (`{"+5V": "Power", "/USB_D*": "USB"}`) and sets board minimums; it reports the DRC error
  count before and after. Existing tracks keep their width.
- `zones` lists copper zones; with `nets` / `layers` / `ids` / `all` and new values it changes
  clearance, minimum width, priority or pad connection and refills. `create` makes a new one
  (see "A new board").
- After `undo` / `redo` of a zone, outline or layer edit, the fills are of the other state:
  the response says so. `refill_zones` before reading `unconnected` or running DRC.

## Look at it

`render_board` returns a PNG of the board as it is (KiCad's own plot: copper, pads, pours,
silkscreen, outline) with mm rulers on the top and left and the unrouted connections as thin
white lines. `refs` frames parts (and zooms to them), `nets` draws nets in cyan, `area`
crops, `layers: ["silk"]` / `["fab"]` / names, `back=true` from below (mirrored). Use it for
what coordinates do not show: silkscreen over pads, part orientation, crowding, how a route
runs. Keep `size` modest (default 1200 px) unless the detail needs more.

## Manufacturing (JLCPCB)

1. `refill_zones` and a clean `run_drc` (or the user's agreement on what remains).
   `export_fab` runs the DRC itself and puts the error count first; `strict=true` writes
   nothing while there are DRC errors, a reference twice or parts off the board.
2. Parts not to be assembled: `sch_set_fields` / `pcb_set_fields` with `dnp` or
   `exclude_from_bom` (they leave both BOM and CPL).
3. `export_fab`: Gerbers, drill, the upload zip, BOM and CPL in JLCPCB's columns, from the
   design in memory. Read its notes: BOM lines without an LCSC number, through-hole parts in
   the CPL, rotations to check in JLCPCB's preview.

## Repeated work

`batch` runs a list of tool calls in one go: `{"steps": [{"tool": "move_footprints", "args":
{...}}, {"tool": "undo"}], "repeat": 20}`. It stops at the first error (`stop_on_error=false`
to go on); `show="last"` gives the last step in full. A step cannot use an earlier step's
answer: when the next call depends on a result, make separate calls.

## Not available (say so instead of improvising)

Autorouting a whole board, differential pairs, length tuning, pushing (shoving) existing tracks;
a picture of the schematic;
PCB or assembly price quotes; 3D models of LCSC parts; STEP export. For those, the user works in
the editor or on the manufacturer's site.
