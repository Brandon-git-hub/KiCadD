"""3D models of LCSC parts (headless, offline from the cache): EasyEDA's OBJ converted to VRML in
<project>/lcsc.3dshapes, placed as EasyEDA places it.

usage: python3 -I model_e2e.py --build BUILD --kicad KICAD_SRC --work DIR --cache LCSC_CACHE

The cache needs the parts (C165948, C5446, C6186, C20917, C8734) and the OBJs of the first four
(3d-<uuid>.obj); it is copied, so nothing of it changes. Checked in the written .wrl (KiCad's
VRML frame: 2.54 mm units, y up):
  C165948  USB-C HRO TYPE-C-31-M-12 (turned 180, outline 1.1 mm off the anchor, z -0.85): the
           shell front at the footprint's outline (y 5.05 down), signal leads at the back on the
           board (z 0), legs 0.85 mm through it
  C5446    SOT-23 turned 90: its two leads on pads 1 / 2 (+x, y -0.95 / 0.95), one on pad 3 (-x)
  C6186    SOT-223 turned 180: three leads at +x (y -2.3, 0, 2.3), the tab at -x (its edges at y -1.5 / 1.5); centred where
           EasyEDA has it (0.1 mm left of the pads)
  C20917   SOT-23 whose c_origin is not in the footprint's frame: centred on the footprint anyway
  C8734    its OBJ not cached (offline): no model, said; the footprint references none
  model=false: no model; add_footprint lcsc=: the board's footprint has the model, saved
"""

import argparse
import os
import re
import shutil
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from m5_e2e import FAILS, Mcp, Server, binaries, check, make_project  # noqa: E402


def points(path):
    """[(x, y_down, z)] mm from a .wrl"""
    s = open(path, encoding="utf-8").read()
    return [(float(a) * 2.54, -float(b) * 2.54, float(c) * 2.54)
            for a, b, c in re.findall(r"^(-?[\d.e+-]+) (-?[\d.e+-]+) (-?[\d.e+-]+),$", s, re.M)]


