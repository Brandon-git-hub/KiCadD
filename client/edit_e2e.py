"""Edits that used to need the file opened by hand (headless): beta.15 batch 2.

usage: python3 -I edit_e2e.py --build BUILD --demos DEMOS_DIR --work DIR

On a copy of KiCad's pic_programmer demo:
  transform   board_transform dx/dy and center_on_sheet: everything moves together (parts, tracks,
              zones, the outline, texts), the unrouted count stays, undo / redo put it back exactly
  fields      pcb_set_fields layouts: layer (silk / fab), visible, at, size, justify; silk presets;
              undo; a bad layout changes nothing
  text        pcb_text: add / list / edit / move / remove board texts, ${VARS} kept as written,
              back layers mirrored, one undo per call
  units       sch_add_part on an existing multi-unit ref adds a unit not placed (refuses one that is,
              or another symbol); sch_set_fields Reference renumbers all units
  reload      project reload: the file edited on disk comes in; refused with unsaved changes unless
              discard; the history starts over
"""

import argparse
import os
import re
import shutil
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from m5_e2e import FAILS, Mcp, Server, binaries, check  # noqa: E402


def part_at(m, ref):
    err, t = m.tool("query_area", {"ref": ref, "radius": 0.1})
    g = re.search(r"^FP " + re.escape(ref) + r" \S+ @(-?[\d.]+),(-?[\d.]+)", t, re.M)
    return (float(g.group(1)), float(g.group(2))) if g else None


def unrouted(m):
    err, t = m.tool("board_summary", {})
    g = re.search(r"unconnected=(\d+)", t)
    return int(g.group(1)) if g else -1


def prop(pcb, ref, name):
    """(text, at, layer, hidden, size, thickness, justify) of a footprint's property in the file."""
    i = pcb.find('(property "Reference" "%s"' % ref)
    j = pcb.rfind("(footprint ", 0, i)
    k = pcb.find("\n\t(footprint ", i)
    fp = pcb[j:k if k > 0 else len(pcb)]
    g = re.search(r'\(property "%s" "([^"]*)"\s*\(at ([^)]*)\)[^"]*?\(layer "([^"]+)"\)(.*?)\n\t\t\)' % re.escape(name), fp, re.S)
    if not g:
        return None
    rest = g.group(4)
    size = re.search(r"\(size ([\d.]+) ", rest)
    thick = re.search(r"\(thickness ([\d.]+)\)", rest)
    just = re.search(r"\(justify ([a-z ]+)\)", rest)
    return (g.group(1), g.group(2), g.group(3), "(hide yes)" in rest, size and float(size.group(1)),
            thick and float(thick.group(1)), just and just.group(1))


