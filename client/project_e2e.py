"""A new project from MCP (project {new}), headless.

usage: python3 -I project_e2e.py --build BUILD --kicad KICAD_SRC --work DIR

The server starts on a copy of pic_programmer, then:
  new         a folder that is not there: made, <name>.kicad_pro / .kicad_pcb / .kicad_sch written by
              KiCad (saved: the board has its layers, the project its settings), opened (board_summary
              names it), 4 copper layers with a stackup, a 60 x 40 mm outline; the schematic tools
              work on it (a part added and saved)
  refused     the same again (nothing written over), a relative path, a bad name, a file as the folder,
              odd layer counts; nothing left behind by a refusal
  unsaved     an edit on the open board: a new project is refused (and the folder it would have made
              is not there), discard=true goes ahead
  stackup     stackup=JLC04161H-7628 sets the fab's stackup; a .kicad_pro path names the project
  pours       route to "zone" on the new 4-layer board: a GND pad to its plane on In1.Cu (a via at
              the end), a VCC pad to its pour beside it on F.Cu (no via); DRC finds them connected
  open_board  the first project opens again from disk
"""

import argparse
import json
import re
import os
import shutil
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from m5_e2e import FAILS, Mcp, Server, binaries, check  # noqa: E402


def new_project(m, args):
    """project {new: ...}, discard beside it (new_project's arguments)."""
    args = dict(args)
    call = {"new": args}
    if "discard" in args:
        call["discard"] = args.pop("discard")
    return m.tool("project", call)


