---
name: kicadd
description: How to work on a KiCad schematic or PCB through the kicadd MCP tools (KiCadD): reviewing a design, editing the schematic, sourcing LCSC/JLCPCB parts, schematic-to-board, auto placement, routing (route, route_short, fanout), design rules, DRC/ERC and fabrication output. Use whenever the kicadd tools (board_summary, sch_summary, auto_place, ...) are available and the task touches a schematic, board, footprint, part or manufacturing file.
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
3. No suitable LCSC part: `lib_create_footprint` (chip, sot23, soic, so, qfn, qfp, header) and
   `lib_create_symbol` (a box symbol from a pin list, `footprint` set to the new one); libraries
   already on disk: `lib_register`.

## Schematic to board, placement

1. `sync_pcb dry_run=true`, read it, then `sync_pcb`. New footprints land outside the outline.
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
   move a part, or give waypoints round it.
5. Everything of one call is one `undo`. Then `refill_zones` and `run_drc`.

## Rules and zones

- `get_rules` first. `set_rules` adds/changes net classes, assigns nets by pattern
  (`{"+5V": "Power", "/USB_D*": "USB"}`) and sets board minimums; it reports the DRC error
  count before and after. Existing tracks keep their width.
- `zones` lists copper zones; with `nets` / `layers` / `ids` / `all` and new values it changes
  clearance, minimum width, priority or pad connection and refills.

## Manufacturing (JLCPCB)

1. `refill_zones` and a clean `run_drc` (or the user's agreement on what remains).
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
PCB or assembly price quotes; 3D models of LCSC parts; STEP export. For those, the user works in
the editor or on the manufacturer's site.
