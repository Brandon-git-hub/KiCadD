"""Defaults and behaviour (headless): beta.15 batch 3.

usage: python3 -I behave_e2e.py --build BUILD --demos DEMOS_DIR --work DIR

  poured     route_short leaves a net with a copper zone that came in through refs (listed),
             routes it when named or include_poured; skip_nets globs
  vias       stitch_vias at: one via at each point (no grid), what it lands on said; one on a
             pad of another net refused; undo; route's lone-via error points there
  new        project new: the outline centred on the sheet clear of the title block, on whole mm;
             one too big for A4 on A3
  rename     project rename: the files under the new name, the old ones gone, reopened; refused
             when a file of that name is there
  sch_move   a wire dragged along becomes an L (no diagonal), the nets stay; sch_delete dangling
             leaves a wire teed into another's middle alone
"""

import argparse
import os
import re
import shutil
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from m5_e2e import FAILS, Mcp, Server, binaries, check  # noqa: E402


def unrouted(m):
    err, t = m.tool("board_summary", {})
    g = re.search(r"unconnected=(\d+)", t)
    return int(g.group(1)) if g else -1


def wires(path):
    t = open(path, encoding="utf-8").read()
    return [tuple(float(v) for v in w) for w in
            re.findall(r"\(wire\s*\(pts\s*\(xy ([\d.\-]+) ([\d.\-]+)\)\s*\(xy ([\d.\-]+) ([\d.\-]+)\)", t)]