def main():
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    ap = argparse.ArgumentParser()
    ap.add_argument("--build", required=True)
    ap.add_argument("--demos", required=True)
    ap.add_argument("--work", required=True)
    a = ap.parse_args()
    work = os.path.abspath(a.work)
    shutil.rmtree(work, ignore_errors=True)
    os.makedirs(work)
    proj = os.path.join(work, "p")
    shutil.copytree(os.path.join(a.demos, "pic_programmer"), proj)

    _, kicadd_mcp, env = binaries(os.path.abspath(a.build))
    sock = os.path.join(work, "s.sock")
    board = os.path.join(proj, "pic_programmer.kicad_pcb")
    server = Server(a.build, sock, board, env, os.path.join(work, "server.log"))
    check(server.start(), "api-server up on pic_programmer")
    m = Mcp([kicadd_mcp, "--socket", sock, "--stdio"], env)
    try:
        print("[transform]")
        u0 = unrouted(m)
        p0 = part_at(m, "U1")
        err, t = m.tool("board_transform", {"dx": 10, "dy": -5, "dry_run": True})
        check(not err and t.startswith("would move by 10, -5 mm") and part_at(m, "U1") == p0,
              "dry_run: says what, moves nothing: " + t.splitlines()[0])
        err, t = m.tool("board_transform", {"dx": 10, "dy": -5})
        print("   ", t.replace("\n", "\n    ").rstrip())
        p1 = part_at(m, "U1")
        check(not err and p0 and p1 and abs(p1[0] - p0[0] - 10) < 1e-3 and abs(p1[1] - p0[1] + 5) < 1e-3,
              f"U1 {p0} -> {p1}")
        g = re.search(r"unrouted (\d+) -> (\d+)", t)
        check(g and g.group(1) == g.group(2) and int(g.group(1)) == u0, "unrouted the same before and after: "
              + (g.group(0) if g else t[:200]))
        check(re.search(r"\d+ track\(s\)", t) and " zone(s)" in t and "graphic" in t, "counts tracks, zones, graphics")
        err, t = m.tool("run_drc", {"limit": 0})
        drc_moved = t.splitlines()[0] if t else ""
        err, t = m.tool("undo", {})
        check(not err and part_at(m, "U1") == p0 and unrouted(m) == u0, "undo: back exactly: " + t.splitlines()[0][:100])
        err, t = m.tool("undo", {"redo": True})
        check(not err and part_at(m, "U1") == p1, f"redo: moved again ({part_at(m, 'U1')}): " + t.strip()[:300])
        m.tool("undo", {})
        err, t = m.tool("run_drc", {"limit": 0})
        check(t.splitlines()[0] == drc_moved, f"same DRC counts moved and not: {drc_moved} / {t.splitlines()[0]}")

        err, t = m.tool("board_transform", {"center_on_sheet": True})
        g = re.search(r"-> \[([-\d.]+), ([-\d.]+), ([-\d.]+), ([-\d.]+)\]", t)
        check(not err and g, "center_on_sheet: " + t.splitlines()[0])
        if g:
            x0, y0, x1, y1 = map(float, g.groups())
            # pic_programmer is on A4 (297 x 210)
            check(abs((x0 + x1) / 2 - 148.5) <= 0.5 and abs((y0 + y1) / 2 - 105) <= 0.5,
                  f"the board's centre on the A4 page's ({(x0 + x1) / 2:.2f}, {(y0 + y1) / 2:.2f})")
        g = re.search(r"by (-?[\d.]+), (-?[\d.]+) mm", t)
        check(g and float(g.group(1)) == round(float(g.group(1))) and float(g.group(2)) == round(float(g.group(2))),
              "the shift is whole mm: " + (g.group(0) if g else ""))
        err, t = m.tool("board_transform", {"center_on_sheet": True})
        check("nothing to move" in t, "again: nothing to move")
        m.tool("undo", {})
        check(part_at(m, "U1") == p0, "undo: back")

        print("[fields]")
        err, t = m.tool("pcb_set_fields", {"parts": {"R1": {"Value": {"layer": "silk", "at": [0, 1.5], "size": 0.8,
                                                                       "justify": "left"}}}})
        check(not err and "set fields of 1" in t, "R1's value onto the silkscreen: " + t.strip()[:100])
        err, t = m.tool("pcb_set_fields", {"refs": ["R2", "R3"], "set": {"silk": "value_only"}})
        check(not err, "R2, R3 silk=value_only")
        err, t = m.tool("pcb_set_fields", {"parts": {"R4": {"Value": {"text": "22k", "visible": False}}}})
        check(not err, "R4: text and hidden in one")
        err, t = m.tool("pcb_set_fields", {"parts": {"R1": {"Value": {"layer": "Nowhere"}}}})
        check(err and "no layer Nowhere" in t, "a bad layer: refused: " + t.strip()[:100])
        err, t = m.tool("pcb_set_fields", {"parts": {"R1": {"MPN": {"visible": True}}}})
        check(err and "no field MPN" in t, "a layout for a field it has not: refused")
        err, t = m.tool("pcb_set_fields", {"parts": {"R1": {"MPN": {"text": "RC0603", "layer": "fab", "visible": True}}}})
        check(not err, "a new field with its layout")
        m.tool("save_board", {})
        pcb = open(board, encoding="utf-8").read()
        v = prop(pcb, "R1", "Value")
        check(v and v[1] == "0 1.5 0" and v[2] == "F.SilkS" and v[4] == 0.8 and v[5] == 0.12 and v[6] == "left",
              f"R1 Value in the file: {v}")
        r2r, r2v = prop(pcb, "R2", "Reference"), prop(pcb, "R2", "Value")
        check(r2r and r2v and r2r[2] == "F.Fab" and not r2r[3] and r2v[2] == "F.SilkS" and not r2v[3],
              f"R2: Reference on F.Fab, Value on F.SilkS, both shown ({r2r and r2r[2]}, {r2v and r2v[2]})")
        r4 = prop(pcb, "R4", "Value")
        check(r4 and r4[0] == "22k" and r4[3], f"R4 Value 22k hidden: {r4}")
        mpn = prop(pcb, "R1", "MPN")
        check(mpn and mpn[2] == "F.Fab" and not mpn[3], f"R1 MPN on F.Fab, shown: {mpn}")
        for _ in range(4):
            err, t = m.tool("undo", {})
            print("    undo:", t.strip().replace("\n", " | ")[:300])
        m.tool("save_board", {})
        pcb = open(board, encoding="utf-8").read()
        v = prop(pcb, "R1", "Value")
        check(v and v[1] == "5.08 2.37 0" and v[2] == "F.Fab" and v[6] is None, f"undo x4: R1's value as it was: {v}")
        check(prop(pcb, "R2", "Reference")[2] == "F.SilkS" and prop(pcb, "R1", "MPN") is None,
              f"R2 and the MPN too: {prop(pcb, 'R2', 'Reference')}, {prop(pcb, 'R1', 'MPN')}")
        err, t = m.tool("pcb_set_fields", {"parts": {"R1": {"Value": {"layer": "silk", "at": [0, 1.5], "size": 0.8}}}})
        err, t = m.tool("sync_pcb", {})
        check(not err, "sync_pcb with R1's value laid out: " + t.splitlines()[0][:80])
        m.tool("save_board", {})
        v = prop(open(board, encoding="utf-8").read(), "R1", "Value")
        check(v and v[1] == "0 1.5 0" and v[2] == "F.SilkS" and v[4] == 0.8, f"the sync kept the layout: {v}")
        for _ in range(2 if "undo takes all of it back" in t else 1):
            m.tool("undo", {})

        print("[text]")
        err, t = m.tool("pcb_text", {})
        n0 = len(re.findall(r"^  [0-9a-f]{8} ", t, re.M))
        check(not err, f"list: {n0} board text(s) on the demo")
        err, t = m.tool("pcb_text", {"add": [{"text": "REV ${REVISION}", "at": [120, 60], "size": 1.5},
                                             {"text": "BOTTOM", "at": [120, 64], "layer": "B.SilkS", "justify": "left"}]})
        print("   ", t.replace("\n", "\n    ").rstrip())
        ids = re.findall(r"^  ([0-9a-f]{8}) ", t, re.M)
        check(not err and len(ids) == 2 and "board texts: 2 added" in t, "add two")
        check('"REV ${REVISION}"' in t, "${REVISION} kept as written")
        check(re.search(r'"BOTTOM" @120,64 B\.Silkscreen \S+ left mirrored', t) or
              re.search(r'"BOTTOM" @120,64 B\.SilkS \S+ left mirrored', t), "the back one mirrored, left justified")
        check(re.search(r'"REV \$\{REVISION\}"[^\n]* 1\.5/0\.225', t), "size 1.5, thickness 0.15 x size")
        err, t = m.tool("pcb_text", {})
        check(len(re.findall(r"^  [0-9a-f]{8} ", t, re.M)) == n0 + 2, "listed")
        err, t = m.tool("pcb_text", {"edit": [{"id": ids[0], "text": "REV B", "by": [5, 0], "angle": 90}]})
        check(not err and re.search(r'"REV B" @125,60 \S+ \S+ 90deg', t), "edit: text, moved by 5, turned: " + t.strip()[:160])
        err, t = m.tool("pcb_text", {"edit": [{"match": "BOTTOM", "layer": "F.SilkS"}]})
        check(not err and re.search(r'"BOTTOM" @120,64 F\.\S+ \S+ left$', t, re.M), "to the front: no longer mirrored: "
              + t.strip()[-80:])
        err, t = m.tool("pcb_text", {"edit": [{"match": "NOPE", "size": 2}]})
        check(err and "not found" in t and "NOPE" in t, "an unknown match: said")
        err, t = m.tool("pcb_text", {"edit": [{"id": ids[0], "size": 500}]})
        check(err and "size" in t, "a bad size: refused, nothing changed")
        err, t = m.tool("undo", {})
        err, t = m.tool("pcb_text", {})
        check('"BOTTOM" @120,64 B.' in t, "undo: the layer change taken back (mirrored again)")
        m.tool("undo", {})
        err, t = m.tool("pcb_text", {})
        check('"REV ${REVISION}" ' in t and "@120,60" in t, "undo: the first edit taken back")
        err, t = m.tool("pcb_text", {"remove": [ids[0], {"match": "BOTTOM"}]})
        check(not err and t.count("\n  removed ") == 2, "remove by id and by match")
        err, t = m.tool("pcb_text", {})
        check(len(re.findall(r"^  [0-9a-f]{8} ", t, re.M)) == n0, "gone from the list")
        m.tool("undo", {})
        err, t = m.tool("pcb_text", {})
        check(len(re.findall(r"^  [0-9a-f]{8} ", t, re.M)) == n0 + 2, "undo: both back")
        err, t = m.tool("save_board", {})
        pcb = open(board, encoding="utf-8").read()
        check("REV ${REVISION}" in pcb and "BOTTOM" in pcb, "saved into the .kicad_pcb")
        err, t = m.tool("board_transform", {"dx": 1, "dy": 1})
        err, t = m.tool("pcb_text", {})
        check("@121,61" in t, "texts move with board_transform")

        print("[reload]")
        text = open(board, encoding="utf-8").read()
        open(board, "w", encoding="utf-8").write(text.replace('"BOTTOM"', '"EDITED ON DISK"'))
        err, t = m.tool("project", {"reload": True})
        check(err and "unsaved changes" in t, "reload with the unsaved move: refused: " + t.strip()[:90])
        err, t = m.tool("project", {"reload": True, "discard": True})
        check(not err and t.startswith("reloaded pic_programmer.kicad_pcb from disk"), "discard=true: reloaded: "
              + t.splitlines()[0][:100])
        err, t = m.tool("pcb_text", {})
        check('"EDITED ON DISK" @120,64' in t, "the file's edit is on the board (and the move is gone)")
        err, t = m.tool("undo", {})
        check("nothing to undo" in t.lower() or "no " in t.lower()[:40], "the history starts over: " + t.strip()[:80])
        err, t = m.tool("project", {"reload": True})
        check(not err, "reload with nothing unsaved: no discard needed")

        print("[units]")
        ecc = os.path.join(work, "ecc83")
        shutil.copytree(os.path.join(a.demos, "ecc83"), ecc)
        err, t = m.tool("project", {"open": os.path.join(ecc, "ecc83-pp.kicad_pcb")})
        check(not err, "ecc83 opened (a 3-unit ECC83 as U1)")
        err, t = m.tool("sch_add_part", {"lib_id": "ecc83-pp:ECC83", "ref": "U1"})
        check(err and "all its 3 units placed" in t, "U1 again: all 3 units are placed: " + t.strip()[:120])
        err, t = m.tool("sch_add_part", {"lib_id": "ecc83-pp:ECC83", "ref": "U2"})
        check(not err, "U2: a new tube (unit 1)")
        err, t = m.tool("sch_add_part", {"lib_id": "ecc83-pp:ECC83", "ref": "U2"})
        check(not err and "unit 2 of U2" in t, "U2 again: its unit 2: " + next((ln for ln in t.splitlines() if "unit" in ln), t[:120]))
        err, t = m.tool("sch_add_part", {"lib_id": "ecc83-pp:ECC83", "ref": "U2", "unit": 2})
        check(err and "placed already (units placed: 1, 2 of 3)" in t, "unit 2 asked again: refused, with the units placed")
        err, t = m.tool("sch_add_part", {"lib_id": "ecc83-pp:ECC83", "ref": "U2", "unit": 3})
        check(not err and "unit 3 of U2" in t, "unit 3 asked: placed")
        err, t = m.tool("sch_add_part", {"lib_id": "ecc83-pp:R", "ref": "U2"})
        check(err and "already exists (ecc83-pp:ECC83)" in t, "another symbol on U2: refused")
        err, t = m.tool("sch_set_fields", {"parts": {"U2": {"Reference": "U1"}}})
        check(err and "another part is U1" in t, "renumber onto a taken reference: refused")
        err, t = m.tool("sch_set_fields", {"parts": {"U2": {"Reference": "U7", "Value": "12AX7"}}})
        check(not err and "U2 -> U7 (3 units)" in t, "sch_set_fields Reference: U2 -> U7, all 3 units: " + t.strip()[:160])
        err, t = m.tool("sch_part", {"ref": "U7"})
        check(not err and "12AX7" in t, "U7 is there with its value")
        err, t = m.tool("sch_part", {"ref": "U2"})
        check(err, "U2 is gone")
        err, t = m.tool("undo", {"target": "schematic"})
        err, t = m.tool("sch_part", {"ref": "U2"})
        check(not err, "undo: U2 again")
    finally:
        m.close()
        server.stop()

    print("\nRESULT:", "PASS" if not FAILS else f"FAIL ({len(FAILS)})")
    return 1 if FAILS else 0


if __name__ == "__main__":
    sys.exit(main())
