"""Connectors over the board edge (headless): auto_place edge rules turn a connector to open over
its edge; the footprint's own "PCB Edge" line goes on the edge, else its front flush with it.

usage: python3 -I overhang_e2e.py --build BUILD --kicad KICAD_SRC --work DIR

Blank project (m5_e2e's), a 60 x 40 mm outline, KiCad's library footprints:
  J1 USB-C CNCTech C-ARA1-AK51X   left    its PCB Edge line (local y 5.25) on x = 0: 1.25 mm over
  J2 USB-C HRO TYPE-C-31-M-12     right   no line: its courtyard front (4.15) on x = 60
  J3 barrel jack CUI PJ-063AH     bottom  overhang 2: its front (12.5) 2 mm below y = 40
  J4 RJ12 Amphenol 54601          top     turned 180: its front (17.27) on y = 0
  J5 USB-C HRO                    left, inside=true: kept inside like any part
  R1 0805                         left    no opening: inside, edge 0.5 mm as before
  refusals / notes: a rotation rule turning J1 away from its edge; an overhang that would put
  J3's pads past the edge (kept inside, said); undo
  no room: a 12 x 8 mm board says it is too small, and which parts can't fit it in any rotation
"""

import argparse
import os
import re
import shutil
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from m5_e2e import FAILS, Mcp, Server, binaries, check, make_project  # noqa: E402

PARTS = (("J1", "Connector_USB:USB_C_Receptacle_CNCTech_C-ARA1-AK51X"),
         ("J2", "Connector_USB:USB_C_Receptacle_HRO_TYPE-C-31-M-12"),
         ("J3", "Connector_BarrelJack:BarrelJack_CUI_PJ-063AH_Horizontal"),
         ("J4", "Connector_RJ:RJ12_Amphenol_54601-x06_Horizontal"),
         ("J5", "Connector_USB:USB_C_Receptacle_HRO_TYPE-C-31-M-12"),
         ("R1", "Resistor_SMD:R_0805_2012Metric"))