def leads(pts):
    """the lowest vertices at the +x / -x ends: their y positions (0.5 mm steps)"""
    zmin = min(p[2] for p in pts)
    low = [p for p in pts if p[2] < zmin + 0.05]
    xmax = max(abs(p[0]) for p in pts)
    def side(s):
        ys = sorted(p[1] for p in low if s * p[0] > xmax - 0.4)
        groups = []
        for y in ys:                       # a lead: its corners, less than 0.8 mm apart
            if groups and y - groups[-1][-1] < 0.8:
                groups[-1].append(y)
            else:
                groups.append([y])
        return [round((g[0] + g[-1]) / 2, 1) for g in groups]
    return side(1), side(-1)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--build", required=True)
    ap.add_argument("--kicad", required=True)
    ap.add_argument("--work", required=True)
    ap.add_argument("--cache", required=True)
    a = ap.parse_args()

    work = os.path.abspath(a.work)
    if os.path.exists(work):
        shutil.rmtree(work)
    os.makedirs(work)
    cache = os.path.join(work, "cache")
    shutil.copytree(a.cache, cache)
    for f in os.listdir(cache):     # C8734's model must be missing
        if f.startswith("3d-a4b96ad8"):
            os.remove(os.path.join(cache, f))

    proj = make_project(a.kicad, work)
    pcb = os.path.join(proj, "blank.kicad_pcb")
    sock = os.path.join(work, "s.sock")
    kicad_cli, kicadd_mcp, env = binaries(a.build)
    env = dict(env, KICADD_LCSC_CACHE=cache, KICADD_LCSC_OFFLINE="1")
    server = Server(a.build, sock, pcb, env, os.path.join(work, "server.log"))
    check(server.start(), "api-server up with the blank board")
    mcp = Mcp([kicadd_mcp, "--socket", sock, "--stdio"], env)
    shapes = os.path.join(proj, "lcsc.3dshapes")
    try:
        print("[USB-C C165948]")
        err, t = mcp.tool("lcsc_fetch", {"lcsc": "C165948"})
        line = next((ln.strip() for ln in t.splitlines() if "3D model" in ln), "")
        print("      ", line)
        check(not err and line.startswith("3D model lcsc.3dshapes/USB-C_SMD-TYPE-C-31-M-12_1.wrl (EasyEDA's, 8.9 x 7.9 x 4.1 mm"),
              "lcsc_fetch says where the model went")
        wrl = os.path.join(shapes, "USB-C_SMD-TYPE-C-31-M-12_1.wrl")
        check(os.path.isfile(wrl) and open(wrl, encoding="utf-8").readline().startswith("#VRML V2.0 utf8"), "a VRML 2.0 file")
        mod = open(os.path.join(proj, "lcsc.pretty", "USB-C_SMD-TYPE-C-31-M-12_1.kicad_mod"), encoding="utf-8").read()
        check('(model "${KIPRJMOD}/lcsc.3dshapes/USB-C_SMD-TYPE-C-31-M-12_1.wrl"' in mod, "the footprint references it")
        p = points(wrl)
        ys, zs = [q[1] for q in p], [q[2] for q in p]
        check(abs(max(ys) - 5.05) < 0.05 and abs(min(ys) + 2.85) < 0.05, f"shell front at the outline: y {min(ys):.2f}..{max(ys):.2f}")
        check(abs(min(zs) + 0.85) < 0.01 and abs(max(zs) - 3.25) < 0.01, f"z {min(zs):.2f}..{max(zs):.2f} (legs through the board)")
        back = [q for q in p if q[1] < -2.4 and abs(q[0]) < 3.4]
        check(back and abs(min(q[2] for q in back)) < 0.01, "the signal leads at the back sit on the board")

        print("[SOT-23 / SOT-223]")
        # the model's centre: EasyEDA's own (its 3D outline): C6186's is 0.1 mm left of the pads
        for lcsc, name, want, cx in (("C5446", "SOT-23-3_L2.9-W1.6-P1.90-LS2.8-BR", ([-0.95, 0.95], [0.0]), 0),
                                     ("C6186", "SOT-223-3_L6.5-W3.4-P2.30-LS7.0-BR", ([-2.3, 0.0, 2.3], [-1.5, 1.5]), -0.1),
                                     ("C20917", "SOT-23-3_L2.9-W1.3-P1.90-LS2.4-BR", ([-0.95, 0.95], [0.0]), 0)):
            err, t = mcp.tool("lcsc_fetch", {"lcsc": lcsc})
            path = os.path.join(shapes, name + ".wrl")
            check(not err and os.path.isfile(path), f"{lcsc}: {name}.wrl")
            if os.path.isfile(path):
                p = points(path)
                got = leads(p)
                same = all(len(g) == len(w) and all(abs(x - y) < 0.1 for x, y in zip(g, w)) for g, w in zip(got, want))
                check(same, f"{lcsc}: leads at +x {got[0]}, at -x {got[1]}")
                xs, ys = [q[0] for q in p], [q[1] for q in p]
                check(abs((max(xs) + min(xs)) / 2 - cx) < 0.01 and abs(max(ys) + min(ys)) < 0.02,
                      f"{lcsc}: centre x {(max(xs) + min(xs)) / 2:.2f} (EasyEDA's {cx}), y {(max(ys) + min(ys)) / 2:.2f}")

        print("[no model]")
        err, t = mcp.tool("lcsc_fetch", {"lcsc": "C8734"})
        check(not err and "3D model not in the cache (KICADD_LCSC_OFFLINE)" in t, "C8734 offline without its OBJ: said")
        mod = open(os.path.join(proj, "lcsc.pretty", "LQFP-48_L7.0-W7.0-P0.50-LS9.0-BL.kicad_mod"), encoding="utf-8").read()
        check("(model" not in mod, "and the footprint references no model")
        os.remove(os.path.join(shapes, "SOT-23-3_L2.9-W1.6-P1.90-LS2.8-BR.wrl"))
        err, t = mcp.tool("lcsc_fetch", {"lcsc": "C5446", "model": False})
        check(not err and "3D model" not in t and not os.path.exists(os.path.join(shapes, "SOT-23-3_L2.9-W1.6-P1.90-LS2.8-BR.wrl")),
              "model=false: none")

        print("[on the board]")
        err, t = mcp.tool("add_footprint", {"lcsc": "C6186", "ref": "U1", "x": 100, "y": 100})
        check(not err and t.startswith("U1 added"), "U1 from LCSC C6186")
        mcp.tool("save_board")
        board = open(pcb, encoding="utf-8").read()
        check('(model "${KIPRJMOD}/lcsc.3dshapes/SOT-223-3_L6.5-W3.4-P2.30-LS7.0-BR.wrl"' in board, "the saved board's U1 has the model")
    finally:
        mcp.close()
        server.stop()

    print("\nRESULT:", "PASS" if not FAILS else f"FAIL ({len(FAILS)})")
    sys.exit(1 if FAILS else 0)


if __name__ == "__main__":
    main()
