"""export_fab on pic_programmer (headless): the Gerbers and drill files are the same as
`kicad-cli pcb export gerbers / drill` writes, the zip holds them, BOM / CPL in JLCPCB's columns,
and an unsaved move shows in the CPL (the board in memory, not the file).

usage: python3 -I fab_e2e.py --build BUILD --demos DEMOS_DIR --work DIR """
import argparse, csv, os, re, shutil, subprocess, sys, zipfile
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from m5_e2e import Mcp, Server
ap = argparse.ArgumentParser(); ap.add_argument("--build", required=True); ap.add_argument("--demos", required=True)
ap.add_argument("--work", required=True)
a = ap.parse_args()
build, work = os.path.abspath(a.build), os.path.abspath(a.work)
shutil.rmtree(work, ignore_errors=True)
shutil.copytree(os.path.join(a.demos, "pic_programmer"), os.path.join(work, "p"))
pcb = os.path.join(work, "p", "pic_programmer.kicad_pcb")
sock = os.path.join(work, "s.sock"); env = dict(os.environ, KICAD_RUN_FROM_BUILD_DIR="1")
fails = []


def check(c, label):
    print(("  ok   " if c else "  FAIL ") + label); c or fails.append(label)


def body(path):
    return [l for l in open(path, errors="replace").read().splitlines()
            if "CreationDate" not in l and not l.startswith(("G04", ";"))]