def where(mcp):
    """{ref: (x, y, rot)} from query_area."""
    err, t = mcp.tool("query_area", {"x": 30, "y": 20, "radius": 200, "kinds": ["footprint"]})
    out = {}
    for m in re.finditer(r"^FP (\S+) \S+ @([-\d.]+),([-\d.]+)(?: r([-\d.]+))?", t, re.M):
        out[m.group(1)] = (float(m.group(2)), float(m.group(3)), float(m.group(4) or 0) % 360)
    return out


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
    kicad_cli, kicadd_mcp, env = binaries(a.build)
    server = Server(a.build, sock, pcb, env, os.path.join(work, "server.log"))
    check(server.start(), "api-server up with the blank board")
    mcp = Mcp([kicadd_mcp, "--socket", sock, "--stdio"], env)
    try:
        err, t = mcp.tool("board_outline", {"rect": [0, 0, 60, 40]})
        check(not err, "a 60 x 40 mm board")
        for k, (ref, fp) in enumerate(PARTS):
            err, t = mcp.tool("add_footprint", {"footprint": fp, "ref": ref, "x": 100, "y": 100 + 25 * k})
            check(not err and t.startswith(ref + " added"), f"{ref} {fp.split(':')[1]} off the board")

        print("[refusals]")
        err, t = mcp.tool("auto_place", {"rules": {"edge": {"J1": "left"}, "rotation": {"J1": 0}}})
        check(err and "J1 opens to the bottom, away from its edge" in t, "a rotation turning J1 away: refused: " + t[:110])

        print("[auto_place]")
        rules = {"edge": {"J1": "left", "J2": "right", "J3": {"side": "bottom", "overhang": 2}, "J4": "top",
                          "J5": {"side": "left", "inside": True}, "R1": "left"}}
        err, t = mcp.tool("auto_place", {"rules": rules})
        print("      ", "\n        ".join(t.splitlines()[:8]))
        check(not err and t.startswith("placed 6 part(s)"), "all six placed")
        check("J1: opens over the board's left edge, 1.25 mm over it (the footprint's PCB Edge line)" in t,
              "J1: the footprint's edge line")
        check("J2: opens over the board's right edge, its courtyard flush with it" in t, "J2: flush")
        check("J3: opens over the board's bottom edge, 2.00 mm over it (overhang given)" in t, "J3: 2 mm given")
        check("J4: opens over the board's top edge" in t, "J4: top")
        check("J5:" not in t and "R1:" not in t, "J5 (inside=true) and R1 (no opening): no edge note")

        w = where(mcp)
        print("       ", w)
        x, y, r = w.get("J1", (0, 0, 0))
        check(abs(x - 5.25) < 0.002 and r == 270, f"J1 turned to face left, its edge line on x = 0: {x}, r{r}")
        x, y, r = w.get("J2", (0, 0, 0))
        check(abs(x - (60 - 4.15)) < 0.01 and r == 90, f"J2 turned to face right, its front on x = 60: {x}, r{r}")
        x, y, r = w.get("J3", (0, 0, 0))
        check(abs(y - (40 - 12.5 + 2)) < 0.01 and r == 0, f"J3 facing down, front 2 mm past y = 40: {y}")
        x, y, r = w.get("J4", (0, 0, 0))
        check(abs(y - 17.27) < 0.01 and r == 180, f"J4 turned to face up, its front on y = 0: {y}, r{r}")
        x, y, r = w.get("J5", (0, 0, 0))
        check(0 < x < 60 and 0 < y < 40 and x > 5.5, f"J5 inside the board: {x}, {y}")
        x, y, r = w.get("R1", (0, 0, 0))
        check(1.4 < x < 3, f"R1 at the left edge, inside by the edge gap: x {x}")

        err, t = mcp.tool("run_drc")
        kinds = re.search(r"errors: (.*)", t)
        check(kinds is None or "copper_edge" not in kinds.group(1), "DRC: no copper-to-edge errors: "
              + (kinds.group(1) if kinds else "none"))

        err, t = mcp.tool("undo")
        check(not err and t.startswith("undid: auto-place 6 parts"), "undo: " + t.splitlines()[0])
        check(where(mcp).get("J1", (0,))[0] == 100, "J1 back off the board")

        print("[too far over]")
        err, t = mcp.tool("auto_place", {"refs": ["J3"], "rules": {"edge": {"J3": {"side": "bottom", "overhang": 12}}}})
        check(not err and re.search(r"J3: its pads would reach [\d.]+ mm past the board edge: kept inside the edge", t)
              is not None, "J3 12 mm over would cut its pads: kept inside, said: "
              + next((ln for ln in t.splitlines() if "J3:" in ln), t)[:120])
        x, y, r = where(mcp).get("J3", (0, 0, 0))
        check(y + 12.5 <= 40, f"J3 inside: front at y {y + 12.5:.2f}")

        print("[misplaced rules refused]")
        err, t = mcp.tool("auto_place", {"rules": {"J2": {"side": "left", "overhang": 1}}})
        check(err and 'unknown key "J2"' in t and '{"edge": {"J2": ...}}' in t,
              "a ref at the top of rules (edge left out): refused, the edge rule hinted: " + t[:160])
        err, t = mcp.tool("auto_place", {"rules": {"edge": {"J2": {"sid": "left"}}}})
        check(err and 'edge rule J2: unknown key "sid"' in t, "a typo in an edge rule: refused")
        err, t = mcp.tool("auto_place", {"rules": {"edge": {"J2": 3}}})
        check(err and "edge rule J2: a side" in t, "an edge rule neither a side nor an object: refused")

        print("[mounting holes stay]")
        err, t = mcp.tool("board_outline", {"keep_old": True, "hole_inset": 3})
        holes = {k: v for k, v in where(mcp).items() if re.fullmatch(r"H\d+", k)}
        check(not err and len(holes) == 4, f"4 corner holes: {sorted(holes)}")
        err, t = mcp.tool("auto_place", {"refs": [], "time_limit": 1})
        check("kept: H1, H2, H3, H4 (mounting holes: name them in refs to move them)" in t,
              "auto_place: the holes (boxes past the edge) kept, said: "
              + next((ln.strip() for ln in t.splitlines() if "kept:" in ln), t[:80]))
        err, t = mcp.tool("auto_place", {"all": True, "time_limit": 1})
        check({k: where(mcp)[k] for k in holes} == holes, "all: True moves everything but the holes")
        mcp.tool("undo")
        mcp.tool("undo")
        mcp.tool("undo")

        print("[no room]")
        err, t = mcp.tool("board_outline", {"rect": [0, 0, 12, 8]})
        err, t = mcp.tool("auto_place", {"all": True})
        why = next((ln.strip() for ln in t.splitlines() if ln.strip().startswith("why:")), "")
        print("      ", why[:300])
        check("no room for" in t and "the board is too small for them" in why, "a 12 x 8 mm board: too small, said")
        check(re.search(r"bigger than the board in any rotation:.* J4 \(", why) is not None, "J4 (RJ12) can't fit it at all: said")
    finally:
        mcp.close()
        server.stop()

    print("\nRESULT:", "PASS" if not FAILS else f"FAIL ({len(FAILS)})")
    sys.exit(1 if FAILS else 0)


if __name__ == "__main__":
    main()
