"""export_fab on pic_programmer (headless): the Gerbers and drill files are the same as
`kicad-cli pcb export gerbers / drill` writes, the zip holds them, BOM / CPL in JLCPCB's columns,
and an unsaved move shows in the CPL (the board in memory, not the file); export_assembly
writes the assembly drawing (a page per side with parts, DNP crossed out).

usage: python3 -I fab_e2e.py --build BUILD --demos DEMOS_DIR --work DIR

BUILD: a build tree, or an installed folder (the Windows zip's bin/). """
import argparse, csv, os, re, shutil, subprocess, sys, zipfile
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from m5_e2e import Mcp, Server, binaries, png_pixels
ap = argparse.ArgumentParser(); ap.add_argument("--build", required=True); ap.add_argument("--demos", required=True)
ap.add_argument("--work", required=True)
a = ap.parse_args()
build, work = os.path.abspath(a.build), os.path.abspath(a.work)
shutil.rmtree(work, ignore_errors=True)
shutil.copytree(os.path.join(a.demos, "pic_programmer"), os.path.join(work, "p"))
pcb = os.path.join(work, "p", "pic_programmer.kicad_pcb")
sock = os.path.join(work, "s.sock"); kicad_cli, kicadd_mcp, env = binaries(build)
fails = []


def check(c, label):
    print(("  ok   " if c else "  FAIL ") + label); c or fails.append(label)


def body(path):
    return [l for l in open(path, errors="replace").read().splitlines()
            if "CreationDate" not in l and not l.startswith(("G04", ";"))]


# the reference: kicad-cli on the file as it is
cli = os.path.join(work, "cli"); os.makedirs(cli)
cmd = kicad_cli
subprocess.run([cmd, "pcb", "export", "gerbers", "--layers", "F.Cu,B.Cu,F.Mask,F.Silkscreen,Edge.Cuts", "--subtract-soldermask", "-o", cli + "/", pcb],
               env=env, capture_output=True)
subprocess.run([cmd, "pcb", "export", "drill", "--format", "excellon", "--excellon-units", "mm", "--excellon-separate-th",
                "-o", cli + "/", pcb], env=env, capture_output=True)

