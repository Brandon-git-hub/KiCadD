"""auto_place rules end to end, on KiCad's pic_programmer demo (headless).

usage: python3 -I rules_e2e.py --build BUILD --kicad KICAD_SRC --work DIR

Tracks stripped, every front part moved off the board, then auto_place with the rules a
designer would give (the human layout's own intent):
  edge      J1 (DB9) and P1 (power terminal) on the left edge
  rotation  J1 and P1 at 270
  region    U3 (the regulator) in the left part of the board
  near      U4 and C4 within 8 mm
The board is saved and read back: courtyards (from the file, footprint by footprint) show every
rule met; DRC has no error the human layout doesn't have. A near rule that can't be met
(two big sockets 2 mm apart) is reported, bad rules are refused.
"""

import argparse
import math
import os
import re
import shutil
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from m5_e2e import FAILS, Mcp, Server, check, top_level_blocks  # noqa: E402


def courtyards(pcb_text):
    """{ref: (x0, y0, x1, y1, rotation, layer)}: courtyard boxes in board coordinates."""
    out = {}
    for name, s, e in top_level_blocks(pcb_text):
        if name != "footprint":
            continue
        fp = pcb_text[s:e]
        ref = re.search(r'\(property "Reference" "([^"]+)"', fp).group(1)
        # (at x y rot) in older files, (transform (translate x y) (rotate r)) in newer ones
        at = re.search(r'\n\t\t\(at ([-\d.]+) ([-\d.]+)(?: ([-\d.]+))?\)', fp)
        if at:
            x, y, rot = float(at.group(1)), float(at.group(2)), float(at.group(3) or 0)
        else:
            tr = re.search(r'\(translate ([-\d.]+) ([-\d.]+)\)', fp)
            ro = re.search(r'\(transform.*?\(rotate ([-\d.]+)\)', fp, re.S)
            x, y, rot = float(tr.group(1)), float(tr.group(2)), float(ro.group(1)) if ro else 0.0
        layer = re.search(r'\(footprint "[^"]+"\s*\(layer "([^"]+)"', fp).group(1)
        a = math.radians(rot)
        pts = []
        for item in re.finditer(r'\((fp_line|fp_rect|fp_poly|fp_circle|fp_arc)(.*?)\(layer "([FB])\.CrtYd"\)', fp, re.S):
            for px, py in re.findall(r'\((?:start|end|xy|center|mid) ([-\d.]+) ([-\d.]+)\)', item.group(2)):
                lx, ly = float(px), float(py)
                # KiCad: local -> board, counter-clockwise on screen (Y down)
                pts.append((x + lx * math.cos(a) + ly * math.sin(a), y - lx * math.sin(a) + ly * math.cos(a)))
        # the pads' box (near rules measure between its centres)
        pads = []
        for pm in re.finditer(r'\(pad "[^"]*" \w+ \w+\s*\(at ([-\d.]+) ([-\d.]+)(?: [-\d.]+)?\)\s*\(size ([-\d.]+) ([-\d.]+)\)', fp):
            px, py, w, h = (float(v) for v in pm.groups())
            for cx, cy in ((px - w / 2, py - h / 2), (px + w / 2, py + h / 2), (px - w / 2, py + h / 2), (px + w / 2, py - h / 2)):
                pads.append((x + cx * math.cos(a) + cy * math.sin(a), y - cx * math.sin(a) + cy * math.cos(a)))
        if pts:
            xs, ys = [p[0] for p in pts], [p[1] for p in pts]
            pc = ((min(p[0] for p in pads) + max(p[0] for p in pads)) / 2,
                  (min(p[1] for p in pads) + max(p[1] for p in pads)) / 2) if pads else None
            out[ref] = (min(xs), min(ys), max(xs), max(ys), rot % 360, layer, pc)
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
    proj = os.path.join(work, "proj")
    shutil.copytree(os.path.join(a.kicad, "demos", "pic_programmer"), proj)
    pcb = os.path.join(proj, "pic_programmer.kicad_pcb")
    text = open(pcb, encoding="utf-8").read()
    for name, s, e in reversed(top_level_blocks(text)):
        if name in ("segment", "via", "zone", "arc"):
            text = text[:s] + text[e:]
    open(pcb, "w", encoding="utf-8").write(text)
    human = courtyards(text)

    sock = os.path.join(work, "s.sock")
    env = dict(os.environ, KICAD_RUN_FROM_BUILD_DIR="1")
    server = Server(a.build, sock, pcb, env, os.path.join(work, "server.log"))
    check(server.start(), "api-server up on pic_programmer (tracks stripped)")
    mcp = Mcp([os.path.join(a.build, "kicadd-mcp"), "--socket", sock, "--stdio"], env)
    try:
        err, t = mcp.tool("board_summary")
        o = re.search(r"outline=([-\d.]+),([-\d.]+)\.\.([-\d.]+),([-\d.]+)mm", t)
        x0, y0, x1, y1 = (float(v) for v in o.groups())
        tol = 0.5 + 2 * 0.2 + 0.05      # edge clearance + two grid steps
        check(human["J1"][0] - x0 < 3, f"the human layout has J1 at the left edge too ({human['J1'][0] - x0:.2f} mm)")

        err, d0 = mcp.tool("run_drc", {"include_warnings": False})
        base = {k: int(n) for k, n in re.findall(r"(\w+) (\d+)", re.search(r"errors: (.*)", d0).group(1))}

        err, q = mcp.tool("query_area", {"x": 0, "y": 0, "radius": 2000, "kinds": ["footprint"], "limit": 2000})
        refs = [m.group(1) for m in re.finditer(r"^FP (\S+) (\S+)", q, re.M) if "top" in m.group(2).lower()
                or m.group(2).startswith("F.")]
        mcp.tool("move_footprints", {"refs": refs, "dx": -(x1 - x0 + 50), "dy": 0})

        print("[bad rules]")
        err, t = mcp.tool("auto_place", {"rules": {"edge": {"J1": "middle"}}})
        check(err and "left, right, top, bottom or any" in t, "edge side checked: " + t[:70])
        err, t = mcp.tool("auto_place", {"rules": {"near": [{"refs": ["Q99", "U1"], "max": 3}]}})
        check(err and "no footprint Q99" in t, "unknown ref refused: " + t[:60])
        err, t = mcp.tool("auto_place", {"rules": {"rotation": {"J1": 45}}})
        check(err and "0, 90, 180 or 270" in t, "rotation checked")

        print("[rules]")
        region = [x0, y0, x0 + (x1 - x0) * 0.35, y1]
        rules = {"edge": {"J1": "left", "P1": "left"}, "rotation": {"J1": 270, "P1": 270},
                 "region": {"U3": region}, "near": [{"refs": ["U4", "C4"], "max": 8}]}
        err, t = mcp.tool("auto_place", {"rules": rules})
        m = re.search(r"placed (\d+) part.*wire length \(half-perimeter\) [\d.]+ -> ([\d.]+) mm", t)
        check(not err and m and "no room" not in t and "rules not met" not in t,
              "auto_place with rules: " + (f"{m.group(1)} parts, {m.group(2)} mm" if m else t[:120]) + " | " + " ".join(t.split("\n")[1:])[:300])
        mcp.tool("save_board")
        cy = courtyards(open(pcb, encoding="utf-8").read())

        for ref in ("J1", "P1"):
            bx = cy[ref]
            check(bx[0] - x0 <= tol and bx[4] == 270, f"{ref} at the left edge ({bx[0] - x0:.2f} mm), rotated {bx[4]:.0f}")
        u3 = cy["U3"]
        check(u3[0] >= region[0] - 0.01 and u3[2] <= region[2] + 0.01 and u3[1] >= region[1] - 0.01
              and u3[3] <= region[3] + 0.01, f"U3 inside its region (x {u3[0]:.1f}..{u3[2]:.1f} <= {region[2]:.1f})")
        dist = math.dist(cy["U4"][6], cy["C4"][6])
        check(dist <= 8.05, f"U4 and C4 {dist:.1f} mm apart, pad centres (max 8)")

        err, d = mcp.tool("run_drc", {"include_warnings": False})
        kinds = {k: int(n) for k, n in re.findall(r"(\w+) (\d+)", re.search(r"errors: (.*)", d).group(1))}
        new = {k: n - base.get(k, 0) for k, n in kinds.items() if k != "unconnected_items" and n > base.get(k, 0)}
        check(not new, f"DRC: nothing the human layout doesn't have (new: {new or 'none'})")

        print("[a rule that can't be met]")
        err, t = mcp.tool("auto_place", {"refs": ["U5"], "rules": {"near": [{"refs": ["P2", "U5"], "max": 2}]}})
        check("rules not met" in t and re.search(r"P2 U5: [\d.]+ mm apart \(max 2.0\)|U5 P2: [\d.]+ mm apart \(max 2.0\)", t),
              "reported: " + " ".join(t.split("rules not met:")[-1].split())[:70])
    finally:
        mcp.close()
        server.stop()

    print("\nRESULT:", "PASS" if not FAILS else f"FAIL ({len(FAILS)})")
    return 1 if FAILS else 0


if __name__ == "__main__":
    sys.exit(main())