def main():
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    ap = argparse.ArgumentParser()
    ap.add_argument("--build", required=True)
    ap.add_argument("--kicad", required=True)
    ap.add_argument("--work", required=True)
    a = ap.parse_args()
    work = os.path.abspath(a.work)
    shutil.rmtree(work, ignore_errors=True)
    os.makedirs(work)
    shutil.copytree(os.path.join(a.kicad, "demos", "pic_programmer"), os.path.join(work, "pic"))
    _, kicadd_mcp, env = binaries(os.path.abspath(a.build))
    sock = os.path.join(work, "s.sock")
    srv = Server(a.build, sock, os.path.join(work, "pic", "pic_programmer.kicad_pcb"), env, os.path.join(work, "server.log"))
    check(srv.start(), "api-server up on pic_programmer")
    m = Mcp([kicadd_mcp, "--socket", sock, "--stdio"], env)
    try:
        print("[new]")
        one = os.path.join(work, "projects", "alpha")
        err, t = new_project(m, {"path": one, "copper_layers": 4, "outline": [60, 40]})
        print("      ", "\n       ".join(t.splitlines()))
        files = {e: os.path.join(one, "alpha" + e) for e in (".kicad_pro", ".kicad_pcb", ".kicad_sch")}
        check(not err and all(os.path.isfile(f) for f in files.values()), "alpha.kicad_pro / .kicad_pcb / .kicad_sch written")
        check("file=alpha.kicad_pcb" in t and "cu_layers=4" in t, "opened: board_summary names it, 4 copper layers")
        check("outline: board 60 x 40 mm" in t, "the 60 x 40 mm outline")
        pcb = open(files[".kicad_pcb"], encoding="utf-8").read()
        check('(generator "pcbnew")' in pcb and '"In2.Cu"' in pcb and "(stackup" in pcb and "Edge.Cuts" in pcb,
              "the board file is KiCad's own (saved): 4 layers, a stackup, the outline")
        pro = json.load(open(files[".kicad_pro"], encoding="utf-8"))
        check("board" in pro and "net_settings" in pro, "the project file is KiCad's own (saved): board, net settings")
        # KiCadD ships no symbol library: one made in the project (lib_create_symbol), then placed
        m.tool("lib_create_symbol", {"name": "BUF1", "value": "BUF1", "pins": [
            {"number": "1", "name": "A", "type": "input"}, {"number": "2", "name": "Y", "type": "output"}]})
        err, t = m.tool("sch_add_part", {"lib_id": "kicadd:BUF1", "value": "BUF1", "x": 50.8, "y": 50.8})
        err2, t2 = m.tool("sch_save", {})
        check(not err and not err2 and "kicadd:BUF1" in open(files[".kicad_sch"], encoding="utf-8").read(),
              "the schematic tools work on the new one (a symbol made in the project, placed, saved)" + ("" if not err else ": " + t[:120]))
        err, t = m.tool("impedance", {"z": 50, "layers": ["F.Cu"]})
        check(not err and "F.Cu: width" in t, "the stackup is there for impedance")

        print("[refused]")
        before = sorted(os.listdir(one))
        for args, want in (({"path": one}, "is there already"),
                           ({"path": "relative/beta"}, "must be absolute"),
                           ({"path": os.path.join(work, "projects", "x"), "name": "a/b"}, "name:"),
                           ({"path": files[".kicad_pcb"]}, "a folder or a .kicad_pro"),
                           ({"path": os.path.join(work, "projects", "odd"), "copper_layers": 3}, "copper_layers")):
            err, t = new_project(m, args)
            check(err and want in t, f"refused: {want} ({t.strip()[:90]})")
        check(sorted(os.listdir(one)) == before and not os.path.exists(os.path.join(work, "projects", "odd"))
              and not os.path.exists(os.path.join(work, "projects", "x")), "nothing written by a refusal")

        print("[unsaved]")
        m.tool("board_outline", {"rect": [100, 100, 170, 150]})
        two = os.path.join(work, "projects", "beta")
        err, t = new_project(m, {"path": two})
        check(err and "unsaved changes" in t and not os.path.exists(two), "an unsaved edit: refused, beta not made")
        err, t = new_project(m, {"path": two, "discard": True})
        check(not err and "file=beta.kicad_pcb" in t and "cu_layers=2" in t, "discard=true: beta made and open (2 layers by default)")

        print("[stackup, a .kicad_pro path]")
        three = os.path.join(work, "projects", "gamma", "board-x.kicad_pro")
        err, t = new_project(m, {"path": three, "stackup": "JLC04161H-7628"})
        check(not err and "file=board-x.kicad_pcb" in t and os.path.isfile(three), "a .kicad_pro path: its name, its folder")
        err, t = m.tool("board_setup", {})
        check("JLC04161H-7628" in t or "7628" in t, "the fab's stackup set: " + t.splitlines()[0][:80])

        print("[route to a pour]")
        # on the 4-layer alpha: a capacitor GND / VCC, GND as a plane on In1.Cu, VCC as a pour on
        # F.Cu beside the part: GND reaches its plane through a via, VCC its pour on its own layer
        err, t = m.tool("project", {"open": files[".kicad_pro"], "discard": True})
        # the coordinates below are from the outline's corner at (100, 100); project new centres it
        g = re.search(r"outline=(-?[\d.]+),(-?[\d.]+)\.\.", t)
        if g:
            m.tool("board_transform", {"dx": round(100 - float(g.group(1)) - 0.05),
                                       "dy": round(100 - float(g.group(2)) - 0.05)})
        m.tool("sch_add_part", {"lib_id": "power:PWR_FLAG", "x": 101.6, "y": 50.8})
        m.tool("sch_connect", {"nets": {"VBUF": ["#FLG01.1", "U1.1"]}})
        err, t = m.tool("sch_move", {"ref": "#FLG01", "x": 127, "y": 76.2})
        err2, e = m.tool("run_erc", {"warnings": True})
        check(not err and "-> /VBUF" in t and "label_dangling" not in e,
              "sch_move of a PWR_FLAG: its stub and label go with it, still on VBUF: " + t.splitlines()[-1].strip()[:70])
        m.tool("sch_save", {})

        err, t = m.tool("lib_create_footprint", {"kind": "chip", "size": "0603", "prefix": "C"})
        fp = re.search(r"(kicadd:[\w.-]+)", t)
        check(not err and fp, "a 0603 footprint made: " + t.splitlines()[0][:80])
        err, t = m.tool("add_footprint", {"footprint": fp.group(1) if fp else "", "ref": "C1", "x": 120, "y": 120,
                                          "nets": {"1": "GND", "2": "VCC"}})
        check(not err, "C1 placed with GND / VCC" + ("" if not err else ": " + t.strip()[:160]))
        m.tool("zones", {"create": {"net": "GND", "layers": ["In1.Cu"]}})
        m.tool("zones", {"create": {"net": "VCC", "layers": ["F.Cu"], "rect": [125, 110, 140, 130]}})
        m.tool("refill_zones", {})
        err, t = m.tool("route", {"from": "C1.1", "to": "zone"})
        print("      ", t.strip().splitlines()[0][:160])
        check(not err and "pour In1.Cu" in t and "1 via(s)" in t, "C1.1 (GND) to its plane on In1.Cu: a via at the end")
        err, t = m.tool("route", {"from": "C1.2", "to": "zone"})
        print("      ", t.strip().splitlines()[0][:160])
        check(not err and "pour F.Cu" in t and "0 via(s)" in t, "C1.2 (VCC) to its pour on F.Cu: no via")
        err, t = m.tool("add_footprint", {"footprint": fp.group(1) if fp else "", "ref": "C2", "x": 112, "y": 135, "back": True,
                                          "nets": {"1": "SIG", "2": "VCC2"}})
        m.tool("add_footprint", {"footprint": fp.group(1) if fp else "", "ref": "C3", "x": 150, "y": 135, "nets": {"1": "SIG", "2": "VCC3"}})
        err, t = m.tool("route_short", {"nets": ["SIG"], "dry_run": True, "vias": False})
        err2, t2 = m.tool("route_short", {"nets": ["SIG"], "dry_run": True})
        check("0 would be routed" in t and "1 would be routed" in t2 and "1 via(s)" in t2,
              "route_short: C2 (bottom) to C3 (top): no route without vias, one via with them: " + t2.splitlines()[0][:70])
        m.tool("delete_footprints", {"refs": ["C2", "C3"]})
        m.tool("refill_zones", {})
        err, t = m.tool("run_drc", {})
        check("unconnected_items" not in t, "after a refill both are connected: " + t.splitlines()[0][:80])

        print("[open_board]")
        err, t = m.tool("project", {"open": files[".kicad_pro"], "discard": True})
        check(not err and "file=alpha.kicad_pcb" in t and "cu_layers=4" in t, "alpha opens again from disk")
    finally:
        m.close()
        srv.stop()

    print("\nRESULT: " + ("PASS" if not FAILS else "FAIL (%d)" % len(FAILS)))
    sys.exit(1 if FAILS else 0)


if __name__ == "__main__":
    main()