srv = Server(build, sock, pcb, env, os.path.join(work, "server.log")); assert srv.start()
try:
    m = Mcp([kicadd_mcp, "--socket", sock, "--stdio"], env)
    print("[export_fab]")
    err, t = m.tool("export", {"fab": {"refill": False}})     # the fills as saved, as kicad-cli plots them
    print("      ", " | ".join(t.splitlines()[1:6]))
    fab = os.path.join(work, "p", "fab")
    check(not err and "9 layer file(s)" in t and "56 placements" in t, "9 Gerbers, 56 placements (63 parts, 7 not in the BOM)")
    for name in ("top_layer.gtl", "bottom_layer.gbl", "F_Mask.gts", "F_Silkscreen.gto", "Edge_Cuts.gm1", "PTH.drl", "NPTH.drl"):
        ours, ref = os.path.join(fab, "pic_programmer-" + name), os.path.join(cli, "pic_programmer-" + name)
        same = os.path.exists(ref) and body(ours) == body(ref)
        if not same:
            shutil.copy(ours, os.path.join(work, "diff-" + name))
        check(same, f"{name} same as kicad-cli")
    z = zipfile.ZipFile(os.path.join(fab, "pic_programmer-gerbers.zip")).namelist()
    check(len(z) == 12 and "pic_programmer-job.gbrjob" in z and "pic_programmer-PTH.drl" in z, f"zip: {len(z)} files")
    bom = list(csv.reader(open(os.path.join(fab, "pic_programmer-bom.csv"), encoding="utf-8")))
    check(bom[0] == ["Comment", "Designator", "Footprint", "LCSC Part #"], f"BOM header {bom[0]}")
    check(any(r[1] == "C6,C7" and r[0] == "100nF" for r in bom), "equal parts on one line (C6,C7 100nF)")
    rows = {r[0]: r for r in csv.reader(open(os.path.join(fab, "pic_programmer-cpl.csv"), encoding="utf-8"))}
    check(rows["Designator"] == ["Designator", "Mid X", "Mid Y", "Layer", "Rotation"], "CPL header")
    bom_refs = {x for r in bom[1:] for x in r[1].split(",")}
    check("P101" not in rows and "JP1" not in rows and set(rows) - {"Designator"} <= bom_refs,
          f"CPL only has parts in the BOM ({len(rows) - 1} rows, {len(bom_refs)} BOM refs)")
    x0 = float(rows["U1"][1].rstrip("mm"))

    print("[an unsaved move]")
    m.tool("move_footprints", {"refs": ["U1"], "dx": 5, "dy": 0})
    err, t = m.tool("export", {"fab": {}})
    check(t.startswith("all zones refilled"), "refills the zones by default")
    rows = {r[0]: r for r in csv.reader(open(os.path.join(fab, "pic_programmer-cpl.csv"), encoding="utf-8"))}
    check(abs(float(rows["U1"][1].rstrip("mm")) - x0 - 5) < 1e-3, f"CPL U1 x {x0} -> {rows['U1'][1]}")

    print("[DRC before the export]")
    first = t.splitlines()[1] if len(t.splitlines()) > 1 else ""
    print("      ", first)
    check(re.match(r"warning: DRC: \d+ error\(s\) \(.*\): the board would be made with them", first) is not None,
          "the moved U1's DRC errors come right after the refill")
    cplf = os.path.join(fab, "pic_programmer-cpl.csv"); before = os.path.getmtime(cplf)
    err, t = m.tool("export", {"fab": {"strict": True, "refill": False}})
    print("      ", " | ".join(t.splitlines()[:3])[:200])
    check(err and "nothing written (strict)" in t and "- DRC:" in t and os.path.getmtime(cplf) == before,
          "strict: refused, the files left as they were")
    err, t = m.tool("export", {"fab": {"drc": False, "refill": False}})
    check(not err and "DRC" not in t, "drc=false: no DRC")
    m.tool("undo")
    err, t = m.tool("export", {"fab": {"strict": True}})
    print("      ", " | ".join(t.splitlines()[:3])[:200])
    check(not err and "\nDRC: no errors" in t, "U1 back: strict writes the files")
    m.tool("undo", {"redo": True})

    print("[board_render]")
    import base64
    def shot(args):
        r = m.rpc("tools/call", {"name": "board_render", "arguments": args})["result"]
        text = "".join(c.get("text", "") for c in r["content"])
        img = [base64.b64decode(c["data"]) for c in r["content"] if c["type"] == "image"]
        return r.get("isError", False), text, (img[0] if img else b"")
    err, t, png = shot({})
    print("      ", t.splitlines()[0])
    w, h, rows = png_pixels(png)
    dims = re.match(r"picture (\d+)x(\d+) px, ([\d.]+) um/px; x ([\d.]+) \.\. ([\d.]+), y ([\d.]+) \.\. ([\d.]+) mm", t)
    check(not err and dims and (int(dims[1]), int(dims[2])) == (w, h) and 1100 <= max(w, h) <= 1300,
          f"the whole board: a {w}x{h} PNG, the size the text says")
    check("layers, bottom first: B.Cu (bottom_layer) F.Cu (top_layer) F.Silkscreen Edge.Cuts" in t, "front: copper, front silk, outline")
    um, x0, x1 = float(dims[3]), float(dims[4]), float(dims[5])
    check(abs((x1 - x0) * 1000 / um - (w - 34)) < 3, "x range / scale = the width without the ruler strip")
    err, t, png = shot({"refs": ["U1"], "nets": ["GND"], "size": 600})
    w, h, rows = png_pixels(png)
    magenta = sum(1 for row in rows for p in row if p[0] > 230 and p[1] < 90 and p[2] > 230)
    cyan = sum(1 for row in rows for p in row if p[0] < 110 and p[1] > 230 and p[2] > 230)
    dims = re.match(r"picture \d+x\d+ px, [\d.]+ um/px; x ([\d.]+) \.\. ([\d.]+), y ([\d.]+) \.\. ([\d.]+) mm", t)
    check(not err and 590 <= max(w, h) <= 600 and magenta > 200 and cyan > 500,
          f"U1 framed (magenta {magenta} px), GND in cyan ({cyan} px), {w}x{h}")
    check(float(dims[1]) < 179 < float(dims[2]), "zoomed to U1 and GND (U1 at x 179 is inside)")
    err, t, png = shot({"refs": ["U99"], "layers": ["F.Cu", "Nope"], "back": True, "zoom": False, "path": "pic.png"})
    check(not err and "not found: layer Nope U99" in t and "seen from below" in t, "unknown ref / layer listed; back: " + t.replace("\n", " | ")[-120:])
    check(os.path.exists(os.path.join(work, "p", "pic.png")) and open(os.path.join(work, "p", "pic.png"), "rb").read() == png,
          "path: saved in the project folder, the same PNG")
    err, t, png = shot({"layers": ["silk"], "zoom": False})
    check(not err and "layers, bottom first: F.Mask F.Silkscreen Edge.Cuts" in t, "silk: mask, silkscreen, outline, no copper")
    err, t, png = shot({"layers": ["Nope"]})
    check(err and "no layer to draw" in t, "no layer at all: refused")

    print("[sch_render]")
    def sshot(args):
        r = m.rpc("tools/call", {"name": "sch_render", "arguments": args})["result"]
        text = "".join(c.get("text", "") for c in r["content"])
        img = [base64.b64decode(c["data"]) for c in r["content"] if c["type"] == "image"]
        return r.get("isError", False), text, (img[0] if img else b"")
    err, t, png = sshot({})
    print("      ", t.splitlines()[0])
    w, h, rows = png_pixels(png)
    check(not err and t.startswith(f"picture of / {w}x{h} px") and max(w, h) == 1600, f"the root sheet: {w}x{h}")
    err, t, png = sshot({"refs": ["U1"], "size": 800})
    w, h, rows = png_pixels(png)
    magenta = sum(1 for row in rows for p in row if p[0] > 200 and p[1] < 60 and p[2] > 170)
    check(not err and t.startswith("picture of /pic_sockets/") and magenta > 300,
          f"U1: its sheet (/pic_sockets/), framed ({magenta} px): " + t.splitlines()[0][:60])
    err, t, png = sshot({"sheet": "pic_sockets", "nets": ["DATA-RB7"], "zoom": False, "size": 1000})
    w, h, rows = png_pixels(png)
    blue = sum(1 for row in rows for p in row if p[0] < 90 and 140 < p[1] < 215 and p[2] > 200)
    check(not err and blue > 1500, f"DATA-RB7 in blue on pic_sockets ({blue} px)")
    err, t, png = sshot({"refs": ["U2", "U99"], "nets": ["VPP_ON", "NOPE"], "sheet": "/"})
    check(not err and "not found: U99" in t and "net NOPE not on this sheet" in t, "unknown ref / net listed")
    err, t, png = sshot({"refs": ["U1"], "sheet": "/", "zoom": False})
    check(not err and "elsewhere: U1 on /pic_sockets/" in t, "a ref on another sheet: said where")
    err, t, png = sshot({"frame": True, "area": [0, 0, 100, 50], "path": "sch.png"})
    check(not err and "x 0 .. 100, y 0 .. 50 mm" in t and os.path.exists(os.path.join(work, "p", "sch.png")), "area; saved")
    err, t, png = sshot({"sheet": "pic_sockets", "frame": True, "path": "sub.png"})
    print("      ", t.splitlines()[0])
    check(not err and t.startswith("picture of /pic_sockets/") and "; x 0 .. " in t and ", y 0 .. " in t,
          "frame=true on a sub-sheet: the whole page " + t.splitlines()[0][:90])
    err, t, png = sshot({"sheet": "pic_sockets", "refs": ["U1"], "frame": True})
    check(not err and "; x 0 .. " in t, "frame=true with refs: still the whole page")
    err, t, png = sshot({"area": [2000, 2000, 2100, 2050]})
    check(not err and "nothing drawn in this area" in t, "an area with nothing in it: said so")
    err, t, png = sshot({"sheet": "nope"})
    check(err and "no sheet nope" in t, "an unknown sheet is refused")

    print("[unknown arguments, add_footprint like]")
    err, t = m.tool("board_render", {"zom": True})
    check(err and "unknown argument(s) zom" in t and "zoom" in t, "an unknown argument is an error, with the ones there are")
    err, t = m.tool("zones", {"create": {"net": "GND", "area": [100, 60, 120, 80]}})
    check(err and "unknown argument(s) create.area" in t and "in create" in t and "rect" in t, "create.area: " + t[:120])
    err, t = m.tool("zones", {"create": {"net": "GND", "layers": ["F.Cu"], "rect": [100, 60, 120, 80]}})
    check(not err and "new zone GND on F.Cu" in t, "create.rect: a rectangle")
    err, z = m.tool("zones")
    check("2 copper zone(s)" in z, "two zones now")
    m.tool("undo")
    err, s0 = m.tool("board_summary")
    nets0 = int(re.search(r"nets=(\d+)", s0).group(1))
    err, t = m.tool("add_footprint", {"like": "R1", "ref": "R30", "x": 100, "y": 30, "nets": {"1": "TESTNET1", "2": "TESTNET2"}})
    check(not err and "R30" in t, "add_footprint like R1 -> R30")
    err, s1 = m.tool("board_summary")
    check(int(re.search(r"nets=(\d+)", s1).group(1)) == nets0 + 2, "two new nets")
    err, t = m.tool("pcb_set_fields", {"parts": {"R30": {"Reference": "R31"}}})
    check(not err and "comes from the schematic" not in t, "the copy is not tied to R1's symbol: " + t.replace("\n", " | ")[:120])
    m.tool("undo")
    err, t = m.tool("undo")
    err, s2 = m.tool("board_summary")
    check(not err and int(re.search(r"nets=(\d+)", s2).group(1)) == nets0 and "footprints=63" in s2,
          "undo takes the part and the nets it made away: " + t.splitlines()[0])
    nets = lambda: int(re.search(r"nets=(\d+)", m.tool("board_summary")[1]).group(1))   # noqa: E731
    m.tool("add_footprint", {"like": "R1", "ref": "R30", "x": 100, "y": 30, "nets": {"1": "TESTNET1", "2": "VCC"}})
    err, t = m.tool("delete_footprints", {"refs": ["R30"]})
    check(not err and "nets left with nothing on them, removed: TESTNET1" in t and "VCC" not in t.split("removed:")[-1]
          and nets() == nets0, "delete: R30's own net goes with it, VCC stays: " + t.replace("\n", " | ")[:120])
    err, t = m.tool("undo")
    err, t2 = m.tool("query_net", {"net": "TESTNET1"})
    check(not err and nets() == nets0 + 1 and "R30" in t2, "undo: R30 back on TESTNET1: " + t2.strip().replace("\n", " | ")[:100])
    err, t = m.tool("undo", {"redo": True})
    check(not err and "removed: TESTNET1" in t and nets() == nets0, "redo: the net goes again")
    m.tool("undo")
    m.tool("undo")
    check(nets() == nets0 and "footprints=63" in m.tool("board_summary")[1], "undo the add: as before")

    print("[audit]")
    err, t = m.tool("audit")
    print("      ", t.splitlines()[0])
    check(not err and "U2.14: nearest decoupling cap C1 is 33.28 mm away" in t, "far decoupling cap measured on the board")
    check("(14, 14" not in t, "a multi-unit part's shared power pin counted once")
    check("J1 (DB9-FEMAL): 4 net(s) without a TVS / ESD part" in t, "the DB9's nets have no ESD part")
    check(re.search(r"not checked .*P2", t) is not None and "P2 (SUPP28)" not in t, "the ZIF sockets are not treated as cable connectors")
    check(re.search(r"\b4 connectors?\b", t) is not None and "P101" not in t, "the mounting holes P101-P106 are not connectors")
    check("35 um outer copper" in t and "no test points on the board" in t and "voltage:" not in t,
          "board half: copper, no test points, no voltage section without voltages")
    err, t = m.tool("audit", {"currents": {"VCC": 5, "VPP": 0.05, "NOPE": 1}, "voltages": {"/VPP_ON": 400}})
    check(not err and re.search(r"VCC \(5 A\): \d+ B\.Cu track segment\(s\) narrower than 2\.77 mm .*good for about 2\.03 A", t)
          and "VPP (0.05 A)" not in t and "no net NOPE on the board" in t, "currents: VCC 5 A too much for its 0.8 mm tracks, VPP fine")
    check("its net class POWER routes 0.8 mm tracks; 5 A wants 2.77 mm" in t, "currents: the class and the set_rules call")
    check(re.search(r"/VPP_ON \(400 V\) to /PC-DATA-OUT \(0 V, taken as 0\) on [FB]\.Cu: 1\.17 mm \(pad J1\.3 to pad J1\.4\).* wants 2\.5 mm for 400 V", t)
          and t.count("to /PC-DATA-OUT") == 1, "voltages: 400 V on the DB9, one line per pair of nets")
    err, t = m.tool("move_footprints", {"refs": ["R1"], "dx": -2.98, "dy": -3.226})
    err, t = m.tool("audit")
    check("silk:" in t and "R1 reference over pad R2.1" in t, "silk: a reference moved onto a pad")
    m.tool("undo")
    err, t = m.tool("set_rules", {"classes": {"Pow": {"current": 1.5}}, "drc": False})
    check(not err and "Pow: 1.5 A -> 0.53 mm tracks" in t and "inner" not in t, "set_rules current on 2 layers")
    m.tool("undo")

    print("[pcb_set_fields]")
    def bom_of():
        m.tool("export", {"fab": {"refill": False}})
        return list(csv.reader(open(os.path.join(fab, "pic_programmer-bom.csv"), encoding="utf-8")))
    err, t = m.tool("pcb_set_fields", {"parts": {"R1": {"LCSC": "C25804", "MPN": "RC0603FR-0710KL"}, "R2": {"dnp": True}}})
    check(not err and "set fields of 2 footprint(s)" in t, "set: " + t.splitlines()[0])
    bom = bom_of()
    r1 = next((r for r in bom if "R1" in r[1].split(",")), None)
    check(r1 is not None and r1[3] == "C25804", f"R1's LCSC number in the BOM: {r1}")
    check(not any("R2" in r[1].split(",") for r in bom), "R2 (DNP) not in the BOM")
    err, t = m.tool("pcb_set_fields", {"refs": ["R1"], "set": {"MPN": None}})
    check(not err, "remove a user field: " + t.splitlines()[0])
    err, t = m.tool("pcb_set_fields", {"parts": {"R1": {"Footprint": "Resistor_SMD:R_0603"}}})
    check(err and "not changed here" in t, "the footprint is refused")
    err, t = m.tool("pcb_set_fields", {"parts": {"R1": {"Reference": "R2"}}})
    check(err and "another part is R2 already" in t, "a reference in use is refused: " + t.strip()[:90])
    err, t = m.tool("pcb_set_fields", {"parts": {"R1": {"Reference": "R100"}}})
    check(not err and "R1 is now R100" in t, "R1 renumbered: " + t.splitlines()[0])
    check("note: R1 -> R100: this part comes from the schematic" in t and "sync_pcb puts that back" in t,
          "with a note that the schematic still has R1")
    err, q = m.tool("pcb_set_fields", {"parts": {"R100": {"Reference": "R1"}, "R2": {"Reference": "R100"}}})
    check(not err and "2 renumbered" in q, "two at once, one taking the other's old reference: " + q.splitlines()[0])
    m.tool("undo")
    err, t = m.tool("undo")
    check(not err and "R1 is now R100" in t, "undo: " + t.splitlines()[0])
    err, t = m.tool("run_drc", {"local": True, "refs": ["R1", "R100"]})
    check(t.startswith("not found: R100\n"), "R1 is R1 again")
    m.tool("undo"); m.tool("undo")
    bom = bom_of()
    r1 = next((r for r in bom if "R1" in r[1].split(",")), None)
    check(r1 is not None and r1[3] == "" and any("R2" in r[1].split(",") for r in bom), "two undo: R1 without LCSC, R2 back in the BOM")
    err, t = m.tool("undo", {"redo": True})
    check(not err and "set fields" in t, "redo: " + t.splitlines()[0])
    print("[board_diff]")
    err, t = m.tool("save_board")
    err, t = m.tool("board_diff")
    check(not err and t.startswith("no changes against ") and "pic_programmer.kicad_pcb" in t, "as loaded: " + t.splitlines()[0][:40])
    edits = [("move_footprints", {"refs": ["R4"], "dx": 2}), ("move_footprints", {"refs": ["C1"], "rotate": 90}),
             ("pcb_set_fields", {"parts": {"R5": {"LCSC": "C25804", "Value": "4k7"}}}),
             ("delete_tracks", {"nets": ["VPP"]}), ("add_footprint", {"like": "R3", "ref": "R40", "x": 120, "y": 60}),
             ("zones", {"create": {"net": "VCC", "rect": [120, 60, 150, 90]}})]
    for tool, args in edits:
        err, t = m.tool(tool, args); check(not err, f"edit: {tool}")
    err, t = m.tool("board_diff")
    print("      ", " | ".join(t.splitlines()[:3])[:200])
    check("footprints: 63 -> 64; 1 added, 0 removed, 3 changed" in t and "added R40 10K" in t
          and "C1: rotated 180 -> 270 deg" in t and re.search(r"R4: moved \+2, \+0 mm to \(", t)
          and re.search(r"R5: value \S+ -> 4k7; LCSC added \(C25804\)", t), "footprints: added, rotated, moved, value and field")
    check(re.search(r"VPP: 13 -> 0 segments, [\d.]+ -> 0 mm \(all tracks removed\)", t) and "13 removed" in t, "tracks: VPP's removed")
    check("added zone VCC on F.Cu+B.Cu" in t, "zones: the new one")
    err, t = m.tool("board_diff")
    err, tl = m.tool("board_diff", {"limit": 1})
    check("... 3 more (limit)" in tl, "limit")
    import uuid   # another revision: the same board with new UUIDs, paired by reference and geometry
    other = re.sub(r'\(uuid "[0-9a-f-]+"\)', lambda _: f'(uuid "{uuid.uuid4()}")', open(pcb, encoding="utf-8").read())
    open(os.path.join(work, "p", "other.kicad_pcb"), "w", encoding="utf-8").write(other)
    err, t2 = m.tool("board_diff", {"file": "other.kicad_pcb"})
    strip = lambda x: x.splitlines()[1:]
    check(not err and strip(t2) == strip(t), "another file with new UUIDs: the same answer (by reference and geometry)")
    for _ in edits:
        m.tool("undo")
    err, t = m.tool("board_diff")
    check(not err and "footprints: 63 -> 63; 0 added, 0 removed, 0 changed" in t and "zones: 1 -> 1; 0 added" in t
          and re.search(r"tracks: (\d+) -> \1 segments", t), "after undo: the same footprints, zones, tracks: " + " | ".join(t.splitlines()[:3])[:300])
    err, t = m.tool("board_diff", {"file": "nope.kicad_pcb"})
    check(err and "no file" in t, "a missing file is refused")
    print("[silk_tidy]")
    err, t = m.tool("audit")
    check(re.search(r"P104 reference at \([\d.]+, [\d.]+\) reaches past the board edge", t) is not None,
          "audit: the mounting hole's reference past the board edge")
    err, t = m.tool("silk_tidy", {"dry_run": True})
    print("      ", t.splitlines()[0])
    plan = t
    check(not err and t.startswith("would move ") and re.search(r"P104 reference \(229\.87, 39\.2\) -> \(229\.87, [\d.]+\), below", t)
          and re.search(r"texts with a problem (\d+) -> 0", t), "dry run: the mounting holes' references off the board go below them")
    err, t = m.tool("silk_tidy", {"refs": ["P104", "ZZ9"], "dry_run": True})
    check(t.startswith("would move 1 text(s) of 1 checked") and "ZZ9: no part" in t, "refs: only those; an unknown one said")
    err, t = m.tool("silk_tidy")
    check(not err and t.startswith("moved ") and "(undo takes it back)" in t and t.splitlines()[1:] == plan.splitlines()[1:],
          "applied as planned")
    check("(was off the board)" in plan, "the plan says why a text moves")
    err, t = m.tool("board_diff")
    check(not err and re.search(r"P104: reference moved [-+][\d.]+, [-+][\d.]+ mm on the part", t) is not None
          and re.search(r"\(\d+ texts\)", t) is not None, "board_diff lists the moved references: " + " | ".join(t.splitlines()[:4])[:300])
    err, t = m.tool("silk_tidy", {"dry_run": True})
    check(t.startswith("would move 0 text(s)") and "texts with a problem 0 -> 0" in t, "nothing left to move")
    err, t = m.tool("audit")
    check("reference over pad" not in t and "reference overlaps" not in t and "P104 reference at" not in t,
          "audit: no reference over a pad, on another or past the edge")
    err, t = m.tool("undo")
    check(not err and t.startswith("undid: silk tidy"), "undo: " + t.splitlines()[0])
    err, t = m.tool("silk_tidy", {"dry_run": True})
    check(t.splitlines()[1:] == plan.splitlines()[1:], "after undo: the texts are back where they were")

    print("[export_assembly]")
    def pages(path):
        return len(re.findall(rb"/Type\s*/Page(?![s\w])", open(path, "rb").read()))
    err, t = m.tool("export", {"assembly": {}})
    print("      ", t.strip())
    f = os.path.join(work, "p", "fab", "pic_programmer-assembly.pdf")
    nb = re.search(r"(\d+) on the bottom", t)
    nd = re.search(r"(\d+) DNP", t)
    nb, nd = int(nb.group(1)) if nb else -1, int(nd.group(1)) if nd else 0
    check(not err and os.path.isfile(f) and open(f, "rb").read(5) == b"%PDF-" and pages(f) == (2 if nb > 0 else 1),
          f"a PDF in <project>/fab, a page per side with parts ({pages(f) if os.path.isfile(f) else 0} pages, {nb} on the bottom)")
    m.tool("move_footprints", {"refs": ["R1"], "flip": True})
    m.tool("pcb_set_fields", {"parts": {"R3": {"dnp": True}}})
    err, t = m.tool("export", {"assembly": {"path": "asm.pdf", "values": True}})
    print("      ", t.strip())
    f2 = os.path.join(work, "p", "asm.pdf")
    check(not err and os.path.isfile(f2) and pages(f2) == 2 and f", {nb + 1} on the bottom" in t,
          "R1 flipped: one more on the bottom page; path relative to the project")
    check(f"{nd + 1} DNP crossed out" in t, "R3 set DNP: crossed out too")
    m.tool("undo"); m.tool("undo")
    m.close()
finally:
    srv.stop()
print("\nRESULT:", "PASS" if not fails else f"FAIL ({len(fails)})")
sys.exit(1 if fails else 0)
