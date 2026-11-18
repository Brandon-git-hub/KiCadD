"""Tools no other test covers well (headless): open_board, flush_ratsnest.

usage: python3 -I tools_e2e.py --build BUILD --demos DEMOS_DIR --work DIR

  * flush_ratsnest: stickhub's GND (a large net) has its ratsnest deferred while its tracks are
    deleted; flush rebuilds it and gives the full unrouted count; after undo it is 0 again
  * open_board: errors (no path, relative, a missing file, not a board: the old board comes
    back); unsaved changes refused unless discard=true; another project by .kicad_pcb and by
    .kicad_pro, with its schematic and rules; the agent history starts over; the same file
    again reloads it
"""

import argparse
import os
import re
import shutil
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from m5_e2e import FAILS, Mcp, Server, binaries, check  # noqa: E402

ap = argparse.ArgumentParser()
ap.add_argument("--build", required=True)
ap.add_argument("--demos", required=True)
ap.add_argument("--work", required=True)
a = ap.parse_args()

build, work = os.path.abspath(a.build), os.path.abspath(a.work)
shutil.rmtree(work, ignore_errors=True)
shutil.copytree(os.path.join(a.demos, "stickhub"), os.path.join(work, "sh"))
shutil.copytree(os.path.join(a.demos, "pic_programmer"), os.path.join(work, "pic"))
stickhub = os.path.join(work, "sh", "StickHub.kicad_pcb")
pic = os.path.join(work, "pic", "pic_programmer.kicad_pcb")
pic_bytes = open(pic, "rb").read()
sock = os.path.join(work, "s.sock")
kicad_cli, kicadd_mcp, env = binaries(build)

srv = Server(build, sock, stickhub, env, os.path.join(work, "server.log"))
assert srv.start(), "api-server did not start"
try:
    m = Mcp([kicadd_mcp, "--socket", sock, "--stdio"], env)

    print("[flush_ratsnest]")
    err, t = m.tool("delete_tracks", {"nets": ["GND"]})
    check(not err and "excl. deferred nets: GND" in t, "GND's ratsnest deferred after delete_tracks")
    err, t = m.tool("flush_ratsnest")
    n = re.search(r"rebuilt (\d+) deferred nets; unconnected=(\d+)", t)
    print("      ", t.strip())
    check(not err and n and int(n.group(1)) >= 1 and int(n.group(2)) > 0, "flush: GND rebuilt, its pads unrouted")
    err, t = m.tool("flush_ratsnest")
    check(not err and t.startswith("rebuilt 0 deferred nets"), "flush again: nothing deferred")
    m.tool("undo")
    err, t = m.tool("flush_ratsnest")
    check(not err and t.strip().endswith("unconnected=0"), "after undo: routed again, unconnected=0")

    print("[open_board]")
    err, t = m.tool("open_board", {})
    check(err and "path" in t, "no path: refused")
    err, t = m.tool("open_board", {"path": os.path.join(work, "nope.kicad_pcb")})
    check(err and "no such board" in t, "a missing file: refused: " + t.strip()[:100])
    err, t = m.tool("open_board", {"path": "pic/pic_programmer.kicad_pcb"})
    check(err and "absolute" in t, "a relative path: refused")
    err, t = m.tool("open_board", {"path": pic})
    check(err and "unsaved changes" in t and "StickHub.kicad_pcb" in t, "unsaved changes (the flush / undo above): refused")
    junk = os.path.join(work, "junk.kicad_pcb")
    open(junk, "w").write("this is not a board\n")
    err, t = m.tool("open_board", {"path": junk, "discard": True})
    print("      ", t.strip()[:200])
    check(err and "could not open" in t and "StickHub.kicad_pcb is open again" in t, "not a board: refused, stickhub reopened")
    err, t = m.tool("board_summary")
    check(not err and "footprints=94" in t, "stickhub is open")
    err, t = m.tool("board_diff")
    check(not err and t.startswith("no changes against"), "as saved (reopened from disk)")

    err, t = m.tool("move_footprints", {"refs": ["U1"], "dx": 1})
    check(not err, "an unsaved edit on stickhub")
    err, t = m.tool("open_board", {"path": pic, "discard": True})
    print("      ", t.splitlines()[0][:150])
    check(not err and "footprints=63" in t and "unsaved changes of StickHub.kicad_pcb were dropped" in t,
          "pic_programmer opened by .kicad_pcb, the drop said")
    err, t = m.tool("undo")
    check(not err and t.startswith("nothing to undo"), "a fresh history: " + t.splitlines()[0])
    err, t = m.tool("board_diff")
    check(not err and t.startswith("no changes against") and "pic_programmer.kicad_pcb" in t, "board_diff: against its own file")
    err, t = m.tool("sch_netlist")
    check(not err and "VPP" in t, "its schematic opened with it (sch_netlist)")
    err, t = m.tool("get_rules")
    check(not err and "POWER" in t, "its project's net classes (POWER)")
    m.tool("move_footprints", {"refs": ["R1"], "dx": 1})

    err, t = m.tool("open_board", {"path": os.path.join(work, "sh", "StickHub.kicad_pro"), "discard": True})
    check(not err and "footprints=94" in t, "stickhub opened by .kicad_pro")
    err, t = m.tool("board_diff")
    check(not err and t.startswith("no changes against"), "stickhub as saved: the unsaved U1 move went with the old board")
    check(open(pic, "rb").read() == pic_bytes, "pic_programmer's file untouched by the unsaved R1 move")
    err, t = m.tool("get_rules")
    check(not err and "POWER" not in t, "stickhub's rules, not pic_programmer's")
    m.tool("move_footprints", {"refs": ["U1"], "dx": 1})
    err, t = m.tool("open_board", {"path": stickhub, "discard": True})
    err, t2 = m.tool("board_diff")
    check(not err and t2.startswith("no changes against"), "the same file again: reloaded from disk")
    m.close()
finally:
    srv.stop()

print("\nRESULT:", "PASS" if not FAILS else f"FAIL ({len(FAILS)})")
sys.exit(1 if FAILS else 0)