def png_pixels(data):
    """(width, height, rows of (r, g, b, a)) of an 8-bit RGBA / RGB PNG (what cairo writes)."""
    import struct, zlib
    assert data[:8] == b"\x89PNG\r\n\x1a\n"
    pos, idat, w = 8, b"", 0
    while pos < len(data):
        n, kind = struct.unpack(">I4s", data[pos:pos + 8]); body = data[pos + 8:pos + 8 + n]; pos += 12 + n
        if kind == b"IHDR":
            w, h, depth, ctype = struct.unpack(">IIBB", body[:10])
        elif kind == b"IDAT":
            idat += body
    bpp = 4 if ctype == 6 else 3
    raw, rows, prev, stride = zlib.decompress(idat), [], bytearray(w * bpp), w * bpp
    for y in range(h):
        f, line = raw[y * (stride + 1)], bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        for i in range(stride):
            a = line[i - bpp] if i >= bpp else 0; b = prev[i]; c = prev[i - bpp] if i >= bpp else 0
            if f == 1: line[i] = (line[i] + a) & 255
            elif f == 2: line[i] = (line[i] + b) & 255
            elif f == 3: line[i] = (line[i] + (a + b) // 2) & 255
            elif f == 4:
                pa, pb, pc = abs(b - c), abs(a - c), abs(a + b - 2 * c)
                line[i] = (line[i] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        rows.append([tuple(line[i:i + bpp]) for i in range(0, stride, bpp)]); prev = line
    return w, h, rows


# the reference: kicad-cli on the file as it is
cli = os.path.join(work, "cli"); os.makedirs(cli)
cmd = os.path.join(build, "kicad", "kicad-cli")
subprocess.run([cmd, "pcb", "export", "gerbers", "--layers", "F.Cu,B.Cu,F.Mask,F.Silkscreen,Edge.Cuts", "--subtract-soldermask", "-o", cli + "/", pcb],
               env=env, capture_output=True)
subprocess.run([cmd, "pcb", "export", "drill", "--format", "excellon", "--excellon-units", "mm", "--excellon-separate-th",
                "-o", cli + "/", pcb], env=env, capture_output=True)

srv = Server(build, sock, pcb, env, os.path.join(work, "server.log")); assert srv.start()
try:
    m = Mcp([os.path.join(build, "kicadd-mcp"), "--socket", sock, "--stdio"], env)
    print("[export_fab]")
    err, t = m.tool("export_fab", {"refill": False})     # the fills as saved, as kicad-cli plots them
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
    err, t = m.tool("export_fab")
    check(t.startswith("all zones refilled"), "refills the zones by default")
    rows = {r[0]: r for r in csv.reader(open(os.path.join(fab, "pic_programmer-cpl.csv"), encoding="utf-8"))}
    check(abs(float(rows["U1"][1].rstrip("mm")) - x0 - 5) < 1e-3, f"CPL U1 x {x0} -> {rows['U1'][1]}")

    print("[DRC before the export]")
    first = t.splitlines()[1] if len(t.splitlines()) > 1 else ""
    print("      ", first)
    check(re.match(r"warning: DRC: \d+ error\(s\) \(.*\): the board would be made with them", first) is not None,
          "the moved U1's DRC errors come right after the refill")
    cplf = os.path.join(fab, "pic_programmer-cpl.csv"); before = os.path.getmtime(cplf)
    err, t = m.tool("export_fab", {"strict": True, "refill": False})
    print("      ", " | ".join(t.splitlines()[:3])[:200])
    check(err and "nothing written (strict)" in t and "- DRC:" in t and os.path.getmtime(cplf) == before,
          "strict: refused, the files left as they were")
    err, t = m.tool("export_fab", {"drc": False, "refill": False})
    check(not err and "DRC" not in t, "drc=false: no DRC")
    m.tool("undo")
    err, t = m.tool("export_fab", {"strict": True})
    print("      ", " | ".join(t.splitlines()[:3])[:200])
    check(not err and "\nDRC: no errors" in t, "U1 back: strict writes the files")
    m.tool("redo")

    print("[render_board]")
    import base64
    def shot(args):
        r = m.rpc("tools/call", {"name": "render_board", "arguments": args})["result"]
        text = "".join(c.get("text", "") for c in r["content"])
        img = [base64.b64decode(c["data"]) for c in r["content"] if c["type"] == "image"]
        return r.get("isError", False), text, (img[0] if img else b"")
    err, t, png = shot({})
    print("      ", t.splitlines()[0])
    w, h, rows = png_pixels(png)
    dims = re.match(r"picture (\d+)x(\d+) px, ([\d.]+) um/px; x ([\d.]+) \.\. ([\d.]+), y ([\d.]+) \.\. ([\d.]+) mm", t)
    check(not err and dims and (int(dims[1]), int(dims[2])) == (w, h) and 1100 <= max(w, h) <= 1300,
          f"the whole board: a {w}x{h} PNG, the size the text says")
    check("layers, bottom first: bottom_layer top_layer F.Silkscreen Edge.Cuts" in t, "front: copper, front silk, outline")
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
    err, t, png = shot({"layers": ["Nope"]})
    check(err and "no layer to draw" in t, "no layer at all: refused")

    print("[audit]")
    err, t = m.tool("audit")
    print("      ", t.splitlines()[0])
    check(not err and "U2.14: nearest decoupling cap C1 is 33.28 mm away" in t, "far decoupling cap measured on the board")
    check("(14, 14" not in t, "a multi-unit part's shared power pin counted once")
    check("J1 (DB9-FEMAL): 4 net(s) without a TVS / ESD part" in t, "the DB9's nets have no ESD part")
    check(re.search(r"not checked .*P2", t) is not None and "P2 (SUPP28)" not in t, "the ZIF sockets are not treated as cable connectors")
    check(re.search(r"\b4 connectors?\b", t) is not None and "P101" not in t, "the mounting holes P101-P106 are not connectors")

    print("[pcb_set_fields]")
    def bom_of():
        m.tool("export_fab", {"refill": False})
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
    err, t = m.tool("check_drc", {"refs": ["R1", "R100"]})
    check(t.startswith("not found: R100\n"), "R1 is R1 again")
    m.tool("undo"); m.tool("undo")
    bom = bom_of()
    r1 = next((r for r in bom if "R1" in r[1].split(",")), None)
    check(r1 is not None and r1[3] == "" and any("R2" in r[1].split(",") for r in bom), "two undo: R1 without LCSC, R2 back in the BOM")
    err, t = m.tool("redo")
    check(not err and "set fields" in t, "redo: " + t.splitlines()[0])
    m.close()
finally:
    srv.stop()
print("\nRESULT:", "PASS" if not fails else f"FAIL ({len(fails)})")
sys.exit(1 if fails else 0)
