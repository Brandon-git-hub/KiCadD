"""M5c benchmark: auto_place a real board from scratch and compare with its human placement.

usage: python3 -I autoplace_bench.py --build BUILD --kicad KICAD_SRC --work DIR [--board demos/pic_programmer]

Copies the demo project, strips tracks/vias/zones, starts kicad-cli api-server on it, moves
every movable footprint 250 mm off the board, runs auto_place and reports: time, half-perimeter
wire length (human layout vs auto), parts without room, and the full DRC error kinds (only
unconnected_items is expected: nothing is routed).
"""

import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from m5_e2e import Mcp, Server, top_level_blocks  # noqa: E402


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--build", required=True)
    ap.add_argument("--kicad", required=True)
    ap.add_argument("--work", required=True)
    ap.add_argument("--board", default="demos/pic_programmer")
    a = ap.parse_args()

    work = os.path.abspath(a.work)
    if os.path.exists(work):
        shutil.rmtree(work)
    src = os.path.join(a.kicad, a.board)
    proj = os.path.join(work, "proj")
    shutil.copytree(src, proj)
    pcb = next(os.path.join(proj, f) for f in os.listdir(proj) if f.endswith(".kicad_pcb"))

    text = open(pcb, encoding="utf-8").read()
    for name, s, e in reversed(top_level_blocks(text)):
        if name in ("segment", "via", "zone", "arc"):
            text = text[:s] + text[e:]
    open(pcb, "w", encoding="utf-8").write(text)

    sock = os.path.join(work, "s.sock")
    env = dict(os.environ, KICAD_RUN_FROM_BUILD_DIR="1")
    server = Server(a.build, sock, pcb, env, os.path.join(work, "server.log"))
    if not server.start():
        print("server did not start")
        return 1
    mcp = Mcp([os.path.join(a.build, "kicadd-mcp"), "--socket", sock, "--stdio"], env)
    try:
        err, t = mcp.tool("board_summary")
        print(t.splitlines()[0])
        err, q = mcp.tool("query_area", {"x": 0, "y": 0, "radius": 2000, "kinds": ["footprint"], "limit": 2000})
        refs = [m.group(1) for m in re.finditer(r"^FP (\S+) (\S+)", q, re.M) if m.group(2).startswith("F.")
                or m.group(2) in ("top_layer",)]
        # the human layout's wire length: a placement request that cannot place anything
        # (one part, 1 m spacing) reports the current length and moves nothing
        err, h = mcp.tool("auto_place", {"refs": [refs[0]], "rotate": False, "spacing": 1000})
        hm = re.search(r"wire length \(half-perimeter\) ([\d.]+) ->", h)
        human = float(hm.group(1)) if hm else None
        mcp.tool("move_footprints", {"refs": refs, "dx": -250, "dy": 0})
        t0 = time.time()
        err, t = mcp.tool("auto_place")
        dt = time.time() - t0
        m = re.search(r"wire length \(half-perimeter\) ([\d.]+) -> ([\d.]+) mm", t)
        err, d = mcp.tool("run_drc", {"include_warnings": False})
        kinds = re.search(r"errors: (.*)", d).group(1) if "errors:" in d else d[:100]
        print(f"{len(refs)} parts off the board, auto_place {dt:.1f} s")
        print(f"wire length: human {human} mm, auto {m.group(2) if m else '?'} mm")
        print(t.splitlines()[-1] if "no room" in t else "all placed")
        print("DRC errors:", kinds)
        ok = m and "no room" not in t and set(re.findall(r"(\w+) \d+", kinds)) <= {"unconnected_items"}
        print("RESULT:", "PASS" if ok else "FAIL")
        return 0 if ok else 1
    finally:
        mcp.close()
        server.stop()


if __name__ == "__main__":
    sys.exit(main())
