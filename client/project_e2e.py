"""A new project from MCP (new_project), headless.

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
  open_board  the first project opens again from disk
"""

import argparse
import json
import os
import shutil
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from m5_e2e import FAILS, Mcp, Server, binaries, check  # noqa: E402


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
        err, t = m.tool("new_project", {"path": one, "copper_layers": 4, "outline": [60, 40]})
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
            err, t = m.tool("new_project", args)
            check(err and want in t, f"refused: {want} ({t.strip()[:90]})")
        check(sorted(os.listdir(one)) == before and not os.path.exists(os.path.join(work, "projects", "odd"))
              and not os.path.exists(os.path.join(work, "projects", "x")), "nothing written by a refusal")

        print("[unsaved]")
        m.tool("board_outline", {"rect": [100, 100, 170, 150]})
        two = os.path.join(work, "projects", "beta")
        err, t = m.tool("new_project", {"path": two})
        check(err and "unsaved changes" in t and not os.path.exists(two), "an unsaved edit: refused, beta not made")
        err, t = m.tool("new_project", {"path": two, "discard": True})
        check(not err and "file=beta.kicad_pcb" in t and "cu_layers=2" in t, "discard=true: beta made and open (2 layers by default)")

        print("[stackup, a .kicad_pro path]")
        three = os.path.join(work, "projects", "gamma", "board-x.kicad_pro")
        err, t = m.tool("new_project", {"path": three, "stackup": "JLC04161H-7628"})
        check(not err and "file=board-x.kicad_pcb" in t and os.path.isfile(three), "a .kicad_pro path: its name, its folder")
        err, t = m.tool("board_setup", {})
        check("JLC04161H-7628" in t or "7628" in t, "the fab's stackup set: " + t.splitlines()[0][:80])

        print("[open_board]")
        err, t = m.tool("open_board", {"path": files[".kicad_pro"]})
        check(not err and "file=alpha.kicad_pcb" in t and "cu_layers=4" in t, "alpha opens again from disk")
    finally:
        m.close()
        srv.stop()

    print("\nRESULT: " + ("PASS" if not FAILS else "FAIL (%d)" % len(FAILS)))
    sys.exit(1 if FAILS else 0)


if __name__ == "__main__":
    main()
