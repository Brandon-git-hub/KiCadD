"""A board from nothing (headless): outline, holes, pours, a fab's rules, layers and stackup.

usage: python3 -I setup_e2e.py --build BUILD --kicad KICAD_SRC --work DIR

Blank project (m5_e2e's: a 60 x 40 mm outline, nothing else), two resistors placed by hand:
  board_outline  a rounded 50 x 30 mm rect replaces the old outline, 4 M3 holes at the corners
                 (H1..H4, out of the BOM / CPL); a round cut-out added; one outside is refused;
                 undo / redo; a hole where one is already is left out; warnings for a cut-out or
                 a hole through a track, a hole on a part, a track outside the new outline
  zones create   GND on F.Cu + B.Cu over the board outline, filled; undo takes it away; a zone of
                 another net over it at the same priority is warned about, an island in it goes
                 one priority above
  set_rules      preset jlcpcb_2layer: the board minimums and the Default class; undo
  board_setup    4 layers 1.6 mm (stackup with In1/In2), a GND pour on In1.Cu; back to 2 layers is
                 refused while In1.Cu has it; undo; export_fab has the inner layers' Gerbers;
                 saved: the file has the 4 layers and the stackup
"""

import argparse
import os
import re
import shutil
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from m5_e2e import FAILS, Mcp, Server, check, make_project  # noqa: E402

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--build", required=True)
    ap.add_argument("--kicad", required=True)
    ap.add_argument("--work", required=True)
    a = ap.parse_args()

    work = os.path.abspath(a.work)
    if os.path.exists(work):
        shutil.rmtree(work)
    os.makedirs(work)
    proj = make_project(a.kicad, work)
    pcb = os.path.join(proj, "blank.kicad_pcb")
    sock = os.path.join(work, "s.sock")
    env = dict(os.environ, KICAD_RUN_FROM_BUILD_DIR="1")
    server = Server(a.build, sock, pcb, env, os.path.join(work, "server.log"))
    check(server.start(), "api-server up with the blank board")
    mcp = Mcp([os.path.join(a.build, "kicadd-mcp"), "--socket", sock, "--stdio"], env)
    try:
        print("[parts]")
        # two pads on GND / VCC without a library: lib_create_footprint makes one
        err, t = mcp.tool("lib_create_footprint", {"kind": "chip", "size": "0805"})
        name = re.search(r"made (kicadd:\S+?):", t or "")
        check(not err and name, "a chip footprint for the parts: " + (t or "").splitlines()[0][:80])
        fpname = name.group(1) if name else "kicadd:R_0805"
        for ref, x in (("R1", 20), ("R2", 30)):
            err, t = mcp.tool("add_footprint", {"footprint": fpname, "ref": ref, "x": x, "y": 15,
                                                "nets": {"1": "GND", "2": "VCC"}})
            check(not err and t.startswith(ref), f"{ref} placed: " + t.splitlines()[0][:60])

        err, t = mcp.tool("get_rules")
        check(re.search(r"POWER: .*\[2 net\(s\)\]", t) is not None,
              "the new nets GND and VCC are in their net class (POWER, by pattern) at once")

        print("[board_outline]")
        err, t = mcp.tool("board_setup")
        check(not err and t.startswith("2 copper layers"), "board_setup reads: " + t.splitlines()[0])
        err, t = mcp.tool("board_outline", {"rect": [0, 0, 50, 30], "corner_radius": 2, "hole_inset": 3.5})
        print("      ", " | ".join(t.splitlines()[:3])[:220])
        check(not err and t.startswith("board 50 x 30 mm") and "1 old Edge.Cuts shape(s) replaced" in t,
              "a rounded 50 x 30 mm outline replaces the old one")
        m = re.search(r"([\d.]+) mm\^2", t)
        check(m and 1490 < float(m.group(1)) < 1500, f"area less the rounded corners: {m and m.group(1)}")
        check(all(f"H{i} @" in t for i in range(1, 5)) and "H1 @3.5,3.5 d3.2" in t, "4 M3 holes at the corners")
        check("not (wholly) inside" not in t, "R1, R2 are on the board")
        err, t = mcp.tool("export_fab", {"dir": os.path.join(work, "fab0"), "zip": False})
        bom = open(os.path.join(work, "fab0", [f for f in os.listdir(os.path.join(work, "fab0")) if f.endswith("-bom.csv")][0])).read()
        check("H1" not in bom and "R1" in bom, "the holes are not in the BOM")
        err, t = mcp.tool("undo")
        check(not err and t.startswith("undid: board outline"), "undo: " + t.splitlines()[0])
        err, q = mcp.tool("query_area", {"x": 3.5, "y": 3.5, "radius": 0.5, "kinds": ["footprint"]})
        check("H1" not in q, "the holes are gone with it")
        err, s = mcp.tool("board_summary")
        check("outline=99.95,99.95..160.05,140.05mm" in s, "the old outline is back: " + s.splitlines()[0][:120])
        err, t = mcp.tool("redo")
        check(not err and t.startswith("redid: board outline"), "redo")
        err, q = mcp.tool("query_area", {"x": 3.5, "y": 3.5, "radius": 0.5, "kinds": ["footprint"]})
        check("H1" in q, "the holes are back with it")

        # what the holes and cut-outs run into is said right away
        err, t = mcp.tool("board_outline", {"keep_old": True, "hole_inset": 3.5})
        check(not err and t.count("has a hole there already") == 4 and "no hole at 3.5,3.5: H1 has a hole" in t
              and "nothing changed" in t, "holes where there are holes are left out: " + t.splitlines()[1][:80])
        err, q = mcp.tool("query_area", {"x": 3.5, "y": 3.5, "radius": 0.5, "kinds": ["footprint"]})
        check("H5" not in q, "no second hole at the corner")
        err, t = mcp.tool("route", {"from": "R1.1", "to": "R2.1"})
        check(not err and "routed" in t, "a GND track R1.1 - R2.1: " + t.splitlines()[0][:80])
        err, t = mcp.tool("board_outline", {"keep_old": True, "cutouts": [{"rect": [24, 8, 26, 22]}]})
        check(not err and re.search(r"warning: cut-out 1 cuts through \d track\(s\) \(GND\)", t) is not None,
              "a cut-out across the track: " + next((ln for ln in t.splitlines() if "cut-out" in ln), t)[:110])
        mcp.tool("undo")
        err, t = mcp.tool("board_outline", {"keep_old": True, "holes": [[20, 15, 2]]})
        check(not err and re.search(r"warning: H5 @20,15 is on R1", t) is not None,
              "a hole on a part: " + next((ln for ln in t.splitlines() if "is on" in ln), t)[:110])
        mcp.tool("undo")
        err, t = mcp.tool("board_outline", {"keep_old": True, "holes": [{"x": 25, "y": 15, "diameter": 2, "plated": True, "net": "GND"}]})
        check(not err and "goes through" not in t, "a plated GND hole on the GND track is fine")
        mcp.tool("undo")
        err, t = mcp.tool("board_outline", {"keep_old": True, "holes": [{"x": 25, "y": 15, "diameter": 2}]})
        check(not err and re.search(r"warning: H5 @25,15 goes through \d track\(s\) \(GND\)", t) is not None,
              "an unplated hole through it is not: " + next((ln for ln in t.splitlines() if "goes through" in ln), t)[:110])
        mcp.tool("undo")
        err, t = mcp.tool("board_outline", {"rect": [0, 0, 24, 30]})
        check(not err and re.search(r"warning: \d track\(s\) \(GND\) are outside the new outline, or cross it", t) is not None
              and re.search(r"part\(s\) not \(wholly\) inside the outline:.*R2", t) is not None,
              "an outline that leaves a track and R2 outside: " + next((ln for ln in t.splitlines() if "outside the new" in ln), t)[:110])
        mcp.tool("undo")
        err, t = mcp.tool("board_outline", {"keep_old": True, "cutouts": [{"circle": [40, 15, 5]}]})
        m2 = re.search(r"([\d.]+) mm\^2", t)
        check(not err and m2 and abs(float(m.group(1)) - float(m2.group(1)) - 19.6) < 0.5,
              f"a 5 mm round cut-out takes ~19.6 mm^2: {t.splitlines()[0]}")
        err, t = mcp.tool("board_outline", {"keep_old": True, "cutouts": [{"rect": [45, 10, 55, 20]}]})
        check(err and "reaches outside" in t, "a cut-out over the edge is refused")
        err, t = mcp.tool("board_outline", {"rect": [0, 0, 20, 10], "holes": [[30, 5]]})
        check(err and "not on the board" in t, "a hole off the board is refused")

        print("[zones create]")
        err, t = mcp.tool("zones", {"create": {"net": "GND"}})
        print("      ", " | ".join(t.splitlines()[:3])[:220])
        check(not err and t.startswith("new zone GND on F.Cu+B.Cu") and "refilled" in t, "a GND pour on both sides")
        check(re.search(r"GND F\.Cu,B\.Cu .*pads thermal", t) and "not filled" not in t, "listed, filled: " + t.splitlines()[-1][:150])
        err, t = mcp.tool("run_drc")
        kinds = re.search(r"errors: (.*)", t).group(1)
        check("clearance" not in kinds and "copper_edge" not in kinds, "DRC: no clearance / edge errors: " + kinds)
        if "clearance" in kinds:
            print(t)
        err, t = mcp.tool("undo")
        err, z = mcp.tool("zones")
        check(not err and z.startswith("0 copper zone"), "undo takes the zone away")
        mcp.tool("redo")
        err, t = mcp.tool("zones", {"create": {"net": "NOPE"}})
        check(err and "no net NOPE" in t, "an unknown net is refused")

        # zones of other nets on the same copper
        err, z = mcp.tool("zones")
        gnd = re.search(r"^  (\S+) GND ", z, re.M).group(1)
        err, t = mcp.tool("zones", {"create": {"net": "VCC", "layers": ["B.Cu"]}})
        check(not err and "warning: it overlaps the zone GND on B.Cu at the same priority 0" in t
              and f'zones ids=["{gnd}"] priority=1' in t,
              "the same area and priority as the GND pour is warned about: " + t.splitlines()[1][:100])
        err, t = mcp.tool("undo")
        check(not err and "note: the zone fills are out of date after this: refill_zones" in t,
              "undo says the fills are out of date")
        err, t = mcp.tool("zones", {"create": {"net": "VCC", "layers": ["F.Cu"], "outline": [[8, 6], [18, 6], [18, 11], [8, 11]]}})
        check(not err and "priority 1: above the bigger zone(s) it lies in (GND on F.Cu)" in t and "warning" not in t
              and re.search(r"VCC F\.Cu priority 1", t) is not None,
              "an island in the pour goes above it: " + t.splitlines()[1][:100])
        err, t = mcp.tool("zones", {"create": {"net": "VCC", "layers": ["F.Cu"], "priority": 0,
                                                "outline": [[30, 20], [40, 20], [40, 26], [30, 26]]}})
        check(not err and "at the same priority 0" in t, "a priority given is kept, and warned about")
        mcp.tool("undo")
        mcp.tool("undo")
        mcp.tool("refill_zones")

        print("[set_rules preset]")
        err, t = mcp.tool("set_rules", {"preset": "jlcpcb_2layer"})
        print("      ", " | ".join(t.splitlines()[:2])[:200])
        check(not err and "preset: JLCPCB standard process" in t, "the preset says where its numbers come from")
        check("track 0.127" in t and "drill 0.3" in t and "copper to edge 0.3" in t and "silk text 1 high 0.15" in t,
              "board minimums: " + next((ln for ln in t.splitlines() if ln.startswith("constraints")), ""))
        err, t = mcp.tool("set_rules", {"preset": "nope"})
        check(err and "jlcpcb_2layer" in t, "an unknown preset lists the ones there are")
        err, t = mcp.tool("undo")
        err, r = mcp.tool("get_rules")
        check("track 0.127" not in r.split("constraints")[1], "undo puts the old minimums back")

        print("[board_setup]")
        err, t = mcp.tool("board_setup", {"copper_layers": 4, "thickness": 1.6, "inner_copper_oz": 0.5})
        print("      ", " | ".join(t.splitlines()[:6])[:300])
        check(not err and "4 copper layers, 1.6 mm thick" in t and "In1.Cu copper 0.0175 mm (0.5 oz)" in t
              and "F.Cu copper 0.035 mm (1 oz)" in t, "4 layers, the stackup made anew")
        err, s = mcp.tool("board_summary")
        check("cu_layers=4" in s, "board_summary: 4 layers")
        err, t = mcp.tool("board_setup", {"stackup": "list"})
        check(not err and "JLC04161H-7628 " in t and "JLC06161H-3313" in t and "jlcpcb.com/impedance" in t, "stackup list")
        err, t = mcp.tool("board_setup", {"stackup": "jlc04161h-7628b"})
        print("      ", " | ".join(t.splitlines()[:8])[:400])
        check(not err and "4 copper layers, 1.61 mm thick" in t
              and "prepreg 7628 0.218 mm (er 4.4) + 7628 0.218 mm (er 4.4) + 2116 0.1164 mm (er 4.16)" in t
              and "core FR4 0.4 mm (er 4.6)" in t and "In1.Cu copper 0.0152 mm" in t and "note: JLCPCB's" in t,
              "JLC04161H-7628B: its sheets, thicknesses and Dk")
        err, t = mcp.tool("board_setup", {"stackup": "JLC04161H-7628", "thickness": 1.2})
        check(err and "give it alone" in t, "a fab stackup with a thickness: refused")
        err, t = mcp.tool("board_setup", {"stackup": "JLC99"})
        check(err and "no stackup JLC99" in t, "an unknown stackup: refused")
        err, t = mcp.tool("undo")
        err, t = mcp.tool("board_setup")
        check("1.6 mm thick" in t and "7628" not in t, "undo: the default 4-layer stackup again")
        err, t = mcp.tool("zones", {"create": {"net": "GND", "layers": ["In1.Cu"], "name": "GND plane"}})
        check(not err and t.startswith("new zone GND on In1.Cu"), "a GND plane on In1.Cu")
        err, t = mcp.tool("board_setup", {"copper_layers": 2})
        check(err and "zone on In1.Cu" in t, "back to 2 layers refused while In1.Cu has a zone: " + t[:120])
        err, t = mcp.tool("set_rules", {"preset": "jlcpcb_4layer"})
        check(not err and "track 0.09" in t, "jlcpcb_4layer")
        err, t = mcp.tool("export_fab", {"dir": os.path.join(work, "fab4"), "zip": False})
        files = os.listdir(os.path.join(work, "fab4"))
        check(any("In1_Cu" in f for f in files) and any("In2_Cu" in f for f in files), "Gerbers of the inner layers")
        err, t = mcp.tool("save_board")
        text = open(pcb, encoding="utf-8").read()
        check('"In1.Cu"' in text and "(stackup" in text and "GND plane" in text, "saved: 4 layers, the stackup, the plane")
        for _ in range(3):
            err, t = mcp.tool("undo")
        check(not err and t.startswith("undid: board setup"), "undo the setup: " + t.splitlines()[0])
        err, t = mcp.tool("board_setup")
        check(t.startswith("2 copper layers"), "2 layers again")
    finally:
        mcp.close()
        server.stop()
    print("\nRESULT:", "PASS" if not FAILS else f"FAIL ({len(FAILS)})")
    sys.exit(1 if FAILS else 0)


if __name__ == "__main__":
    main()