def netlist(m):
    err, t = m.tool("sch_netlist", {})
    return {ln.split(":")[0]: set(ln.split(":")[1].split()) for ln in t.splitlines()[1:] if ":" in ln
            and not ln.startswith(("unconnected", "no-connect"))}


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
        print("[poured]")
        err, t = m.tool("query_net", {"net": "GND"})
        part = next(r for r in re.findall(r"\b(C\d+)\.\d+\b", t))
        err, t = m.tool("delete_tracks", {"refs": [part]})
        check(not err, f"tracks of {part} deleted: " + t.splitlines()[0][:80])
        err, t = m.tool("route_short", {"refs": [part], "dry_run": True})
        check("skipped (poured" in t and "GND (B.Cu)" in t, "GND left to its zone, said: "
              + next((ln for ln in t.splitlines() if "skipped" in ln), t[:120]))
        err, t2 = m.tool("route_short", {"refs": [part], "dry_run": True, "include_poured": True})
        check("skipped (poured" not in t2, "include_poured: not skipped")
        err, t3 = m.tool("route_short", {"refs": [part], "nets": ["GND"], "dry_run": True})
        check("skipped (poured" not in t3, "named in nets: not skipped")
        others = [n for n in re.findall(r"\[([^\]]+)\]", t) if n != "GND"]
        if others:
            err, t4 = m.tool("route_short", {"refs": [part], "dry_run": True, "skip_nets": [others[0][:2] + "*"]})
            check("skipped (skip_nets)" in t4, "skip_nets glob: " + next((ln for ln in t4.splitlines() if "skip_nets" in ln), t4[:100]))
        m.tool("undo", {})

        print("[vias]")
        err, t = m.tool("query_area", {"x": 150, "y": 100, "radius": 30, "via_net": "GND"})
        spot = re.search(r"@(-?[\d.]+),(-?[\d.]+)", t)
        check(spot is not None, "a free GND via spot: " + t.splitlines()[0][:80] if t else "")
        x, y = float(spot.group(1)), float(spot.group(2))
        u0 = unrouted(m)
        err, t = m.tool("stitch_vias", {"at": [x, y]})
        check(err and "net" in t, "at without net: refused")
        err, t = m.tool("stitch_vias", {"at": [[x, y]], "net": "GND", "dry_run": True})
        check(not err and t.startswith("would add 1 of 1 GND via"), "dry_run: " + t.splitlines()[0][:80])
        err, t = m.tool("stitch_vias", {"at": [[x, y]], "net": "GND"})
        check(not err and t.startswith("added 1 of 1 GND via") and "zone B.Cu" in t,
              "one via at the spot, on the GND zone: " + " | ".join(t.splitlines()[:2])[:140])
        err, t = m.tool("query_area", {"x": x, "y": y, "radius": 0.05, "kinds": ["via"]})
        check("VIA" in t.upper() and "GND" in t, "the via is there")
        err, t = m.tool("query_area", {"ref": "U1", "radius": 0.1, "kinds": ["pad"]})
        pad = re.search(r"PAD U1\.\d+ \[(?!GND)[^\]]*\] \S+ @(-?[\d.]+),(-?[\d.]+)", t)
        if pad:
            err, t = m.tool("stitch_vias", {"at": [float(pad.group(1)), float(pad.group(2))], "net": "GND"})
            check("added 0 of 1" in t and "not placed" in t, "on another net's pad: not placed: "
                  + next((ln.strip() for ln in t.splitlines() if "not placed" in ln), t)[:120])
        err, t = m.tool("undo", {})
        check(not err and unrouted(m) == u0, "undo takes the via back: " + t.strip()[:60])

        print("[new]")
        fresh = os.path.join(work, "fresh")
        err, t = m.tool("project", {"new": {"path": fresh, "outline": [50, 40]}, "discard": True})
        check(not err and "centred above the title block" in t, "new: " + next((ln for ln in t.splitlines() if "outline" in ln), t)[:120])
        pcb = open(os.path.join(fresh, "fresh.kicad_pcb"), encoding="utf-8").read()
        check(re.search(r"\(start 124 68\)|\(xy 124 68\)", pcb) and '(paper "A4")' in pcb,
              "50 x 40 at (124, 68) on A4 (frame centre above the title block, whole mm)")
        big = os.path.join(work, "big")
        err, t = m.tool("project", {"new": {"path": big, "outline": [300, 200]}, "discard": True})
        pcb = open(os.path.join(big, "big.kicad_pcb"), encoding="utf-8").read()
        check(not err and "A3" in t and '(paper "A3")' in pcb, "300 x 200: on an A3 sheet: "
              + next((ln for ln in t.splitlines() if "outline" in ln), t)[:120])

        print("[rename]")
        err, t = m.tool("project", {"open": os.path.join(fresh, "fresh.kicad_pcb"), "discard": True})
        err, t = m.tool("pcb_text", {"add": [{"text": "X", "at": [130, 70]}]})
        err, t = m.tool("project", {"rename": "fresh2"})
        names = sorted(os.listdir(fresh))
        check(not err and "renamed fresh -> fresh2" in t, "rename: " + t.splitlines()[0][:120])
        check("fresh2.kicad_pro" in names and "fresh2.kicad_pcb" in names and "fresh2.kicad_sch" in names
              and not any(n.startswith("fresh.") for n in names), f"files renamed: {names}")
        check("file=fresh2.kicad_pcb" in t, "reopened under the new name")
        pro = open(os.path.join(fresh, "fresh2.kicad_pro"), encoding="utf-8").read()
        check('"filename": "fresh2.kicad_pro"' in pro, "the .kicad_pro's own name in it")
        check('"X"' in open(os.path.join(fresh, "fresh2.kicad_pcb"), encoding="utf-8").read(),
              "saved before the rename (the unsaved text is in the file)")
        open(os.path.join(fresh, "taken.kicad_sch"), "w").write("")
        err, t = m.tool("project", {"rename": "taken"})
        check(err and "taken.kicad_sch is there already" in t and os.path.exists(os.path.join(fresh, "fresh2.kicad_pcb")),
              "rename onto an existing file: refused, nothing changed")

        print("[sch_move]")
        ecc = os.path.join(work, "ecc83")
        shutil.copytree(os.path.join(a.demos, "ecc83"), ecc)
        err, t = m.tool("project", {"open": os.path.join(ecc, "ecc83-pp.kicad_pcb"), "discard": True})
        refs = []
        for x, y in ((25.4, 25.4), (76.2, 25.4), (25.4, 63.5)):
            err, t = m.tool("sch_add_part", {"lib_id": "ecc83-pp:R", "value": "1k", "x": x, "y": y})
            refs.append(t.split()[1])
        ra, rb, rc = refs
        err, t = m.tool("sch_connect", {"nets": {"NA": [ra + ".2", rb + ".1"]}})
        pins = {}
        for r in (rb, rc):
            err, t = m.tool("sch_part", {"ref": r})
            pins[r] = {n: (float(px), float(py)) for n, px, py in re.findall(r"^\s+(\S+) .*?@(-?[\d.]+),(-?[\d.]+)", t, re.M)}
        p, q = pins[rc]["1"], pins[rb]["2"]
        err, t = m.tool("sch_wire", {"points": [list(p), [p[0], q[1] + 7.62], [q[0], q[1] + 7.62], list(q)]})
        check(not err, "a real wire " + rc + ".1 -> " + rb + ".2")
        before = netlist(m)
        err, t = m.tool("sch_move", {"ref": rc, "dx": 7.62, "dy": 5.08})
        check(not err and "an L through" in t, "the dragged wire becomes an L: "
              + next((ln.strip() for ln in t.splitlines() if "wire to" in ln), t)[:120])
        check(netlist(m) == before, "nets unchanged")
        m.tool("sch_save", {})
        diag = [w for w in wires(os.path.join(ecc, "ecc83-pp.kicad_sch")) if w[0] != w[2] and w[1] != w[3]]
        check(not diag, f"no diagonal wire in the file {diag[:2]}")
        err, t = m.tool("sch_move", {"ref": ra, "dx": 2.54})
        check(not err and "wire(s) dragged" not in t, "a part with only stubs: they move with it, nothing dragged")
        # a T: a loose branch hanging off a wire's middle; dangling takes the branch, not the wire
        err, t = m.tool("sch_wire", {"points": [[q[0], q[1] + 7.62], [q[0] + 12.7, q[1] + 7.62]]})
        err, t = m.tool("sch_delete", {"dangling": True})
        check(not err and netlist(m) == before, "sch_delete dangling: nets unchanged (" + t.strip()[:60] + ")")
    finally:
        m.close()
        server.stop()

    print("\nRESULT:", "PASS" if not FAILS else f"FAIL ({len(FAILS)})")
    return 1 if FAILS else 0


if __name__ == "__main__":
    sys.exit(main())
